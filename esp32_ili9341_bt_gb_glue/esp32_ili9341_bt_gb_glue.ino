#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Bluepad32.h>
#include <SPI.h>
#include <SD.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <Preferences.h>
#include <driver/i2s.h>

#include "config.h"
#include "gb_core_port.h"

TFT_eSPI tft;
SPIClass sdSpi(HSPI);
Preferences prefs;

static ControllerPtr gamepads[BP32_MAX_GAMEPADS];
static uint16_t* gbFrame = nullptr;
static uint16_t scaledLine[320];
static uint32_t fpsWindowStart = 0;
static uint16_t fpsFrames = 0;
static uint16_t currentFps = 0;
static uint16_t filteredButtons = 0;
static uint16_t lastPhysicalButtons = 0;
static uint32_t buttonDownAt[8] = {};
static uint32_t lastRepeatAt[8] = {};

static constexpr uint32_t AB_TAP_MS = 55;

enum DisplayMode : uint8_t {
  DISPLAY_BIG,
  DISPLAY_BALANCED,
  DISPLAY_FAST,
  DISPLAY_MODE_COUNT,
};

enum SystemAction : uint8_t {
  SYS_RESUME,
  SYS_CHANGE_GAME,
  SYS_SAVE,
  SYS_EJECT_SD,
  SYS_SLEEP,
  SYS_ACTION_COUNT,
};

struct DisplayRect {
  int x;
  int y;
  int w;
  int h;
};

static DisplayMode displayMode = DISPLAY_BIG;
static DisplayRect frameRect = {40, 16, 240, 216};
static uint8_t brightnessLevel = 4;
static const uint8_t brightnessValues[] = {32, 64, 96, 144, 200, 255};
static uint16_t previousButtons = 0;
static bool controllerLShoulderPressed = false;
static bool previousLShoulderPressed = false;
static bool controllerRShoulderPressed = false;
static bool previousRShoulderPressed = false;
static bool controllerLTriggerPressed = false;
static bool previousLTriggerPressed = false;
static bool controllerRTriggerPressed = false;
static bool previousRTriggerPressed = false;
static bool controllerThumbRPressed = false;
static bool previousThumbRPressed = false;
static uint32_t statusUntil = 0;
static char statusText[20] = "";
static char lastMarginFps[12] = "";
static char lastMarginStatus[20] = "";
static uint8_t lastQuoteIndex = 0xFF;
static bool softSleeping = false;
static uint32_t sessionStartMs = 0;
static uint32_t softSleepStartedMs = 0;
static uint32_t sessionSleepTotalMs = 0;
static uint32_t lastDrawHash = 0;
static uint16_t lastDrawFps = 0xFFFF;
static DisplayMode lastDrawMode = DISPLAY_MODE_COUNT;
static bool lastFullStatusVisible = false;
static bool sdReady = false;
static bool sdBusStarted = false;
static char currentGameKey[48] = "";
static bool menuMusicActive = false;
static bool menuWavActive = false;
static File menuWavFile;
static uint32_t menuWavDataStart = 0;
static uint32_t menuWavDataEnd = 0;
static uint32_t menuWavReadPos = 0;
static uint32_t menuWavSampleRate = 11025;
static uint8_t menuWavChannels = 1;
static uint8_t menuWavBits = 8;
static uint8_t menuWavBuffer[2048];
static size_t menuWavBufferPos = 0;
static size_t menuWavBufferLen = 0;
static bool menuI2sStarted = false;
static uint16_t menuI2sBuffer[256];
static size_t menuI2sPendingOffset = 0;
static size_t menuI2sPendingBytes = 0;

static constexpr int TRIGGER_DEADZONE = 384;
static constexpr uint32_t QUOTE_INTERVAL_MS = 60000UL;
static constexpr size_t SAVE_SLOT_BYTES = 0x10000;
static constexpr uint32_t SAVE_MAGIC = 0x45564153; // SAVE
static constexpr uint32_t SAVE_GUARD_MAGIC = 0x53475632; // SGV2
static constexpr uint8_t MAX_ROMS = 24;
static constexpr bool AUTO_IMPORT_SD_SAVES = true;
static constexpr int COVER_W = 128;
static constexpr int COVER_H = 128;
static constexpr uint8_t MENU_WAV_VOLUME_PERCENT = 55;

struct SaveHeader {
  uint32_t magic;
  uint32_t size;
  uint32_t checksum;
  uint32_t sequence;
  uint32_t guard;
};

struct RomEntry {
  char title[48];
  char key[48];
  char romPath[96];
  char coverPath[96];
  char rawCoverPath[96];
  char savePath[96];
  size_t romSize;
};

static RomEntry romEntries[MAX_ROMS];
static uint8_t romCount = 0;
static uint8_t coverRowBgr[COVER_W * 3];
static uint16_t coverLine565[COVER_W];

static bool initSdCard(bool createDirs);
static void closeSdCard();

static void showStatus(const char* line1, const char* line2 = nullptr) {
  Serial.print(line1);
  if (line2) {
    Serial.print(" - ");
    Serial.print(line2);
  }
  Serial.println();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, TFT_WIDTH / 2, TFT_HEIGHT / 2 - 12, 2);
  if (line2) {
    tft.drawString(line2, TFT_WIDTH / 2, TFT_HEIGHT / 2 + 12, 2);
  }
}

static void drawLoadingStatus(const char* line1, const char* line2 = nullptr, int percent = -1) {
  static uint8_t spinnerIndex = 0;
  static const char spinner[] = "|/-\\";
  Serial.print(line1);
  if (line2) {
    Serial.print(" - ");
    Serial.print(line2);
  }
  if (percent >= 0) {
    Serial.print(" ");
    Serial.print(percent);
    Serial.print("%");
  }
  Serial.println();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line1, TFT_WIDTH / 2, 70, 2);
  if (line2) {
    tft.setTextColor(tft.color565(190, 220, 255), TFT_BLACK);
    tft.drawString(line2, TFT_WIDTH / 2, 98, 2);
  }

  const int barX = 42;
  const int barY = 132;
  const int barW = 236;
  const int barH = 14;
  tft.drawRect(barX, barY, barW, barH, tft.color565(80, 120, 160));
  if (percent >= 0) {
    const int fillW = max(0, min(barW - 4, (barW - 4) * percent / 100));
    tft.fillRect(barX + 2, barY + 2, fillW, barH - 4, tft.color565(80, 190, 120));
  } else {
    const int pulseW = 42;
    const int pulseX = barX + 2 + ((spinnerIndex * 17) % (barW - pulseW - 4));
    tft.fillRect(pulseX, barY + 2, pulseW, barH - 4, tft.color565(80, 190, 120));
  }

  char spinText[2] = {spinner[spinnerIndex++ & 3], '\0'};
  tft.setTextColor(tft.color565(220, 220, 180), TFT_BLACK);
  tft.drawString(spinText, TFT_WIDTH / 2, 168, 2);
}

static void setOverlayStatus(const char* text, uint32_t durationMs) {
  strncpy(statusText, text, sizeof(statusText) - 1);
  statusText[sizeof(statusText) - 1] = '\0';
  statusUntil = millis() + durationMs;
  lastDrawHash = 0;
  lastDrawFps = 0xFFFF;
  lastDrawMode = DISPLAY_MODE_COUNT;
  lastMarginStatus[0] = '\0';
}

static bool readExact(File& file, uint8_t* out, size_t len) {
  return file.read(out, len) == static_cast<int>(len);
}

static uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

static void stopMenuI2s() {
  if (!menuI2sStarted) {
    return;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
  i2s_driver_uninstall(I2S_NUM_0);
  menuI2sStarted = false;
  menuI2sPendingOffset = 0;
  menuI2sPendingBytes = 0;
}

static bool startMenuI2s(uint32_t sampleRate) {
  stopMenuI2s();

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 512;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  if (i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  menuI2sStarted = true;
  return true;
}

static void closeMenuWav() {
  if (menuWavFile) {
    menuWavFile.close();
  }
  menuWavActive = false;
  stopMenuI2s();
  menuWavDataStart = 0;
  menuWavDataEnd = 0;
  menuWavReadPos = 0;
  menuWavBufferPos = 0;
  menuWavBufferLen = 0;
}

static bool readMenuWavHeader(File& file) {
  uint8_t header[12];
  if (!file.seek(0) || !readExact(file, header, sizeof(header)) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool foundFmt = false;
  bool foundData = false;
  uint16_t audioFormat = 0;
  uint32_t sampleRate = 0;
  uint32_t dataStart = 0;
  uint32_t dataSize = 0;
  uint8_t channels = 0;
  uint8_t bits = 0;

  while (file.position() + 8 <= file.size()) {
    uint8_t chunk[8];
    if (!readExact(file, chunk, sizeof(chunk))) {
      break;
    }

    const uint32_t chunkSize = readLe32(chunk + 4);
    const uint32_t chunkData = file.position();
    const uint32_t nextChunk = chunkData + chunkSize + (chunkSize & 1);
    if (nextChunk < chunkData || nextChunk > file.size() + 1) {
      return false;
    }

    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (chunkSize < sizeof(fmt) || !readExact(file, fmt, sizeof(fmt))) {
        return false;
      }
      audioFormat = readLe16(fmt);
      channels = static_cast<uint8_t>(readLe16(fmt + 2));
      sampleRate = readLe32(fmt + 4);
      bits = static_cast<uint8_t>(readLe16(fmt + 14));
      foundFmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      dataStart = chunkData;
      dataSize = chunkSize;
      foundData = true;
    }

    if (!file.seek(nextChunk)) {
      return false;
    }
  }

  if (!foundFmt || !foundData || audioFormat != 1 || dataSize == 0) {
    return false;
  }
  if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16)) {
    return false;
  }
  if (sampleRate < 8000 || sampleRate > 22050) {
    return false;
  }

  menuWavChannels = channels;
  menuWavBits = bits;
  menuWavSampleRate = sampleRate;
  menuWavDataStart = dataStart;
  menuWavDataEnd = dataStart + dataSize;
  menuWavReadPos = menuWavDataStart;
  return file.seek(menuWavReadPos);
}

static bool seekMenuWavReadPos(uint32_t pos) {
  if (pos >= menuWavDataEnd) {
    pos = menuWavDataStart;
  }

  if (menuWavFile && menuWavFile.seek(pos)) {
    menuWavReadPos = pos;
    return true;
  }

  if (menuWavFile) {
    menuWavFile.close();
  }
  menuWavFile = SD.open(MENU_WAV_PATH, FILE_READ);
  if (!menuWavFile || !menuWavFile.seek(pos)) {
    return false;
  }

  menuWavReadPos = pos;
  return true;
}

