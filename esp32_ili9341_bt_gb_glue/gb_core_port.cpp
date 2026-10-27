#include "gb_core_port.h"
#include "config.h"

#include <Arduino.h>
#include <esp_partition.h>

static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);

#define ENABLE_SOUND 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#include "peanut_gb.h"

static const uint8_t* romData = nullptr;
static size_t romFileSize = 0;
static spi_flash_mmap_handle_t romMmapHandle = 0;
static bool romMapped = false;
static const esp_partition_t* romPart = nullptr;
static uint8_t* cartRam = nullptr;
static size_t cartRamSize = 0;
static gb_s* gb = nullptr;
static bool emulatorReady = false;
static bool frameRendered = false;
static bool saveDirty = false;
static bool saveFlushing = false;
static uint8_t saveEvent = 0;
static uint32_t lastSaveFlushMs = 0;
static uint32_t saveSequence = 0;
static uint32_t frameNo = 0;

struct ToneChannel {
  uint16_t gbFreq = 0;
  uint8_t volume = 0;
  bool active = false;
  uint32_t silenceAtMs = 0;
};

static uint8_t audioRegs[0x30] = {};
static ToneChannel audioTone[2];
static uint16_t currentAudioHz = 0;
static uint8_t currentAudioDuty = 0;
static bool audioMasterEnabled = false;
static constexpr uint8_t AUDIO_FIXED_DUTY = 128;
static bool audioStarted = false;

static constexpr uint32_t SAVE_MAGIC = 0x45564153; // SAVE
static constexpr uint32_t SAVE_GUARD_MAGIC = 0x53475632; // SGV2
static const esp_partition_t* savePart = nullptr;

struct SaveHeader {
  uint32_t magic;
  uint32_t size;
  uint32_t checksum;
  uint32_t sequence;
  uint32_t guard;
};

struct SaveCandidate {
  bool valid;
  uint32_t sequence;
  size_t offset;
};

static uint16_t gbToneToHz(uint16_t gbFreq) {
  if (gbFreq >= 2048) {
    return 0;
  }

  uint32_t hz = 131072UL / (2048UL - gbFreq);
  if (hz < 32) {
    hz = 32;
  } else if (hz > 6000) {
    hz = 6000;
  }
  return static_cast<uint16_t>(hz);
}

static uint8_t envelopeVolume(uint8_t reg) {
  return (reg >> 4) & 0x0F;
}

static void applyAudioTone() {
  ToneChannel* tone = nullptr;
  const uint32_t now = millis();
  for (int i = 1; i >= 0; --i) {
    const bool lengthOk = audioTone[i].silenceAtMs == 0 || now < audioTone[i].silenceAtMs;
    if (audioTone[i].active && audioTone[i].volume && lengthOk) {
      tone = &audioTone[i];
      break;
    }
  }

  if (!audioMasterEnabled || !tone) {
    if (currentAudioDuty != 0) {
      ledcWrite(AUDIO_PWM_CHANNEL, 0);
      currentAudioHz = 0;
      currentAudioDuty = 0;
    }
    return;
  }

  const uint16_t hz = gbToneToHz(tone->gbFreq);
  if (!hz) {
    ledcWrite(AUDIO_PWM_CHANNEL, 0);
    currentAudioHz = 0;
    currentAudioDuty = 0;
    return;
  }

  const uint8_t duty = AUDIO_FIXED_DUTY;
  if (hz != currentAudioHz) {
    ledcWriteTone(AUDIO_PWM_CHANNEL, hz);
    currentAudioHz = hz;
    currentAudioDuty = 0;
  }
  if (duty != currentAudioDuty) {
    ledcWrite(AUDIO_PWM_CHANNEL, duty);
    currentAudioDuty = duty;
  }
}

