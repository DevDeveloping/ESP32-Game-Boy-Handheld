#pragma once

#include <Arduino.h>

enum GbButton : uint16_t {
  GB_BTN_RIGHT  = 1 << 0,
  GB_BTN_LEFT   = 1 << 1,
  GB_BTN_UP     = 1 << 2,
  GB_BTN_DOWN   = 1 << 3,
  GB_BTN_A      = 1 << 4,
  GB_BTN_B      = 1 << 5,
  GB_BTN_SELECT = 1 << 6,
  GB_BTN_START  = 1 << 7,
};

bool gbCoreLoadRom();
void gbCoreUnloadRom();
void gbCoreSetButtons(uint16_t buttons);
bool gbCoreRunFrame(uint16_t* rgb565Frame);
bool gbCoreAudioBegin();
void gbCoreAudioTick();
void gbCoreAudioSilence();
void gbCoreAudioRefresh();
bool gbCoreSaveDirty();
bool gbCoreFlushSave();
uint8_t gbCoreConsumeSaveEvent();