static bool refillMenuWavBuffer() {
  if (!menuWavActive || menuWavDataStart >= menuWavDataEnd) {
    return false;
  }

  if (menuWavReadPos >= menuWavDataEnd) {
    menuWavReadPos = menuWavDataStart;
  }

  if (!menuWavFile || menuWavFile.position() != menuWavReadPos) {
    if (!seekMenuWavReadPos(menuWavReadPos)) {
      return false;
    }
  }

  const uint32_t remaining = menuWavDataEnd - menuWavReadPos;
  const size_t want = remaining > sizeof(menuWavBuffer) ? sizeof(menuWavBuffer) : remaining;
  const int got = menuWavFile.read(menuWavBuffer, want);
  if (got <= 0) {
    if (!seekMenuWavReadPos(menuWavDataStart)) {
      return false;
    }
    const uint32_t retryRemaining = menuWavDataEnd - menuWavDataStart;
    const size_t retryWant = retryRemaining > sizeof(menuWavBuffer)
                                 ? sizeof(menuWavBuffer)
                                 : static_cast<size_t>(retryRemaining);
    const int retry = menuWavFile.read(menuWavBuffer, retryWant);
    if (retry <= 0) {
      return false;
    }
    menuWavBufferPos = 0;
    menuWavBufferLen = static_cast<size_t>(retry);
    menuWavReadPos += retry;
    return true;
  }

  menuWavBufferPos = 0;
  menuWavBufferLen = static_cast<size_t>(got);
  menuWavReadPos += got;
  return true;
}

static bool readMenuWavByte(uint8_t* out) {
  if (menuWavBufferPos >= menuWavBufferLen && !refillMenuWavBuffer()) {
    if (!seekMenuWavReadPos(menuWavDataStart)) {
      return false;
    }
    menuWavBufferPos = 0;
    menuWavBufferLen = 0;
    if (!refillMenuWavBuffer()) {
      return false;
    }
  }

  *out = menuWavBuffer[menuWavBufferPos++];
  return true;
}

static bool readMenuWavSample(uint8_t* outDuty) {
  int32_t mixed = 0;

  for (uint8_t channel = 0; channel < menuWavChannels; ++channel) {
    if (menuWavBits == 8) {
      uint8_t sample = 128;
      if (!readMenuWavByte(&sample)) {
        return false;
      }
      mixed += static_cast<int32_t>(sample) - 128;
    } else {
      uint8_t lo = 0;
      uint8_t hi = 0;
      if (!readMenuWavByte(&lo) || !readMenuWavByte(&hi)) {
        return false;
      }
      const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(lo) |
                                                  (static_cast<uint16_t>(hi) << 8));
      mixed += sample >> 8;
    }
  }

  mixed /= menuWavChannels;
  mixed = (mixed * MENU_WAV_VOLUME_PERCENT) / 100;
  if (mixed < -128) {
    mixed = -128;
  } else if (mixed > 127) {
    mixed = 127;
  }
  *outDuty = static_cast<uint8_t>(mixed + 128);
  return true;
}

static bool writeMenuI2sPending(TickType_t timeoutTicks) {
  if (!menuI2sStarted || menuI2sPendingBytes == 0) {
    return menuI2sStarted;
  }

  size_t written = 0;
  const uint8_t* src = reinterpret_cast<const uint8_t*>(menuI2sBuffer) + menuI2sPendingOffset;
  const esp_err_t err = i2s_write(I2S_NUM_0, src, menuI2sPendingBytes, &written, timeoutTicks);
  if (err != ESP_OK) {
    return false;
  }

  menuI2sPendingOffset += written;
  menuI2sPendingBytes -= written;
  if (menuI2sPendingBytes == 0) {
    menuI2sPendingOffset = 0;
  }
  return true;
}

static bool queueMenuWavChunk(TickType_t timeoutTicks) {
  if (!menuWavActive || !menuI2sStarted) {
    return false;
  }

  if (menuI2sPendingBytes > 0) {
    return writeMenuI2sPending(timeoutTicks);
  }

  for (size_t i = 0; i < sizeof(menuI2sBuffer) / sizeof(menuI2sBuffer[0]); ++i) {
    uint8_t duty = 128;
    if (!readMenuWavSample(&duty)) {
      if (!seekMenuWavReadPos(menuWavDataStart) || !readMenuWavSample(&duty)) {
        return false;
      }
    }
    menuI2sBuffer[i] = static_cast<uint16_t>(duty) << 8;
  }

  menuI2sPendingOffset = 0;
  menuI2sPendingBytes = sizeof(menuI2sBuffer);
  return writeMenuI2sPending(timeoutTicks);
}

static bool startMenuWav() {
  closeMenuWav();
  if (!initSdCard(false) || !SD.exists(MENU_WAV_PATH)) {
    return false;
  }

  menuWavFile = SD.open(MENU_WAV_PATH, FILE_READ);
  if (!menuWavFile || !readMenuWavHeader(menuWavFile)) {
    closeMenuWav();
    return false;
  }

  if (!startMenuI2s(menuWavSampleRate)) {
    closeMenuWav();
    return false;
  }

  menuWavBufferPos = 0;
  menuWavBufferLen = 0;
  menuWavActive = true;
  for (uint8_t i = 0; i < 6; ++i) {
    if (!queueMenuWavChunk(pdMS_TO_TICKS(20))) {
      closeMenuWav();
      return false;
    }
  }
  Serial.println("Menu WAV active");
  return true;
}

static void menuMusicTick() {
  if (!menuMusicActive) {
    return;
  }

  if (menuWavActive) {
    for (uint8_t i = 0; i < 2; ++i) {
      if (!queueMenuWavChunk(0)) {
        closeMenuWav();
        break;
      }
    }
  }
}

static void menuMusicDelay(uint32_t delayMs) {
  const uint32_t endUs = micros() + delayMs * 1000UL;
  while (static_cast<int32_t>(micros() - endUs) < 0) {
    menuMusicTick();
    if (menuWavActive) {
      delay(1);
    } else {
      delay(1);
    }
  }
}

static void startMenuMusic() {
  menuMusicActive = false;
  closeMenuWav();
  gbCoreAudioSilence();
}

static void stopMenuMusic(bool restoreGameAudio) {
  if (!menuMusicActive && !menuWavActive) {
    return;
  }

  menuMusicActive = false;
  closeMenuWav();
  ledcWrite(AUDIO_PWM_CHANNEL, 0);
  if (restoreGameAudio) {
    gbCoreAudioRefresh();
  }
}

static size_t alignUpSize(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static size_t romStorageNeeded(size_t romSize) {
  return alignUpSize(romSize, SAVE_SLOT_BYTES) + SAVE_SLOT_BYTES * 2;
}

static size_t gbRomSizeFromHeader(const uint8_t* header) {
  static const size_t sizes[] = {
    32UL * 1024, 64UL * 1024, 128UL * 1024, 256UL * 1024,
    512UL * 1024, 1024UL * 1024, 2UL * 1024 * 1024,
    4UL * 1024 * 1024, 8UL * 1024 * 1024
  };
  const uint8_t code = header[0x0148];
  if (code < sizeof(sizes) / sizeof(sizes[0])) {
    return sizes[code];
  }
  return 0;
}

static bool gbHeaderLooksValid(const uint8_t* header) {
  static const uint8_t logo[] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
  };
  if (!header) {
    return false;
  }
  if (memcmp(header + 0x0104, logo, sizeof(logo)) != 0) {
    return false;
  }
  if (header[0x0143] == 0xC0) {
    return false;
  }
  return gbRomSizeFromHeader(header) != 0;
}

static bool hasGbExtension(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  return strcasecmp(dot, ".gb") == 0;
}

static void stripExtension(char* text) {
  char* dot = strrchr(text, '.');
  if (dot) {
    *dot = '\0';
  }
}