static void updateToneChannel(uint8_t channel, bool triggered) {
  if (channel > 1) {
    return;
  }

  const uint8_t base = channel == 0 ? 0 : 5;
  const uint8_t volumeReg = audioRegs[base + 2];
  const uint16_t gbFreq = audioRegs[base + 3] | ((audioRegs[base + 4] & 0x07) << 8);
  const uint16_t hz = gbToneToHz(gbFreq);
  audioTone[channel].gbFreq = gbFreq;
  audioTone[channel].volume = envelopeVolume(volumeReg);

  if (triggered && audioTone[channel].volume && hz) {
    audioTone[channel].active = true;
    const bool lengthEnabled = audioRegs[base + 4] & 0x40;
    const uint8_t lengthData = audioRegs[base + 1] & 0x3F;
    uint16_t holdMs = 0;
    if (lengthEnabled) {
      const uint16_t calculatedMs = (64 - lengthData) * 1000UL / 256UL;
      holdMs = calculatedMs < 45 ? 45 : calculatedMs;
    }
    audioTone[channel].silenceAtMs = holdMs ? millis() + holdMs : 0;
  } else if (!audioTone[channel].volume || !hz) {
    audioTone[channel].active = false;
  }

  applyAudioTone();
}

static uint8_t audio_read(uint16_t addr) {
  if (addr < 0xFF10 || addr > 0xFF3F) {
    return 0xFF;
  }

  static const uint8_t readMask[] = {
    0x80, 0x3f, 0x00, 0xff, 0xbf,
    0xff, 0x3f, 0x00, 0xff, 0xbf,
    0x7f, 0xff, 0x9f, 0xff, 0xbf,
    0xff, 0xff, 0x00, 0x00, 0xbf,
    0x00, 0x00, 0x70,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  const uint8_t index = addr - 0xFF10;
  uint8_t value = audioRegs[index] | readMask[index];
  if (addr == 0xFF26) {
    value = audioMasterEnabled ? (value | 0x80) : (value & ~0x80);
  }
  return value;
}

static void audio_write(uint16_t addr, uint8_t val) {
  if (addr < 0xFF10 || addr > 0xFF3F) {
    return;
  }

  const uint8_t index = addr - 0xFF10;
  audioRegs[index] = val;

  if (addr == 0xFF26) {
    audioMasterEnabled = val & 0x80;
    if (!audioMasterEnabled) {
      gbCoreAudioSilence();
    }
    return;
  }

  if (!audioMasterEnabled && addr != 0xFF26) {
    return;
  }

  if (addr >= 0xFF12 && addr <= 0xFF14) {
    updateToneChannel(0, addr == 0xFF14 && (val & 0x80));
  } else if (addr >= 0xFF17 && addr <= 0xFF19) {
    updateToneChannel(1, addr == 0xFF19 && (val & 0x80));
  }
}

static uint32_t saveChecksum(const uint8_t* data, size_t size) {
  uint32_t sum = 0x12345678;
  for (size_t i = 0; i < size; ++i) {
    sum = (sum << 5) | (sum >> 27);
    sum ^= data[i];
  }
  return sum;
}

static size_t romSizeFromHeader() {
  static const size_t sizes[] = {
    32UL * 1024, 64UL * 1024, 128UL * 1024, 256UL * 1024,
    512UL * 1024, 1024UL * 1024, 2UL * 1024 * 1024,
    4UL * 1024 * 1024, 8UL * 1024 * 1024
  };
  const uint8_t code = romData ? romData[0x0148] : 0xFF;
  if (code < sizeof(sizes) / sizeof(sizes[0])) {
    return sizes[code];
  }
  return 1024UL * 1024;
}

static size_t saveOffsetInRomPartition() {
  return (romFileSize + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
}

static bool validateSaveSlot(size_t offset, SaveCandidate* candidate) {
  SaveHeader header = {};
  candidate->valid = false;
  candidate->sequence = 0;
  candidate->offset = offset;

  if (!savePart || !cartRam || savePart->size < offset + 0x10000) {
    return false;
  }

  esp_partition_read(savePart, offset, &header, sizeof(header));
  if (header.magic != SAVE_MAGIC || header.size != cartRamSize) {
    return false;
  }

  const bool newFormat = header.guard == SAVE_GUARD_MAGIC;
  const size_t dataOffset = newFormat ? sizeof(SaveHeader) : 12;
  uint8_t* temp = static_cast<uint8_t*>(malloc(cartRamSize));
  if (!temp) {
    return false;
  }

  esp_partition_read(savePart, offset + dataOffset, temp, cartRamSize);
  const bool checksumOk = header.checksum == saveChecksum(temp, cartRamSize);
  free(temp);

  if (!checksumOk) {
    return false;
  }

  candidate->valid = true;
  candidate->sequence = newFormat ? header.sequence : 0;
  return true;
}

static bool restoreBestSave() {
  const size_t base = saveOffsetInRomPartition();
  SaveCandidate a = {};
  SaveCandidate b = {};
  validateSaveSlot(base, &a);
  validateSaveSlot(base + 0x10000, &b);

  if (!a.valid && !b.valid) {
    return false;
  }

  const SaveCandidate chosen = (!b.valid || (a.valid && a.sequence >= b.sequence)) ? a : b;
  SaveHeader header = {};
  esp_partition_read(savePart, chosen.offset, &header, sizeof(header));
  const size_t dataOffset = (header.guard == SAVE_GUARD_MAGIC) ? sizeof(SaveHeader) : 12;
  esp_partition_read(savePart, chosen.offset + dataOffset, cartRam, cartRamSize);
  saveSequence = chosen.sequence;
  saveDirty = false;
  Serial.printf("Save restored: %u bytes seq=%u\n",
                static_cast<unsigned>(cartRamSize),
                static_cast<unsigned>(saveSequence));
  return true;
}

static uint8_t romRead(gb_s*, const uint_fast32_t addr) {
  if (!romData || addr >= romFileSize) {
    return 0xFF;
  }
  return romData[addr];
}

static uint8_t cartRamRead(gb_s*, const uint_fast32_t addr) {
  if (!cartRam || addr >= cartRamSize) {
    return 0xFF;
  }
  return cartRam[addr];
}

static void cartRamWrite(gb_s*, const uint_fast32_t addr, const uint8_t val) {
  if (cartRam && addr < cartRamSize) {
    if (cartRam[addr] != val) {
      cartRam[addr] = val;
      saveDirty = true;
    }
  }
}

static bool flushSaveNow(bool showEvent) {
  if (!saveDirty || !cartRam) {
    return false;
  }

  saveFlushing = true;
  if (showEvent) {
    saveEvent = 1;
  }
  const size_t saveOffset = saveOffsetInRomPartition();
  const uint32_t nextSequence = saveSequence + 1;
  const size_t slotOffset = saveOffset + ((nextSequence & 1) ? 0x10000 : 0);
  if (savePart && savePart->size >= slotOffset + 0x10000) {
    SaveHeader header = {
      SAVE_MAGIC,
      static_cast<uint32_t>(cartRamSize),
      saveChecksum(cartRam, cartRamSize),
      nextSequence,
      SAVE_GUARD_MAGIC
    };
    esp_partition_erase_range(savePart, slotOffset, 0x10000);
    esp_partition_write(savePart, slotOffset, &header, sizeof(header));
    esp_partition_write(savePart, slotOffset + sizeof(header), cartRam, cartRamSize);
    saveDirty = false;
    saveSequence = nextSequence;
    lastSaveFlushMs = millis();
    saveFlushing = false;
    if (showEvent) {
      saveEvent = 2;
    }
    Serial.println("Save flushed");
    return true;
  }

  lastSaveFlushMs = millis();
  saveFlushing = false;
  if (showEvent) {
    saveEvent = 3;
  }
  Serial.println("Save flush failed");
  return false;
}

static void gbError(gb_s*, const gb_error_e error, const uint16_t addr) {
  Serial.printf("Peanut-GB error %d at 0x%04x\n", error, addr);
}

static void lcdDrawLine(gb_s* ctx, const uint8_t* pixels, const uint_fast8_t line) {
  uint16_t* frame = static_cast<uint16_t*>(ctx->direct.priv);
  if (!frame || line >= GB_HEIGHT) {
    return;
  }
  frameRendered = true;

  static const uint16_t palette[4] = {
    0xFFFF,  // white
    0xC618,  // light gray
    0x7BEF,  // dark gray
    0x0000   // black
  };

  uint16_t* dst = frame + (line * GB_WIDTH);
  for (int x = 0; x < GB_WIDTH; ++x) {
    dst[x] = palette[pixels[x] & LCD_COLOUR];
  }
}

bool gbCoreLoadRom() {
  emulatorReady = false;

  romPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!romPart) {
    Serial.println("ROM partition missing");
    return false;
  }

  if (!romData) {
    const void* mapped = nullptr;
    esp_err_t err = esp_partition_mmap(romPart, 0, romPart->size, SPI_FLASH_MMAP_DATA,
                                       &mapped, &romMmapHandle);
    if (err != ESP_OK || !mapped) {
      Serial.printf("ROM mmap failed: %d\n", err);
      return false;
    }
    romData = static_cast<const uint8_t*>(mapped);
    romMapped = true;
    romFileSize = romSizeFromHeader();
  }

  if (romFileSize < 0x150 || romData[0x0104] == 0xFF || romData[0x0134] == 0xFF) {
    Serial.println("Raw ROM missing at filesystem partition start");
    return false;
  }
  if (romData[0x0143] == 0xC0) {
    Serial.println("GBC-only ROM is not supported on this build");
    return false;
  }

  if (!gb) {
    gb = static_cast<gb_s*>(calloc(1, sizeof(gb_s)));
    if (!gb) {
      Serial.println("No RAM for Peanut-GB context");
      return false;
    }
  }

  const gb_init_error_e err = gb_init(gb, romRead, cartRamRead, cartRamWrite, gbError, nullptr);
  if (err != GB_INIT_NO_ERROR) {
    Serial.printf("Peanut-GB init failed: %d\n", err);
    return false;
  }

  __gb_write(gb, 0xFF47, 0xE4);  // BG: white, light gray, dark gray, black
  __gb_write(gb, 0xFF48, 0xE4);  // OBJ0
  __gb_write(gb, 0xFF49, 0xE4);  // OBJ1

  size_t saveSize = 0;
  if (gb_get_save_size_s(gb, &saveSize) == 0 && saveSize > 0) {
    Serial.printf("Cart RAM: %u bytes\n", static_cast<unsigned>(saveSize));
    if (cartRamSize != saveSize) {
      free(cartRam);
      cartRam = static_cast<uint8_t*>(calloc(1, saveSize));
      cartRamSize = cartRam ? saveSize : 0;
    }
    if (cartRam) {
      savePart = romPart;
      const size_t saveOffset = saveOffsetInRomPartition();
      if (savePart && savePart->size >= saveOffset + 0x20000) {
        if (!restoreBestSave()) {
          Serial.println("No existing save");
        } else {
          saveDirty = false;
        }
      } else {
        Serial.println("No room for save slot");
      }
    }
  }

  gb_init_lcd(gb, lcdDrawLine);
  gb->direct.frame_skip = true;
  Serial.println("Peanut-GB ready");
  emulatorReady = true;
  return true;
}

void gbCoreUnloadRom() {
  flushSaveNow(false);
  gbCoreAudioSilence();
  emulatorReady = false;
  frameRendered = false;

  if (romMapped) {
    spi_flash_munmap(romMmapHandle);
    romMapped = false;
    romMmapHandle = 0;
  }

  romData = nullptr;
  romFileSize = 0;
  romPart = nullptr;
  savePart = nullptr;

  free(cartRam);
  cartRam = nullptr;
  cartRamSize = 0;

  free(gb);
  gb = nullptr;

  saveDirty = false;
  saveFlushing = false;
  saveEvent = 0;
  saveSequence = 0;
  frameNo = 0;
}

void gbCoreSetButtons(uint16_t buttons) {
  if (!gb) {
    return;
  }

  uint8_t joypad = 0xFF;
  if (buttons & GB_BTN_A) joypad &= ~JOYPAD_A;
  if (buttons & GB_BTN_B) joypad &= ~JOYPAD_B;
  if (buttons & GB_BTN_SELECT) joypad &= ~JOYPAD_SELECT;
  if (buttons & GB_BTN_START) joypad &= ~JOYPAD_START;
  if (buttons & GB_BTN_RIGHT) joypad &= ~JOYPAD_RIGHT;
  if (buttons & GB_BTN_LEFT) joypad &= ~JOYPAD_LEFT;
  if (buttons & GB_BTN_UP) joypad &= ~JOYPAD_UP;
  if (buttons & GB_BTN_DOWN) joypad &= ~JOYPAD_DOWN;
  gb->direct.joypad = joypad;
}

bool gbCoreRunFrame(uint16_t* rgb565Frame) {
  if (emulatorReady && gb) {
    frameRendered = false;
    gb->direct.priv = rgb565Frame;
    gb_run_frame(gb);
    return frameRendered;
  }

  const uint16_t bg = 0x001F;
  const uint16_t fg = 0xFFE0;
  const int barX = (frameNo * 2) % GB_WIDTH;

  for (int y = 0; y < GB_HEIGHT; ++y) {
    for (int x = 0; x < GB_WIDTH; ++x) {
      bool grid = ((x / 8) + (y / 8)) & 1;
      bool bar = x >= barX && x < barX + 8;
      rgb565Frame[y * GB_WIDTH + x] = bar ? fg : (grid ? bg : 0x0000);
    }
  }

  frameNo++;
  return true;
}

bool gbCoreAudioBegin() {
  if (audioStarted) {
    return true;
  }

  ledcSetup(AUDIO_PWM_CHANNEL, 2000, AUDIO_PWM_RESOLUTION);
  ledcAttachPin(AUDIO_PIN, AUDIO_PWM_CHANNEL);
  ledcWrite(AUDIO_PWM_CHANNEL, 0);
  audioStarted = true;
  return true;
}

void gbCoreAudioTick() {
  const uint32_t now = millis();
  bool changed = false;
  for (int i = 0; i < 2; ++i) {
    if (audioTone[i].active && audioTone[i].silenceAtMs && now >= audioTone[i].silenceAtMs) {
      audioTone[i].active = false;
      changed = true;
    }
  }
  if (changed) {
    applyAudioTone();
  }
}

void gbCoreAudioSilence() {
  for (int i = 0; i < 2; ++i) {
    audioTone[i].active = false;
  }
  ledcWrite(AUDIO_PWM_CHANNEL, 0);
  currentAudioHz = 0;
  currentAudioDuty = 0;
}

void gbCoreAudioRefresh() {
  ledcSetup(AUDIO_PWM_CHANNEL, 2000, AUDIO_PWM_RESOLUTION);
  ledcAttachPin(AUDIO_PIN, AUDIO_PWM_CHANNEL);
  currentAudioHz = 0;
  currentAudioDuty = 0;
  applyAudioTone();
}

bool gbCoreSaveDirty() {
  return saveDirty || saveFlushing;
}

bool gbCoreFlushSave() {
  return flushSaveNow(true);
}

uint8_t gbCoreConsumeSaveEvent() {
  const uint8_t event = saveEvent;
  saveEvent = 0;
  return event;
}
