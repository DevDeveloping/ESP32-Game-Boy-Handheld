#pragma once

#define TFT_WIDTH 320
#define TFT_HEIGHT 240

#define GB_WIDTH 160
#define GB_HEIGHT 144

#define FRAME_W 240
#define FRAME_H 216
#define FRAME_X 40
#define FRAME_Y 16

// These GPIO defaults match an ESP-WROOM-32 DevKit. Keep the display pins in
// sync with TFT_eSPI_Setup/User_Setup.h when adapting the wiring to another board.
#define TFT_BL 21
#define TFT_BL_CHANNEL 1

#define AUDIO_PIN 25
#define AUDIO_PWM_CHANNEL 0
#define AUDIO_PWM_RESOLUTION 8
#define MENU_WAV_PATH "/menu.wav"

// Build options: set to 0/1 in the firmware config.
#define ENABLE_SD_ROM_MENU 1
#define ENABLE_GPIO_PUSHBUTTONS 0

// Optional active-low controls for an ESP-WROOM-32 without PSRAM.
// GPIO 34, 35, and 39 are input-only and need external 10k pull-ups to 3.3V.
#define BUTTON_UP_PIN 32
#define BUTTON_DOWN_PIN 33
#define BUTTON_LEFT_PIN 34
#define BUTTON_RIGHT_PIN 35
#define BUTTON_A_PIN 16
#define BUTTON_B_PIN 17
#define BUTTON_SELECT_PIN 26
#define BUTTON_START_PIN 39

#define ROM_PATH "/rom.gb"

#define SD_ROMS_DIR "/roms"
#define SD_COVERS_DIR "/covers"
#define SD_SAVES_DIR "/saves"
#define SD_SCLK 14
#define SD_MOSI 13
#define SD_MISO 27
#define SD_CS 22
#define SD_SPI_HZ 12000000