static void makeKeyFromName(const char* name, char* out, size_t outSize) {
  size_t pos = 0;
  for (size_t i = 0; name[i] && pos < outSize - 1; ++i) {
    char c = name[i];
    if (c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
    out[pos++] = c;
  }
  out[pos] = '\0';
  stripExtension(out);
}

static uint32_t saveChecksumBytes(const uint8_t* data, size_t size) {
  uint32_t sum = 0x12345678;
  for (size_t i = 0; i < size; ++i) {
    sum = (sum << 5) | (sum >> 27);
    sum ^= data[i];
  }
  return sum;
}

static void ensureSdDirs() {
  if (!SD.exists(SD_ROMS_DIR)) {
    SD.mkdir(SD_ROMS_DIR);
  }
  if (!SD.exists(SD_COVERS_DIR)) {
    SD.mkdir(SD_COVERS_DIR);
  }
  if (!SD.exists(SD_SAVES_DIR)) {
    SD.mkdir(SD_SAVES_DIR);
  }
}

static bool readPartitionBytes(const esp_partition_t* part, size_t offset,
                               uint8_t* data, size_t len) {
  return part && offset + len <= part->size &&
         esp_partition_read(part, offset, data, len) == ESP_OK;
}

static bool hashSdRange(File& file, size_t size, uint32_t* outHash) {
  uint8_t buffer[512];
  uint32_t hash = 2166136261UL;
  size_t remaining = size;
  if (!file.seek(0)) {
    return false;
  }

  while (remaining) {
    const size_t want = min(remaining, sizeof(buffer));
    const int got = file.read(buffer, want);
    if (got != static_cast<int>(want)) {
      return false;
    }
    for (int i = 0; i < got; ++i) {
      hash ^= buffer[i];
      hash *= 16777619UL;
    }
    remaining -= got;
  }

  *outHash = hash;
  return true;
}

static bool hashFlashRange(const esp_partition_t* part, size_t size, uint32_t* outHash) {
  uint8_t buffer[512];
  uint32_t hash = 2166136261UL;
  size_t offset = 0;
  while (offset < size) {
    const size_t want = min(size - offset, sizeof(buffer));
    if (!readPartitionBytes(part, offset, buffer, want)) {
      return false;
    }
    for (size_t i = 0; i < want; ++i) {
      hash ^= buffer[i];
      hash *= 16777619UL;
    }
    offset += want;
  }

  *outHash = hash;
  return true;
}

static bool copySdRomToFlash(File& file, const esp_partition_t* part, size_t romSize) {
  uint8_t* buffer = static_cast<uint8_t*>(malloc(4096));
  if (!buffer) {
    showStatus("SD copy failed", "No buffer RAM");
    return false;
  }

  const size_t eraseLen = alignUpSize(romSize, 0x1000);
  if (esp_partition_erase_range(part, 0, eraseLen) != ESP_OK) {
    free(buffer);
    showStatus("SD copy failed", "Erase ROM failed");
    return false;
  }

  if (!file.seek(0)) {
    free(buffer);
    showStatus("SD copy failed", "Seek failed");
    return false;
  }

  size_t offset = 0;
  uint8_t lastPercent = 255;
  drawLoadingStatus("Installing ROM", "Preparing", 0);
  while (offset < romSize) {
    const size_t want = min(romSize - offset, static_cast<size_t>(4096));
    const int got = file.read(buffer, want);
    if (got != static_cast<int>(want)) {
      free(buffer);
      showStatus("SD copy failed", "Read failed");
      return false;
    }
    if (esp_partition_write(part, offset, buffer, want) != ESP_OK) {
      free(buffer);
      showStatus("SD copy failed", "Write failed");
      return false;
    }

    offset += want;
    const uint8_t percent = static_cast<uint8_t>((offset * 100UL) / romSize);
    if (percent != lastPercent && (percent % 5 == 0 || percent == 100)) {
      char line2[18];
      snprintf(line2, sizeof(line2), "%u%%", percent);
      drawLoadingStatus("Installing ROM", line2, percent);
      lastPercent = percent;
    }
    BP32.update();
    delay(1);
  }

  free(buffer);
  return true;
}

static bool flashRomMatchesFile(File& file, const esp_partition_t* part, size_t romSize) {
  uint8_t flashHeader[0x150] = {};
  uint32_t sdHash = 0;
  uint32_t flashHash = 0;
  return readPartitionBytes(part, 0, flashHeader, sizeof(flashHeader)) &&
         gbHeaderLooksValid(flashHeader) &&
         gbRomSizeFromHeader(flashHeader) == romSize &&
         hashSdRange(file, romSize, &sdHash) &&
         hashFlashRange(part, romSize, &flashHash) &&
         sdHash == flashHash;
}

static bool clearFlashSaveSlots(const esp_partition_t* part, size_t romSize) {
  const size_t saveOffset = alignUpSize(romSize, SAVE_SLOT_BYTES);
  if (!part || part->size < romStorageNeeded(romSize)) {
    return false;
  }
  return esp_partition_erase_range(part, saveOffset, SAVE_SLOT_BYTES * 2) == ESP_OK;
}

static bool writeSdSaveIntoFlash(const char* savePath, const esp_partition_t* part, size_t romSize) {
  const size_t saveOffset = alignUpSize(romSize, SAVE_SLOT_BYTES);
  if (!part || part->size < romStorageNeeded(romSize)) {
    return false;
  }

  clearFlashSaveSlots(part, romSize);
  if (!SD.exists(savePath)) {
    return true;
  }

  File saveFile = SD.open(savePath, FILE_READ);
  if (!saveFile) {
    return false;
  }
  const size_t saveSize = saveFile.size();
  if (saveSize == 0 || saveSize > SAVE_SLOT_BYTES - sizeof(SaveHeader)) {
    saveFile.close();
    return false;
  }

  uint8_t* data = static_cast<uint8_t*>(malloc(saveSize));
  if (!data) {
    saveFile.close();
    return false;
  }
  const bool readOk = saveFile.read(data, saveSize) == static_cast<int>(saveSize);
  saveFile.close();
  if (!readOk) {
    free(data);
    return false;
  }

  SaveHeader header = {
    SAVE_MAGIC,
    static_cast<uint32_t>(saveSize),
    saveChecksumBytes(data, saveSize),
    1,
    SAVE_GUARD_MAGIC
  };
  const bool writeOk =
      esp_partition_write(part, saveOffset, &header, sizeof(header)) == ESP_OK &&
      esp_partition_write(part, saveOffset + sizeof(header), data, saveSize) == ESP_OK;
  free(data);
  return writeOk;
}

static bool readFlashSaveCandidate(const esp_partition_t* part, size_t offset,
                                   SaveHeader* headerOut, size_t* dataOffsetOut) {
  SaveHeader header = {};
  if (!part || part->size < offset + sizeof(header) ||
      esp_partition_read(part, offset, &header, sizeof(header)) != ESP_OK) {
    return false;
  }
  if (header.magic != SAVE_MAGIC || header.size == 0 ||
      header.size > SAVE_SLOT_BYTES - sizeof(SaveHeader)) {
    return false;
  }

  const bool newFormat = header.guard == SAVE_GUARD_MAGIC;
  const size_t dataOffset = newFormat ? sizeof(SaveHeader) : 12;
  if (part->size < offset + dataOffset + header.size) {
    return false;
  }

  uint8_t* data = static_cast<uint8_t*>(malloc(header.size));
  if (!data) {
    return false;
  }
  const bool readOk =
      esp_partition_read(part, offset + dataOffset, data, header.size) == ESP_OK;
  const bool checksumOk = readOk && header.checksum == saveChecksumBytes(data, header.size);
  free(data);
  if (!checksumOk) {
    return false;
  }

  *headerOut = header;
  *dataOffsetOut = dataOffset;
  return true;
}

static bool exportCurrentFlashSaveToSd(const char* key) {
#if !ENABLE_SD_ROM_MENU
  (void)key;
  return false;
#else
  if (!key || !key[0]) {
    return false;
  }

  const bool openedSdHere = !sdReady;
  if (openedSdHere && !initSdCard(true)) {
    return false;
  }

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  uint8_t header[0x150] = {};
  if (!readPartitionBytes(part, 0, header, sizeof(header)) || !gbHeaderLooksValid(header)) {
    if (openedSdHere) {
      closeSdCard();
    }
    return false;
  }

  const size_t romSize = gbRomSizeFromHeader(header);
  const size_t saveOffset = alignUpSize(romSize, SAVE_SLOT_BYTES);
  SaveHeader first = {};
  SaveHeader second = {};
  size_t firstDataOffset = 0;
  size_t secondDataOffset = 0;
  const bool firstOk = readFlashSaveCandidate(part, saveOffset, &first, &firstDataOffset);
  const bool secondOk = readFlashSaveCandidate(part, saveOffset + SAVE_SLOT_BYTES,
                                               &second, &secondDataOffset);
  if (!firstOk && !secondOk) {
    if (openedSdHere) {
      closeSdCard();
    }
    return false;
  }

  const bool useFirst = !secondOk || (firstOk && first.sequence >= second.sequence);
  const SaveHeader chosen = useFirst ? first : second;
  const size_t chosenOffset = saveOffset + (useFirst ? 0 : SAVE_SLOT_BYTES);
  const size_t chosenDataOffset = useFirst ? firstDataOffset : secondDataOffset;

  uint8_t* data = static_cast<uint8_t*>(malloc(chosen.size));
  if (!data) {
    if (openedSdHere) {
      closeSdCard();
    }
    return false;
  }
  const bool readOk =
      esp_partition_read(part, chosenOffset + chosenDataOffset, data, chosen.size) == ESP_OK;
  if (!readOk) {
    free(data);
    if (openedSdHere) {
      closeSdCard();
    }
    return false;
  }

  char savePath[96];
  snprintf(savePath, sizeof(savePath), "%s/%s.sav", SD_SAVES_DIR, key);
  SD.remove(savePath);
  File saveFile = SD.open(savePath, FILE_WRITE);
  if (!saveFile) {
    free(data);
    if (openedSdHere) {
      closeSdCard();
    }
    return false;
  }
  const bool writeOk = saveFile.write(data, chosen.size) == chosen.size;
  saveFile.close();
  free(data);
  if (writeOk) {
    Serial.print("Save exported: ");
    Serial.println(savePath);
  }
  if (openedSdHere) {
    closeSdCard();
  }
  return writeOk;
#endif
}

static uint32_t frameHash() {
  uint32_t hash = 2166136261UL;
  for (int i = 0; i < GB_WIDTH * GB_HEIGHT; ++i) {
    hash ^= gbFrame[i];
    hash *= 16777619UL;
  }
  return hash;
}

static uint8_t glyphFor(char c, uint8_t row) {
  switch (c) {
    case '0': { static const uint8_t g[] = {7,5,5,5,7}; return g[row]; }
    case '1': { static const uint8_t g[] = {2,6,2,2,7}; return g[row]; }
    case '2': { static const uint8_t g[] = {7,1,7,4,7}; return g[row]; }
    case '3': { static const uint8_t g[] = {7,1,7,1,7}; return g[row]; }
    case '4': { static const uint8_t g[] = {5,5,7,1,1}; return g[row]; }
    case '5': { static const uint8_t g[] = {7,4,7,1,7}; return g[row]; }
    case '6': { static const uint8_t g[] = {7,4,7,5,7}; return g[row]; }
    case '7': { static const uint8_t g[] = {7,1,1,1,1}; return g[row]; }
    case '8': { static const uint8_t g[] = {7,5,7,5,7}; return g[row]; }
    case '9': { static const uint8_t g[] = {7,5,7,1,7}; return g[row]; }
    case 'A': { static const uint8_t g[] = {7,5,7,5,5}; return g[row]; }
    case 'B': { static const uint8_t g[] = {6,5,6,5,6}; return g[row]; }
    case 'D': { static const uint8_t g[] = {6,5,5,5,6}; return g[row]; }
    case 'E': { static const uint8_t g[] = {7,4,6,4,7}; return g[row]; }
    case 'F': { static const uint8_t g[] = {7,4,6,4,4}; return g[row]; }
    case 'G': { static const uint8_t g[] = {7,4,5,5,7}; return g[row]; }
    case 'H': { static const uint8_t g[] = {5,5,7,5,5}; return g[row]; }
    case 'I': { static const uint8_t g[] = {7,2,2,2,7}; return g[row]; }
    case 'K': { static const uint8_t g[] = {5,5,6,5,5}; return g[row]; }
    case 'L': { static const uint8_t g[] = {4,4,4,4,7}; return g[row]; }
    case 'M': { static const uint8_t g[] = {5,7,7,5,5}; return g[row]; }
    case 'O': { static const uint8_t g[] = {7,5,5,5,7}; return g[row]; }
    case 'P': { static const uint8_t g[] = {7,5,7,4,4}; return g[row]; }
    case 'R': { static const uint8_t g[] = {6,5,6,5,5}; return g[row]; }
    case 'S': { static const uint8_t g[] = {7,4,7,1,7}; return g[row]; }
    case 'T': { static const uint8_t g[] = {7,2,2,2,2}; return g[row]; }
    case 'U': { static const uint8_t g[] = {5,5,5,5,7}; return g[row]; }
    case 'V': { static const uint8_t g[] = {5,5,5,5,2}; return g[row]; }
    case 'W': { static const uint8_t g[] = {5,5,7,7,5}; return g[row]; }
    case '+': { static const uint8_t g[] = {0,2,7,2,0}; return g[row]; }
    case '-': { static const uint8_t g[] = {0,0,7,0,0}; return g[row]; }
    case ' ': return 0;
    default: return 0;
  }
}

static void fillFrameRect(int x, int y, int w, int h, uint16_t color) {
  for (int yy = max(0, y); yy < min(GB_HEIGHT, y + h); ++yy) {
    for (int xx = max(0, x); xx < min(GB_WIDTH, x + w); ++xx) {
      gbFrame[yy * GB_WIDTH + xx] = color;
    }
  }
}

static void drawFrameText(const char* text, int x, int y, uint16_t fg, uint16_t bg) {
  for (int i = 0; text[i] && x + i * 4 < GB_WIDTH; ++i) {
    for (int yy = 0; yy < 5; ++yy) {
      const uint8_t bits = glyphFor(text[i], yy);
      for (int xx = 0; xx < 3; ++xx) {
        const int px = x + i * 4 + xx;
        const int py = y + yy;
        if (px >= 0 && px < GB_WIDTH && py >= 0 && py < GB_HEIGHT) {
          gbFrame[py * GB_WIDTH + px] = (bits & (1 << (2 - xx))) ? fg : bg;
        }
      }
    }
  }
}

static void drawHudIntoFrame() {
  if (displayMode != DISPLAY_BIG) {
    return;
  }

  char label[12];
  snprintf(label, sizeof(label), "%u FPS", currentFps);
  fillFrameRect(0, 0, 30, 7, 0x0000);
  drawFrameText(label, 1, 1, 0xFFFF, 0x0000);

  if (statusUntil > millis()) {
    const int chars = strlen(statusText);
    const int boxW = min(GB_WIDTH - 4, chars * 4 + 4);
    const int boxX = (GB_WIDTH - boxW) / 2;
    fillFrameRect(boxX, 0, boxW, 8, 0x0000);
    drawFrameText(statusText, boxX + 2, 1, 0xFFFF, 0x0000);
  }
}

static void updateFrameRect() {
  switch (displayMode) {
    case DISPLAY_BIG:
      frameRect = {0, 0, 320, 240};
      break;
    case DISPLAY_BALANCED:
      frameRect = {40, 16, 240, 216};
      break;
    case DISPLAY_FAST:
      frameRect = {80, 48, 160, 144};
      break;
    default:
      frameRect = {40, 16, 240, 216};
      break;
  }
}

static void applyBrightness() {
  ledcWrite(TFT_BL_CHANNEL, brightnessValues[brightnessLevel]);
}

static void drawBackdrop() {
  tft.fillScreen(TFT_BLACK);
}

static void invalidateDrawCache() {
  lastDrawHash = 0;
  lastDrawFps = 0xFFFF;
  lastDrawMode = DISPLAY_MODE_COUNT;
  lastFullStatusVisible = false;
  lastMarginFps[0] = '\0';
  lastMarginStatus[0] = '\0';
  lastQuoteIndex = 0xFF;
}

static void drawTextStack(const char* text, int centerX, int centerY) {
  uint8_t lineCount = 1;
  for (const char* p = text; *p; ++p) {
    if (*p == '|') {
      lineCount++;
    }
  }

  int y = centerY - (lineCount * 10) / 2 + 5;
  char line[9] = "";
  uint8_t linePos = 0;
  for (const char* p = text;; ++p) {
    if (*p == '|' || *p == '\0') {
      line[linePos] = '\0';
      tft.drawString(line, centerX, y, 1);
      y += 10;
      linePos = 0;
      if (*p == '\0') {
        break;
      }
      continue;
    }
    if (linePos < sizeof(line) - 1) {
      line[linePos++] = *p;
    }
  }
}

static void drawMarginQuote() {
  if (displayMode == DISPLAY_BIG) {
    return;
  }

  struct QuotePair {
    const char* left;
    const char* right;
  };
  static const QuotePair quotes[] = {
    {"SAVE|FIRST", "FLEX|LATER"},
    {"ONE|MORE", "BATTLE|TIME"},
    {"CRITS|HIT", "STAY|COOL"},
    {"MENU|SURF", "IS|REAL"},
    {"FRAME|UP", "LAG|DOWN"},
    {"SAVE|NOW", "PLAY|ON"},
  };
  const uint8_t quoteCount = sizeof(quotes) / sizeof(quotes[0]);
  const uint8_t quoteIndex = (millis() / QUOTE_INTERVAL_MS) % quoteCount;
  if (quoteIndex == lastQuoteIndex) {
    return;
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(190, 225, 255), TFT_BLACK);
  if (displayMode == DISPLAY_BALANCED) {
    tft.fillRect(0, 16, 40, 216, TFT_BLACK);
    tft.fillRect(280, 16, 40, 216, TFT_BLACK);
    drawTextStack(quotes[quoteIndex].left, 20, 124);
    drawTextStack(quotes[quoteIndex].right, 300, 124);
  } else {
    tft.fillRect(0, 48, 80, 144, TFT_BLACK);
    tft.fillRect(240, 48, 80, 144, TFT_BLACK);
    drawTextStack(quotes[quoteIndex].left, 40, 120);
    drawTextStack(quotes[quoteIndex].right, 280, 120);
  }
  lastQuoteIndex = quoteIndex;
}

static void cycleDisplayMode(int8_t direction) {
  if (direction > 0) {
    displayMode = static_cast<DisplayMode>((displayMode + 1) % DISPLAY_MODE_COUNT);
  } else {
    displayMode = static_cast<DisplayMode>((displayMode + DISPLAY_MODE_COUNT - 1) % DISPLAY_MODE_COUNT);
  }
  updateFrameRect();
  invalidateDrawCache();
  drawBackdrop();
  drawMarginQuote();
  setOverlayStatus(displayMode == DISPLAY_BIG ? "FULL" : displayMode == DISPLAY_BALANCED ? "BIG" : "FAST", 1500);
}

static void rumbleAll(uint16_t durationMs, uint8_t weakMagnitude, uint8_t strongMagnitude) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; ++i) {
    ControllerPtr gp = gamepads[i];
    if (gp && gp->isConnected()) {
      gp->playDualRumble(0, durationMs, weakMagnitude, strongMagnitude);
    }
  }
}

static void formatSessionSaveStatus(char* out, size_t outSize) {
  const uint32_t now = millis();
  uint32_t sleepMs = sessionSleepTotalMs;
  if (softSleeping) {
    sleepMs += now - softSleepStartedMs;
  }
  const uint32_t elapsedMs = now - sessionStartMs - sleepMs;
  const uint32_t totalMinutes = elapsedMs / 60000UL;
  if (totalMinutes < 60) {
    snprintf(out, outSize, "SAVED %luM", static_cast<unsigned long>(totalMinutes));
    return;
  }

  const uint32_t hours = totalMinutes / 60;
  const uint32_t minutes = totalMinutes % 60;
  if (hours < 100) {
    snprintf(out, outSize, "SAVED %luH%02luM",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  } else {
    snprintf(out, outSize, "SAVED 99H+");
  }
}

static void flushSaveWithStatus(const char* cleanMessage) {
  const bool dirtyBeforeFlush = gbCoreSaveDirty();
  const bool flushed = gbCoreFlushSave();
  gbCoreConsumeSaveEvent();
  exportCurrentFlashSaveToSd(currentGameKey);
  setOverlayStatus((!dirtyBeforeFlush || flushed) ? cleanMessage : "SAVE FAIL", 2200);
  rumbleAll(flushed || !dirtyBeforeFlush ? 90 : 220, flushed || !dirtyBeforeFlush ? 0x50 : 0x20,
            flushed || !dirtyBeforeFlush ? 0x30 : 0x90);
}

static void enterSoftSleep() {
  flushSaveWithStatus("SLEEP");
  rumbleAll(140, 0x70, 0x30);
  gbCoreAudioSilence();
  drawHudIntoFrame();
  drawFrameScaled();
  drawMarginHud();
  delay(120);
  tft.fillScreen(TFT_BLACK);
  ledcWrite(TFT_BL_CHANNEL, 0);
  softSleepStartedMs = millis();
  softSleeping = true;
  invalidateDrawCache();
}

static void wakeSoftSleep() {
  sessionSleepTotalMs += millis() - softSleepStartedMs;
  softSleeping = false;
  applyBrightness();
  drawBackdrop();
  drawMarginQuote();
  setOverlayStatus("WAKE", 900);
  rumbleAll(90, 0x30, 0x60);
  invalidateDrawCache();
}

static void onConnectedGamepad(ControllerPtr gp) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; ++i) {
    if (!gamepads[i]) {
      gamepads[i] = gp;
      break;
    }
  }
}

static void onDisconnectedGamepad(ControllerPtr gp) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; ++i) {
    if (gamepads[i] == gp) {
      gamepads[i] = nullptr;
      break;
    }
  }
}

static void setupPushbuttons() {
#if ENABLE_GPIO_PUSHBUTTONS
  const int buttonPins[] = {
    BUTTON_UP_PIN, BUTTON_DOWN_PIN, BUTTON_LEFT_PIN, BUTTON_RIGHT_PIN,
    BUTTON_A_PIN, BUTTON_B_PIN, BUTTON_SELECT_PIN, BUTTON_START_PIN,
  };
  for (size_t i = 0; i < sizeof(buttonPins) / sizeof(buttonPins[0]); ++i) {
    const int pin = buttonPins[i];
    if (pin < 0) {
      continue;
    }
    if (pin >= 34 && pin <= 39) {
      pinMode(pin, INPUT);
    } else {
      pinMode(pin, INPUT_PULLUP);
    }
  }
#endif
}

static uint16_t readPushbuttons() {
#if ENABLE_GPIO_PUSHBUTTONS
  uint16_t out = 0;
  if (BUTTON_UP_PIN >= 0 && digitalRead(BUTTON_UP_PIN) == LOW) out |= GB_BTN_UP;
  if (BUTTON_DOWN_PIN >= 0 && digitalRead(BUTTON_DOWN_PIN) == LOW) out |= GB_BTN_DOWN;
  if (BUTTON_LEFT_PIN >= 0 && digitalRead(BUTTON_LEFT_PIN) == LOW) out |= GB_BTN_LEFT;
  if (BUTTON_RIGHT_PIN >= 0 && digitalRead(BUTTON_RIGHT_PIN) == LOW) out |= GB_BTN_RIGHT;
  if (BUTTON_A_PIN >= 0 && digitalRead(BUTTON_A_PIN) == LOW) out |= GB_BTN_A;
  if (BUTTON_B_PIN >= 0 && digitalRead(BUTTON_B_PIN) == LOW) out |= GB_BTN_B;
  if (BUTTON_SELECT_PIN >= 0 && digitalRead(BUTTON_SELECT_PIN) == LOW) out |= GB_BTN_SELECT;
  if (BUTTON_START_PIN >= 0 && digitalRead(BUTTON_START_PIN) == LOW) out |= GB_BTN_START;
  return out;
#else
  return 0;
#endif
}

static uint16_t readButtons() {
  controllerLShoulderPressed = false;
  controllerRShoulderPressed = false;
  controllerLTriggerPressed = false;
  controllerRTriggerPressed = false;
  controllerThumbRPressed = false;
  uint16_t out = readPushbuttons();

  ControllerPtr gp = nullptr;
  for (int i = 0; i < BP32_MAX_GAMEPADS; ++i) {
    if (gamepads[i] && gamepads[i]->isConnected() && gamepads[i]->hasData()) {
      gp = gamepads[i];
      break;
    }
  }
  if (!gp) {
    return out;
  }

  if (gp->dpad() & DPAD_UP) out |= GB_BTN_UP;
  if (gp->dpad() & DPAD_DOWN) out |= GB_BTN_DOWN;
  if (gp->dpad() & DPAD_LEFT) out |= GB_BTN_LEFT;
  if (gp->dpad() & DPAD_RIGHT) out |= GB_BTN_RIGHT;

  if (gp->axisX() < -256) out |= GB_BTN_LEFT;
  if (gp->axisX() > 256) out |= GB_BTN_RIGHT;
  if (gp->axisY() < -256) out |= GB_BTN_UP;
  if (gp->axisY() > 256) out |= GB_BTN_DOWN;

  if (gp->a()) out |= GB_BTN_A;
  if (gp->b()) out |= GB_BTN_B;
  if (gp->x()) out |= GB_BTN_A;
  if (gp->y()) out |= GB_BTN_B;
  if (gp->miscStart()) out |= GB_BTN_START;
  if (gp->miscSelect()) out |= GB_BTN_SELECT;
  controllerLShoulderPressed = gp->l1();
  controllerRShoulderPressed = gp->r1();
  const int32_t throttle = gp->throttle();
  const int32_t brake = gp->brake();
  controllerLTriggerPressed = gp->l2() || brake > TRIGGER_DEADZONE;
  controllerRTriggerPressed = gp->r2() || throttle > TRIGGER_DEADZONE;
  controllerThumbRPressed = gp->thumbR();

  return out;
}

static uint16_t filterButtons(uint16_t physical) {
  const uint32_t now = millis();
  uint16_t out = physical & (GB_BTN_RIGHT | GB_BTN_LEFT | GB_BTN_UP | GB_BTN_DOWN |
                             GB_BTN_SELECT | GB_BTN_START);

  for (int i = 0; i < 8; ++i) {
    const uint16_t mask = 1 << i;
    if (!(mask & (GB_BTN_A | GB_BTN_B))) {
      continue;
    }

    const bool pressed = physical & mask;
    const bool wasPressed = lastPhysicalButtons & mask;

    if (pressed && !wasPressed) {
      buttonDownAt[i] = now;
      lastRepeatAt[i] = now;
      out |= mask;
      continue;
    }

    if (!pressed) {
      continue;
    }

    if (now - buttonDownAt[i] < AB_TAP_MS) {
      out |= mask;
    }
  }

  lastPhysicalButtons = physical;
  filteredButtons = out;
  return filteredButtons;
}

static bool readRomHeader(File& file, uint8_t* header) {
  return file.seek(0) &&
         file.read(header, 0x150) == 0x150 &&
         gbHeaderLooksValid(header);
}

static bool buildRomList() {
  romCount = 0;
  File dir = SD.open(SD_ROMS_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return false;
  }

  while (romCount < MAX_ROMS) {
    File file = dir.openNextFile();
    if (!file) {
      break;
    }
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    const char* fullName = file.name();
    const char* leaf = strrchr(fullName, '/');
    leaf = leaf ? leaf + 1 : fullName;
    if (!hasGbExtension(leaf)) {
      file.close();
      continue;
    }

    uint8_t header[0x150] = {};
    if (!readRomHeader(file, header)) {
      file.close();
      continue;
    }

    RomEntry& entry = romEntries[romCount];
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.title, leaf, sizeof(entry.title) - 1);
    stripExtension(entry.title);
    makeKeyFromName(leaf, entry.key, sizeof(entry.key));
    snprintf(entry.romPath, sizeof(entry.romPath), "%s/%s", SD_ROMS_DIR, leaf);
    snprintf(entry.coverPath, sizeof(entry.coverPath), "%s/%s.bmp", SD_COVERS_DIR, entry.key);
    snprintf(entry.rawCoverPath, sizeof(entry.rawCoverPath), "%s/%s.565", SD_COVERS_DIR, entry.key);
    snprintf(entry.savePath, sizeof(entry.savePath), "%s/%s.sav", SD_SAVES_DIR, entry.key);
    entry.romSize = gbRomSizeFromHeader(header);
    romCount++;
    file.close();
  }
  dir.close();
  return romCount > 0;
}

static uint16_t read16(File& f) {
  uint16_t value = f.read();
  value |= static_cast<uint16_t>(f.read()) << 8;
  return value;
}

static uint32_t read32(File& f) {
  uint32_t value = read16(f);
  value |= static_cast<uint32_t>(read16(f)) << 16;
  return value;
}

static void drawCoverPlaceholder(int x, int y, int w, int h, const char* title) {
  tft.fillRect(x, y, w, h, tft.color565(26, 30, 42));
  tft.drawRect(x, y, w, h, tft.color565(80, 120, 160));
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, tft.color565(26, 30, 42));
  char line[18] = "";
  strncpy(line, title, sizeof(line) - 1);
  tft.drawString(line, x + w / 2, y + h / 2 - 8, 2);
  tft.drawString("NO COVER", x + w / 2, y + h / 2 + 16, 1);
}

static bool drawRawCover(const char* path, int x, int y, int w, int h) {
  if (w != COVER_W || h != COVER_H || !SD.exists(path)) {
    return false;
  }

  File raw = SD.open(path, FILE_READ);
  if (!raw || raw.size() != COVER_W * COVER_H * sizeof(uint16_t)) {
    if (raw) {
      raw.close();
    }
    return false;
  }

  tft.startWrite();
  tft.setAddrWindow(x, y, COVER_W, COVER_H);
  for (int row = 0; row < COVER_H; ++row) {
    const int bytes = COVER_W * sizeof(uint16_t);
    if (raw.read(reinterpret_cast<uint8_t*>(coverLine565), bytes) != bytes) {
      tft.endWrite();
      raw.close();
      return false;
    }
    tft.pushPixels(coverLine565, COVER_W);
  }
  tft.endWrite();
  raw.close();
  return true;
}

static bool drawBmpCover(const char* path, int x, int y, int w, int h) {
  File bmp = SD.open(path, FILE_READ);
  if (!bmp) {
    return false;
  }
  if (read16(bmp) != 0x4D42) {
    bmp.close();
    return false;
  }

  (void)read32(bmp);
  (void)read32(bmp);
  const uint32_t dataOffset = read32(bmp);
  const uint32_t dibSize = read32(bmp);
  if (dibSize < 40) {
    bmp.close();
    return false;
  }

  const int32_t bmpW = static_cast<int32_t>(read32(bmp));
  const int32_t rawH = static_cast<int32_t>(read32(bmp));
  const bool topDown = rawH < 0;
  const int32_t bmpH = topDown ? -rawH : rawH;
  (void)read16(bmp);
  const uint16_t depth = read16(bmp);
  const uint32_t compression = read32(bmp);
  if (bmpW <= 0 || bmpH <= 0 || depth != 24 || compression != 0) {
    bmp.close();
    return false;
  }

  const uint32_t rowStride = ((bmpW * 3 + 3) / 4) * 4;
  if (w <= COVER_W && rowStride <= 2048) {
    uint8_t* row = rowStride <= sizeof(coverRowBgr) ? coverRowBgr
                                                    : static_cast<uint8_t*>(malloc(rowStride));
    if (!row) {
      bmp.close();
      return false;
    }

    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
    for (int outY = 0; outY < h; ++outY) {
      const int srcY = (static_cast<int32_t>(outY) * bmpH) / h;
      const int fileY = topDown ? srcY : (bmpH - 1 - srcY);
      if (!bmp.seek(dataOffset + fileY * rowStride)) {
        tft.endWrite();
        if (row != coverRowBgr) {
          free(row);
        }
        bmp.close();
        return false;
      }
      if (bmp.read(row, rowStride) != static_cast<int>(rowStride)) {
        tft.endWrite();
        if (row != coverRowBgr) {
          free(row);
        }
        bmp.close();
        return false;
      }
      for (int outX = 0; outX < w; ++outX) {
        const int srcX = (static_cast<int32_t>(outX) * bmpW) / w;
        const uint8_t* px = row + srcX * 3;
        coverLine565[outX] = tft.color565(px[2], px[1], px[0]);
      }
      tft.pushPixels(coverLine565, w);
    }
    tft.endWrite();
    if (row != coverRowBgr) {
      free(row);
    }
    bmp.close();
    return true;
  }

  tft.startWrite();
  for (int outY = 0; outY < h; ++outY) {
    const int srcY = (static_cast<int32_t>(outY) * bmpH) / h;
    const int fileY = topDown ? srcY : (bmpH - 1 - srcY);
    for (int outX = 0; outX < w; ++outX) {
      const int srcX = (static_cast<int32_t>(outX) * bmpW) / w;
      const uint32_t pos = dataOffset + fileY * rowStride + srcX * 3;
      if (!bmp.seek(pos)) {
        tft.endWrite();
        bmp.close();
        return false;
      }
      const uint8_t b = bmp.read();
      const uint8_t g = bmp.read();
      const uint8_t r = bmp.read();
      tft.drawPixel(x + outX, y + outY, tft.color565(r, g, b));
    }
  }
  tft.endWrite();
  bmp.close();
  return true;
}

static bool convertBmpCoverToRaw(const char* bmpPath, const char* rawPath) {
  File bmp = SD.open(bmpPath, FILE_READ);
  if (!bmp) {
    return false;
  }
  if (read16(bmp) != 0x4D42) {
    bmp.close();
    return false;
  }

  (void)read32(bmp);
  (void)read32(bmp);
  const uint32_t dataOffset = read32(bmp);
  const uint32_t dibSize = read32(bmp);
  if (dibSize < 40) {
    bmp.close();
    return false;
  }

  const int32_t bmpW = static_cast<int32_t>(read32(bmp));
  const int32_t rawH = static_cast<int32_t>(read32(bmp));
  const bool topDown = rawH < 0;
  const int32_t bmpH = topDown ? -rawH : rawH;
  (void)read16(bmp);
  const uint16_t depth = read16(bmp);
  const uint32_t compression = read32(bmp);
  if (bmpW <= 0 || bmpH <= 0 || depth != 24 || compression != 0) {
    bmp.close();
    return false;
  }

  const uint32_t rowStride = ((bmpW * 3 + 3) / 4) * 4;
  if (rowStride > 4096) {
    bmp.close();
    return false;
  }

  uint8_t* row = rowStride <= sizeof(coverRowBgr) ? coverRowBgr
                                                  : static_cast<uint8_t*>(malloc(rowStride));
  if (!row) {
    bmp.close();
    return false;
  }

  SD.remove(rawPath);
  File raw = SD.open(rawPath, FILE_WRITE);
  if (!raw) {
    if (row != coverRowBgr) {
      free(row);
    }
    bmp.close();
    return false;
  }

  bool ok = true;
  for (int outY = 0; outY < COVER_H && ok; ++outY) {
    const int srcY = (static_cast<int32_t>(outY) * bmpH) / COVER_H;
    const int fileY = topDown ? srcY : (bmpH - 1 - srcY);
    ok = bmp.seek(dataOffset + fileY * rowStride) &&
         bmp.read(row, rowStride) == static_cast<int>(rowStride);
    if (!ok) {
      break;
    }
    for (int outX = 0; outX < COVER_W; ++outX) {
      const int srcX = (static_cast<int32_t>(outX) * bmpW) / COVER_W;
      const uint8_t* px = row + srcX * 3;
      coverLine565[outX] = tft.color565(px[2], px[1], px[0]);
    }
    ok = raw.write(reinterpret_cast<uint8_t*>(coverLine565),
                   COVER_W * sizeof(uint16_t)) == COVER_W * sizeof(uint16_t);
  }

  raw.close();
  bmp.close();
  if (row != coverRowBgr) {
    free(row);
  }
  if (!ok) {
    SD.remove(rawPath);
  }
  return ok;
}

static void buildCoverCache() {
  for (uint8_t i = 0; i < romCount; ++i) {
    if (SD.exists(romEntries[i].rawCoverPath)) {
      File raw = SD.open(romEntries[i].rawCoverPath, FILE_READ);
      const bool valid = raw && raw.size() == COVER_W * COVER_H * sizeof(uint16_t);
      if (raw) {
        raw.close();
      }
      if (valid) {
        continue;
      }
      SD.remove(romEntries[i].rawCoverPath);
    }

    if (!SD.exists(romEntries[i].coverPath)) {
      continue;
    }

    char progress[24];
    snprintf(progress, sizeof(progress), "%u/%u %s",
             static_cast<unsigned>(i + 1), static_cast<unsigned>(romCount),
             romEntries[i].title);
    drawLoadingStatus("Caching covers", progress);
    convertBmpCoverToRaw(romEntries[i].coverPath, romEntries[i].rawCoverPath);
  }
}

static void drawRomMenu(uint8_t selected, bool bootFlashFallback) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SD ROMS", 8, 6, 2);

  const int coverX = 8;
  const int coverY = 36;
  const int coverW = 128;
  const int coverH = 128;
  if (!drawRawCover(romEntries[selected].rawCoverPath, coverX, coverY, coverW, coverH) &&
      !drawBmpCover(romEntries[selected].coverPath, coverX, coverY, coverW, coverH)) {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH, romEntries[selected].title);
  }

  tft.setTextDatum(TL_DATUM);
  const int first = selected > 2 ? selected - 2 : 0;
  for (int row = 0; row < 6; ++row) {
    const int idx = first + row;
    if (idx >= romCount) {
      break;
    }
    const int y = 34 + row * 28;
    const bool active = idx == selected;
    const uint16_t bg = active ? tft.color565(35, 80, 130) : TFT_BLACK;
    const uint16_t fg = active ? TFT_WHITE : tft.color565(190, 210, 220);
    tft.fillRect(148, y - 2, 164, 24, bg);
    tft.setTextColor(fg, bg);
    char label[22] = "";
    strncpy(label, romEntries[idx].title, sizeof(label) - 1);
    tft.drawString(label, 154, y + 3, 2);
  }

  tft.setTextColor(tft.color565(170, 220, 170), TFT_BLACK);
  tft.drawString("A/START PLAY", 8, 178, 1);
  tft.drawString("UP/DOWN PICK", 8, 194, 1);
  tft.drawString(bootFlashFallback ? "B BOOT FLASH" : "B BACK", 8, 210, 1);
}

static int chooseRomFromMenu(bool bootFlashFallback) {
  if (romCount == 0) {
    return -1;
  }

  uint8_t selected = 0;
  uint16_t lastMenuButtons = 0;
  bool lastMenuLShoulder = false;
  bool lastMenuRShoulder = false;
  uint32_t lastRepeat = 0;
  drawRomMenu(selected, bootFlashFallback);
  startMenuMusic();
  BP32.update();
  lastMenuButtons = readButtons();
  lastMenuLShoulder = controllerLShoulderPressed;
  lastMenuRShoulder = controllerRShoulderPressed;
  lastRepeat = millis();

  while (true) {
    BP32.update();
    menuMusicTick();
    const uint16_t buttons = readButtons();
    const uint16_t pressed = buttons & ~lastMenuButtons;
    const bool lShoulderPressed = controllerLShoulderPressed && !lastMenuLShoulder;
    const bool rShoulderPressed = controllerRShoulderPressed && !lastMenuRShoulder;
    const uint32_t now = millis();

    if (rShoulderPressed && brightnessLevel < sizeof(brightnessValues) - 1) {
      brightnessLevel++;
      applyBrightness();
      rumbleAll(35, 0x18, 0x18);
    }
    if (lShoulderPressed && brightnessLevel > 0) {
      brightnessLevel--;
      applyBrightness();
      rumbleAll(35, 0x18, 0x18);
    }

    int8_t move = 0;
    if (pressed & (GB_BTN_DOWN | GB_BTN_RIGHT)) {
      move = 1;
    } else if (pressed & (GB_BTN_UP | GB_BTN_LEFT)) {
      move = -1;
    } else if ((buttons & (GB_BTN_DOWN | GB_BTN_RIGHT)) && now - lastRepeat > 280) {
      move = 1;
    } else if ((buttons & (GB_BTN_UP | GB_BTN_LEFT)) && now - lastRepeat > 280) {
      move = -1;
    }

    if (move) {
      selected = (selected + romCount + move) % romCount;
      drawRomMenu(selected, bootFlashFallback);
      rumbleAll(35, 0x20, 0x10);
      lastRepeat = now;
    }

    if (pressed & (GB_BTN_A | GB_BTN_START)) {
      rumbleAll(120, 0x60, 0x40);
      stopMenuMusic(false);
      return selected;
    }
    if (pressed & GB_BTN_B) {
      rumbleAll(80, 0x20, 0x20);
      stopMenuMusic(false);
      return -1;
    }

    lastMenuButtons = buttons;
    lastMenuLShoulder = controllerLShoulderPressed;
    lastMenuRShoulder = controllerRShoulderPressed;
    menuMusicDelay(16);
  }
}

static bool installSelectedRom(const RomEntry& entry) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part || part->size < romStorageNeeded(entry.romSize)) {
    showStatus("ROM too big", entry.title);
    delay(1200);
    return false;
  }

  File romFile = SD.open(entry.romPath, FILE_READ);
  if (!romFile) {
    showStatus("Open ROM failed", entry.title);
    delay(1200);
    return false;
  }

  uint8_t header[0x150] = {};
  if (!readRomHeader(romFile, header) || gbRomSizeFromHeader(header) != entry.romSize ||
      romFile.size() < entry.romSize) {
    romFile.close();
    showStatus("Bad ROM", entry.title);
    delay(1200);
    return false;
  }

  const bool sameRom = flashRomMatchesFile(romFile, part, entry.romSize);
  if (!sameRom) {
    exportCurrentFlashSaveToSd(currentGameKey);
    drawLoadingStatus("Installing ROM", entry.title, 0);
    if (!copySdRomToFlash(romFile, part, entry.romSize)) {
      romFile.close();
      return false;
    }
    clearFlashSaveSlots(part, entry.romSize);
    if (AUTO_IMPORT_SD_SAVES && !writeSdSaveIntoFlash(entry.savePath, part, entry.romSize)) {
      showStatus("Save import fail", "Starting clean");
      delay(900);
    }
  } else if (currentGameKey[0] && strcmp(currentGameKey, entry.key) == 0) {
    exportCurrentFlashSaveToSd(currentGameKey);
  }

  romFile.close();
  strncpy(currentGameKey, entry.key, sizeof(currentGameKey) - 1);
  currentGameKey[sizeof(currentGameKey) - 1] = '\0';
  prefs.putString("game", currentGameKey);
  showStatus("Loaded", entry.title);
  delay(650);
  return true;
}

static void identifyCurrentFlashRomFromSd() {
  if (currentGameKey[0] || romCount == 0) {
    return;
  }

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) {
    return;
  }

  for (uint8_t i = 0; i < romCount; ++i) {
    File romFile = SD.open(romEntries[i].romPath, FILE_READ);
    if (!romFile) {
      continue;
    }
    const bool matches = flashRomMatchesFile(romFile, part, romEntries[i].romSize);
    romFile.close();
    if (matches) {
      strncpy(currentGameKey, romEntries[i].key, sizeof(currentGameKey) - 1);
      currentGameKey[sizeof(currentGameKey) - 1] = '\0';
      prefs.putString("game", currentGameKey);
      exportCurrentFlashSaveToSd(currentGameKey);
      Serial.print("Current flash ROM identified: ");
      Serial.println(currentGameKey);
      return;
    }
  }
}

static bool initSdCard(bool createDirs) {
  if (sdReady) {
    if (createDirs) {
      ensureSdDirs();
    }
    return true;
  }

  if (!sdBusStarted) {
    sdSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    sdBusStarted = true;
  }

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  const uint32_t speeds[] = {SD_SPI_HZ, 8000000UL, 4000000UL};
  for (uint8_t attempt = 0; attempt < sizeof(speeds) / sizeof(speeds[0]); ++attempt) {
    SD.end();
    delay(10);
    sdReady = SD.begin(SD_CS, sdSpi, speeds[attempt]);
    if (sdReady) {
      break;
    }
    if (attempt == 0) {
      sdSpi.end();
      delay(10);
      sdSpi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    }
  }

  if (sdReady && createDirs) {
    ensureSdDirs();
  }
  return sdReady;
}

static void closeSdCard() {
  if (sdReady) {
    SD.end();
    sdReady = false;
  }
  digitalWrite(SD_CS, HIGH);
}

static void runSdLauncher() {
  showStatus("Checking SD", SD_ROMS_DIR);
  if (!initSdCard(true)) {
    showStatus("No SD found", "Booting flash");
    delay(700);
    return;
  }

  if (!buildRomList()) {
    showStatus("No ROMs found", "/roms/*.gb");
    delay(1000);
    return;
  }

  buildCoverCache();
  identifyCurrentFlashRomFromSd();
  showStatus("Pair controller", "Choose ROM");
  const int choice = chooseRomFromMenu(true);
  if (choice < 0 || choice >= romCount) {
    showStatus("Booting flash", "No ROM change");
    delay(650);
    return;
  }

  installSelectedRom(romEntries[choice]);
}

static bool loadRomFromFlash() {
  closeSdCard();
  delay(20);
  drawLoadingStatus("Loading ROM", "Please wait");
  bool ok = gbCoreLoadRom();
  if (!ok) {
    showStatus("ROM load failed");
  }
  return ok;
}

static void setupSpeakerTest() {
  gbCoreAudioBegin();
}

static void rememberControlState(uint16_t physical);

static void resetGameplayRuntime() {
  sessionStartMs = millis();
  softSleepStartedMs = 0;
  sessionSleepTotalMs = 0;
  fpsWindowStart = millis();
  fpsFrames = 0;
  currentFps = 0;
  previousButtons = 0;
  lastPhysicalButtons = 0;
  filteredButtons = 0;
  for (int i = 0; i < 8; ++i) {
    buttonDownAt[i] = 0;
    lastRepeatAt[i] = 0;
  }
  updateFrameRect();
  drawBackdrop();
  invalidateDrawCache();
}

static bool entryMatchesFlash(const RomEntry& entry) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) {
    return false;
  }

  File romFile = SD.open(entry.romPath, FILE_READ);
  if (!romFile) {
    return false;
  }
  const bool matches = flashRomMatchesFile(romFile, part, entry.romSize);
  romFile.close();
  return matches;
}

static void drawSystemMenu(uint8_t selected) {
#if ENABLE_SD_ROM_MENU
  static const char* items[] = {
    "RESUME",
    "CHANGE GAME",
    "SAVE",
    "EJECT SD",
    "SLEEP",
  };
  static constexpr uint8_t itemCount = 5;
#else
  static const char* items[] = {
    "RESUME",
    "SAVE",
    "SLEEP",
  };
  static constexpr uint8_t itemCount = 3;
#endif

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SYSTEM MENU", 12, 10, 2);
  tft.setTextColor(tft.color565(170, 220, 170), TFT_BLACK);
  tft.drawString("A/START OK   B BACK", 12, 222, 1);

  for (uint8_t i = 0; i < itemCount; ++i) {
    const int y = 46 + i * 32;
    const bool active = i == selected;
    const uint16_t bg = active ? tft.color565(35, 80, 130) : TFT_BLACK;
    const uint16_t fg = active ? TFT_WHITE : tft.color565(190, 210, 220);
    tft.fillRect(44, y - 4, 232, 26, bg);
    tft.setTextColor(fg, bg);
    tft.drawString(items[i], 58, y, 2);
  }
}

static SystemAction chooseSystemAction() {
#if ENABLE_SD_ROM_MENU
  static const SystemAction actions[] = {
    SYS_RESUME, SYS_CHANGE_GAME, SYS_SAVE, SYS_EJECT_SD, SYS_SLEEP,
  };
  static constexpr uint8_t itemCount = sizeof(actions) / sizeof(actions[0]);
#else
  static const SystemAction actions[] = {
    SYS_RESUME, SYS_SAVE, SYS_SLEEP,
  };
  static constexpr uint8_t itemCount = sizeof(actions) / sizeof(actions[0]);
#endif
  uint8_t selected = 0;
  uint16_t lastMenuButtons = 0;
  bool lastMenuLShoulder = false;
  bool lastMenuRShoulder = false;
  uint32_t lastRepeat = 0;
  drawSystemMenu(selected);
  startMenuMusic();
  BP32.update();
  lastMenuButtons = readButtons();
  lastMenuLShoulder = controllerLShoulderPressed;
  lastMenuRShoulder = controllerRShoulderPressed;
  lastRepeat = millis();

  while (true) {
    BP32.update();
    menuMusicTick();
    const uint16_t buttons = readButtons();
    const uint16_t pressed = buttons & ~lastMenuButtons;
    const bool lShoulderPressed = controllerLShoulderPressed && !lastMenuLShoulder;
    const bool rShoulderPressed = controllerRShoulderPressed && !lastMenuRShoulder;
    const uint32_t now = millis();

    if (rShoulderPressed && brightnessLevel < sizeof(brightnessValues) - 1) {
      brightnessLevel++;
      applyBrightness();
      rumbleAll(35, 0x18, 0x18);
    }
    if (lShoulderPressed && brightnessLevel > 0) {
      brightnessLevel--;
      applyBrightness();
      rumbleAll(35, 0x18, 0x18);
    }

    int8_t move = 0;
    if (pressed & (GB_BTN_DOWN | GB_BTN_RIGHT)) {
      move = 1;
    } else if (pressed & (GB_BTN_UP | GB_BTN_LEFT)) {
      move = -1;
    } else if ((buttons & (GB_BTN_DOWN | GB_BTN_RIGHT)) && now - lastRepeat > 280) {
      move = 1;
    } else if ((buttons & (GB_BTN_UP | GB_BTN_LEFT)) && now - lastRepeat > 280) {
      move = -1;
    }

    if (move) {
      selected = (selected + itemCount + move) % itemCount;
      drawSystemMenu(selected);
      rumbleAll(35, 0x20, 0x10);
      lastRepeat = now;
    }

    if (pressed & (GB_BTN_A | GB_BTN_START)) {
      rumbleAll(80, 0x40, 0x30);
      stopMenuMusic(false);
      return actions[selected];
    }
    if (pressed & GB_BTN_B) {
      rumbleAll(45, 0x20, 0x20);
      stopMenuMusic(false);
      return SYS_RESUME;
    }

    lastMenuButtons = buttons;
    lastMenuLShoulder = controllerLShoulderPressed;
    lastMenuRShoulder = controllerRShoulderPressed;
    menuMusicDelay(16);
  }
}

static void resumeAfterMenu() {
#if ENABLE_SD_ROM_MENU
  closeSdCard();
#endif
  gbCoreAudioRefresh();
  drawBackdrop();
  drawMarginQuote();
  invalidateDrawCache();
  fpsWindowStart = millis();
  rememberControlState(readButtons());
}

static void safeEjectSdFromMenu() {
  drawLoadingStatus("Saving game", "Before eject");
  gbCoreFlushSave();
  gbCoreConsumeSaveEvent();
  exportCurrentFlashSaveToSd(currentGameKey);

  closeSdCard();
  showStatus("SD SAFE", "Remove card");
  rumbleAll(150, 0x40, 0x60);
  delay(1200);
  setOverlayStatus("SD SAFE", 1400);
}

static bool switchToRomFromRuntime(const RomEntry& entry) {
  drawLoadingStatus("Saving game", "Before switch");
  gbCoreFlushSave();
  gbCoreConsumeSaveEvent();
  exportCurrentFlashSaveToSd(currentGameKey);

  if (entryMatchesFlash(entry)) {
    strncpy(currentGameKey, entry.key, sizeof(currentGameKey) - 1);
    currentGameKey[sizeof(currentGameKey) - 1] = '\0';
    prefs.putString("game", currentGameKey);
    setOverlayStatus("SAME ROM", 1200);
    resumeAfterMenu();
    return true;
  }

  gbCoreUnloadRom();
  if (!installSelectedRom(entry)) {
    showStatus("Switch failed", "Recovering");
    delay(800);
    if (!loadRomFromFlash()) {
      showStatus("ROM failed", "Restarting");
      delay(1200);
      esp_restart();
    }
    resetGameplayRuntime();
    setOverlayStatus("RECOVERED", 1200);
    return false;
  }

  if (!loadRomFromFlash()) {
    showStatus("ROM load failed", "Restarting");
    delay(1200);
    esp_restart();
  }
  resetGameplayRuntime();
  setOverlayStatus("LOADED", 1400);
  return true;
}

static void openRomSwitcherFromRuntime() {
  drawLoadingStatus("Checking SD", SD_ROMS_DIR);
  if (!initSdCard(true)) {
    showStatus("No SD found", "Resume game");
    delay(900);
    setOverlayStatus("NO SD", 1200);
    resumeAfterMenu();
    return;
  }

  if (!buildRomList()) {
    showStatus("No ROMs found", "/roms/*.gb");
    delay(1000);
    setOverlayStatus("NO ROMS", 1200);
    resumeAfterMenu();
    return;
  }

  buildCoverCache();
  identifyCurrentFlashRomFromSd();
  const int choice = chooseRomFromMenu(false);
  if (choice >= 0 && choice < romCount) {
    switchToRomFromRuntime(romEntries[choice]);
  } else {
    setOverlayStatus("RESUME", 800);
    resumeAfterMenu();
  }
}

static void openSystemMenu() {
  gbCoreSetButtons(0);
  rumbleAll(60, 0x30, 0x30);

  const SystemAction action = chooseSystemAction();
  switch (action) {
    case SYS_CHANGE_GAME:
#if ENABLE_SD_ROM_MENU
      openRomSwitcherFromRuntime();
#else
      resumeAfterMenu();
#endif
      break;
    case SYS_SAVE: {
      char saveStatus[20];
      formatSessionSaveStatus(saveStatus, sizeof(saveStatus));
      flushSaveWithStatus(saveStatus);
      resumeAfterMenu();
      break;
    }
    case SYS_EJECT_SD:
#if ENABLE_SD_ROM_MENU
      safeEjectSdFromMenu();
      resumeAfterMenu();
#else
      resumeAfterMenu();
#endif
      break;
    case SYS_SLEEP:
      enterSoftSleep();
      rememberControlState(readButtons());
      break;
    case SYS_RESUME:
    default:
      setOverlayStatus("RESUME", 700);
      resumeAfterMenu();
      break;
  }
}

static void drawFrameScaled() {
  tft.startWrite();
  tft.setAddrWindow(frameRect.x, frameRect.y, frameRect.w, frameRect.h);

  if (displayMode == DISPLAY_FAST) {
    for (int srcY = 0; srcY < GB_HEIGHT; ++srcY) {
      tft.pushPixels(gbFrame + (srcY * GB_WIDTH), GB_WIDTH);
    }
    tft.endWrite();
    return;
  }

  if (displayMode == DISPLAY_BIG) {
    int outY = 0;
    for (int srcY = 0; srcY < GB_HEIGHT; ++srcY) {
      const uint16_t* src = gbFrame + (srcY * GB_WIDTH);
      int outX = 0;
      for (int srcX = 0; srcX < GB_WIDTH; ++srcX) {
        scaledLine[outX++] = src[srcX];
        scaledLine[outX++] = src[srcX];
      }
      tft.pushPixels(scaledLine, frameRect.w);
      outY++;
      if (srcY % 3 != 2) {
        tft.pushPixels(scaledLine, frameRect.w);
        outY++;
      }
    }
    tft.endWrite();
    return;
  }

  if (displayMode == DISPLAY_BALANCED) {
    for (int srcY = 0; srcY < GB_HEIGHT; ++srcY) {
      const uint16_t* src = gbFrame + (srcY * GB_WIDTH);
      int outX = 0;
      for (int srcX = 0; srcX < GB_WIDTH; srcX += 2) {
        scaledLine[outX++] = src[srcX];
        scaledLine[outX++] = src[srcX + 1];
        scaledLine[outX++] = src[srcX + 1];
      }
      tft.pushPixels(scaledLine, frameRect.w);
      if (srcY & 1) {
        tft.pushPixels(scaledLine, frameRect.w);
      }
    }
    tft.endWrite();
    return;
  }

  for (int srcY = 0; srcY < GB_HEIGHT; ++srcY) {
    const uint16_t* src = gbFrame + (srcY * GB_WIDTH);
    int outX = 0;
    for (int srcX = 0; srcX < GB_WIDTH; srcX += 2) {
      scaledLine[outX++] = src[srcX];
      scaledLine[outX++] = src[srcX + 1];
      scaledLine[outX++] = src[srcX + 1];
    }
    tft.pushPixels(scaledLine, frameRect.w);
    if (srcY & 1) {
      tft.pushPixels(scaledLine, frameRect.w);
    }
  }
  tft.endWrite();
}

static void updateFpsCounter() {
  fpsFrames++;
  const uint32_t now = millis();
  if (now - fpsWindowStart >= 1000) {
    currentFps = fpsFrames;
    fpsFrames = 0;
    fpsWindowStart = now;
  }
}

static void drawMarginHud() {
  if (displayMode == DISPLAY_BIG) {
    return;
  }

  char fpsLabel[12];
  snprintf(fpsLabel, sizeof(fpsLabel), "%u FPS", currentFps);
  if (strcmp(fpsLabel, lastMarginFps) != 0) {
    tft.fillRect(0, 0, 58, 14, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(fpsLabel, 2, 2, 1);
    strncpy(lastMarginFps, fpsLabel, sizeof(lastMarginFps) - 1);
    lastMarginFps[sizeof(lastMarginFps) - 1] = '\0';
  }

  const bool showStatus = statusUntil > millis();
  const char* nextStatus = showStatus ? statusText : "";
  if (strcmp(nextStatus, lastMarginStatus) != 0) {
    tft.fillRect(96, 0, 128, 14, TFT_BLACK);
    if (showStatus) {
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(nextStatus, 98, 2, 1);
    }
    strncpy(lastMarginStatus, nextStatus, sizeof(lastMarginStatus) - 1);
    lastMarginStatus[sizeof(lastMarginStatus) - 1] = '\0';
  }

  drawMarginQuote();
}

static void rememberControlState(uint16_t physical) {
  previousButtons = physical;
  previousLShoulderPressed = controllerLShoulderPressed;
  previousRShoulderPressed = controllerRShoulderPressed;
  previousLTriggerPressed = controllerLTriggerPressed;
  previousRTriggerPressed = controllerRTriggerPressed;
  previousThumbRPressed = controllerThumbRPressed;
}

void setup() {
  Serial.begin(115200);

  setupSpeakerTest();

  tft.init();
  tft.setSwapBytes(true);
  tft.setRotation(1);
  ledcSetup(TFT_BL_CHANNEL, 12000, 8);
  ledcAttachPin(TFT_BL, TFT_BL_CHANNEL);
  applyBrightness();
  tft.fillScreen(TFT_BLACK);
  showStatus("ESP32 GB glue", "Starting...");

  gbFrame = static_cast<uint16_t*>(malloc(GB_WIDTH * GB_HEIGHT * sizeof(uint16_t)));
  if (!gbFrame) {
    showStatus("No RAM for frame");
    while (true) {
      delay(100);
    }
  }

  BP32.setup(&onConnectedGamepad, &onDisconnectedGamepad);
  setupPushbuttons();
  prefs.begin("gbmenu", false);
  prefs.getString("game", "").toCharArray(currentGameKey, sizeof(currentGameKey));
#if ENABLE_SD_ROM_MENU
  runSdLauncher();
#else
  strncpy(currentGameKey, "single", sizeof(currentGameKey) - 1);
  currentGameKey[sizeof(currentGameKey) - 1] = '\0';
  prefs.putString("game", currentGameKey);
#endif

  if (!loadRomFromFlash()) {
    while (true) {
      BP32.update();
      delay(100);
    }
  }

  showStatus("Ready", "Pair controller");
  delay(800);
  sessionStartMs = millis();
  updateFrameRect();
  drawBackdrop();
  fpsWindowStart = millis();
}

void loop() {
  BP32.update();
  gbCoreAudioTick();

  const uint16_t physical = readButtons();
  const uint16_t pressedNow = physical & ~previousButtons;
  const bool selectPressedNow = pressedNow & GB_BTN_SELECT;
  const bool lShoulderPressedNow = controllerLShoulderPressed && !previousLShoulderPressed;
  const bool rShoulderPressedNow = controllerRShoulderPressed && !previousRShoulderPressed;
  const bool lTriggerPressedNow = controllerLTriggerPressed && !previousLTriggerPressed;
  const bool rTriggerPressedNow = controllerRTriggerPressed && !previousRTriggerPressed;
  const bool thumbRPressedNow = controllerThumbRPressed && !previousThumbRPressed;
  const bool systemShoulderComboPressedNow =
      controllerLShoulderPressed && controllerRShoulderPressed &&
      (!previousLShoulderPressed || !previousRShoulderPressed);
  const bool systemStartSelectPressedNow =
      (physical & (GB_BTN_START | GB_BTN_SELECT)) == (GB_BTN_START | GB_BTN_SELECT) &&
      (pressedNow & (GB_BTN_START | GB_BTN_SELECT));
  const bool wakePressedNow = pressedNow || thumbRPressedNow || lShoulderPressedNow ||
                              rShoulderPressedNow || lTriggerPressedNow || rTriggerPressedNow;

  if (softSleeping) {
    if (wakePressedNow) {
      wakeSoftSleep();
    }
    rememberControlState(physical);
    delay(20);
    return;
  }

  if (systemShoulderComboPressedNow || systemStartSelectPressedNow) {
    openSystemMenu();
    return;
  }

  if (thumbRPressedNow) {
    enterSoftSleep();
    rememberControlState(physical);
    return;
  }

  if (selectPressedNow) {
    char saveStatus[20];
    formatSessionSaveStatus(saveStatus, sizeof(saveStatus));
    flushSaveWithStatus(saveStatus);
  }

  if (rTriggerPressedNow) {
    cycleDisplayMode(1);
    rumbleAll(70, 0x35, 0x20);
  }
  if (lTriggerPressedNow) {
    cycleDisplayMode(-1);
    rumbleAll(70, 0x35, 0x20);
  }

  if (rShoulderPressedNow && brightnessLevel < sizeof(brightnessValues) - 1) {
    brightnessLevel++;
    applyBrightness();
    setOverlayStatus("BRIGHT+", 1200);
    rumbleAll(45, 0x25, 0x15);
  }
  if (lShoulderPressedNow && brightnessLevel > 0) {
    brightnessLevel--;
    applyBrightness();
    setOverlayStatus("BRIGHT-", 1200);
    rumbleAll(45, 0x25, 0x15);
  }

  rememberControlState(physical);

  uint16_t emuButtons = physical & ~GB_BTN_SELECT;
  gbCoreSetButtons(filterButtons(emuButtons));
  if (gbCoreRunFrame(gbFrame)) {
    updateFpsCounter();
    const uint32_t hash = frameHash();
    const bool fullStatusVisible = displayMode == DISPLAY_BIG && statusUntil > millis();
    const bool shouldDrawFrame = hash != lastDrawHash ||
                                 displayMode != lastDrawMode ||
                                 (displayMode == DISPLAY_BIG && currentFps != lastDrawFps) ||
                                 fullStatusVisible != lastFullStatusVisible;
    if (shouldDrawFrame) {
      drawHudIntoFrame();
      drawFrameScaled();
      lastDrawHash = hash;
      lastDrawFps = currentFps;
      lastDrawMode = displayMode;
      lastFullStatusVisible = fullStatusVisible;
    }
    drawMarginHud();
  }
}
