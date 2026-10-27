# ESP32 Game Boy Handheld

An Arduino project for ESP32-family boards with an SPI ILI9341 240 x 320 display, a separate SPI microSD module, and a Bluetooth controller supported by Bluepad32.

This repository contains the firmware and hardware configuration. It does not include game ROMs, save files, or cover art. Add only ROMs you are legally allowed to use.

## About this project

This project turns an inexpensive ESP32 board into a small Game Boy handheld using parts that are easy to find: an ILI9341 screen, a microSD module, and either a Bluetooth controller or simple pushbuttons. It runs on the original ESP32-WROOM-32 without PSRAM, so it does not require a Raspberry Pi or a more expensive single-board computer.

The firmware keeps ROM data in a dedicated flash partition and runs it from there. The SD-menu build can hold multiple games and keep saves on the card. A single-game build skips the SD card and runs one user-supplied ROM stored in flash. It is a hands-on example of embedded emulation, SPI displays and storage, Bluetooth gamepads, and persistent saves on constrained hardware.

It emulates original monochrome Game Boy games through Peanut-GB. It does not emulate Game Boy Color, Game Boy Advance, NES, or other systems.

## Hardware and wiring

### ILI9341 display

| Display signal | ESP32 GPIO |
| --- | ---: |
| SCLK | 18 |
| MOSI | 23 |
| MISO | 19 |
| CS | 5 |
| DC | 2 |
| RST | 4 |
| Backlight | 21 |

### microSD module (separate SPI bus)

| SD signal | ESP32 GPIO |
| --- | ---: |
| SCLK | 14 |
| MOSI | 13 |
| MISO | 27 |
| CS | 22 |

The speaker amplifier input connects to GPIO 25. Power the amplifier from the supply recommended for that module and connect grounds together.

### Optional pushbuttons

Enable these with `-Pushbuttons` in the build script or set `ENABLE_GPIO_PUSHBUTTONS` to `1` in `config.h`. The default ESP-WROOM-32 pin assignment is:

| Button | ESP32 GPIO |
| --- | ---: |
| Up / Down | 32 / 33 |
| Left / Right | 34 / 35 |
| A / B | 16 / 17 |
| Select / Start | 26 / 39 |

Connect each button between its GPIO and GND. GPIO 34, 35, and 39 are input-only and do not have internal pull-ups; add a 10 kOhm resistor from each of those GPIOs to 3.3 V. These pins are defaults for ESP-WROOM-32 without PSRAM. Change them in `config.h` if your board uses those pins for PSRAM or another peripheral. Bluetooth controls remain enabled alongside pushbuttons.

## ESP32 board support

The known working build uses an original ESP32 module (ESP-WROOM-32 / ESP32 DevKit) with 4 MB flash. The build script accepts a board FQBN, so you can select another board supported by the installed ESP32 + Bluepad32 package.

Bluepad32 supports several ESP32-family targets, but controller radio support differs: ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, and ESP32-H2 support BLE controllers only. The original ESP32 also supports Bluetooth Classic, which may be required by some controllers. Confirm your controller's Bluetooth mode before choosing one of those chips. See the [Bluepad32 board guide](https://bluepad32.readthedocs.io/en/latest/plat_arduino/) and [controller compatibility notes](https://bluepad32.readthedocs.io/en/latest/supported_gamepads/).

This sketch has only been tested on the original ESP32-WROOM-32. Other models may need pin changes, a compatible Bluepad32 board package, and possibly code changes for their Arduino core. Choose an ESP32 board with at least 4 MB flash. PSRAM is optional for this build.

## Build and upload

1. Install Arduino IDE or Arduino CLI.
2. Install the ESP32 + Bluepad32 board package and the `TFT_eSPI` library.
3. Configure `TFT_eSPI` to use the supplied `TFT_eSPI_Setup/User_Setup.h`.
4. Open `esp32_ili9341_bt_gb_glue/esp32_ili9341_bt_gb_glue.ino` in Arduino IDE, or use the build script below.
5. Select the exact board model, 4 MB or larger flash, and 240 MHz when those options are available. Keep `partitions.csv` beside the sketch; the ROM and saves use its `spiffs` partition.
6. Compile and upload over USB.

### Build script

The script compiles for a chosen board FQBN. Leave out `-Port` to compile only; provide the serial port to upload after compiling.

```powershell
# Known working original ESP32 build
./build.ps1

# Compile for another board; replace the value with that board's FQBN
./build.ps1 -Fqbn "YOUR_VENDOR:esp32:YOUR_BOARD"

# Compile and upload to a selected serial port
./build.ps1 -Port COM5
./build.ps1 -Fqbn "YOUR_VENDOR:esp32:YOUR_BOARD" -Port COM5

# Compile and upload with Bluetooth plus wired pushbuttons
./build.ps1 -Pushbuttons -Port COM5

# Build a one-game version without an SD card and flash your own ROM
./build.ps1 -SingleGame -RomPath "C:\Games\MyGame.gb" -Port COM5
```

Install Arduino CLI and the board package first. To find an installed board's FQBN, run `arduino-cli board listall` and use the identifier shown for your board. The default FQBN selects the known working Bluepad32 ESP32 Dev Module with a 115200 baud upload and the large ROM storage layout.

For `-SingleGame`, the script builds firmware that skips the SD launcher, uploads it, and writes the supplied ROM at the start of the included `spiffs` partition. Arduino CLI and Espressif `esptool` must be installed; if `esptool` is not on PATH, pass its full executable path with `-Esptool`. For a non-classic ESP32 chip, pass its esptool chip name with `-Chip` (for example, `-Chip esp32s3`). The ROM is read from your computer and is never copied into the repository. This partition layout supports original Game Boy ROMs up to 2 MB. Saves remain in ESP32 flash and are not backed up to SD in single-game mode.

### Changing pins for another board

The GPIO numbers in `esp32_ili9341_bt_gb_glue/config.h` and `TFT_eSPI_Setup/User_Setup.h` must match your wiring and the chosen board. Update both files together if you move any display pins. Update the SD and backlight/audio GPIO values in `config.h` when needed. Avoid GPIOs reserved for flash, USB, or boot strapping on your specific board; consult that board's pinout.

The known working project build used ESP32 + Bluepad32 board package 4.1.0 and an ESP-WROOM-32. Newer board package versions can change Arduino APIs or board options; other chip families are not guaranteed until compiled and tested on that hardware.

## SD card layout

Format the card as FAT32 and create these folders in its root:

```text
/roms/
/covers/
/saves/
```

Put Game Boy `.gb` files in `/roms/`. Cover images are optional and go in `/covers/` as `<rom-name>.bmp` (24-bit BMP). The firmware exports saves to `/saves/<rom-name>.sav` when saving or switching games. Keep the SD card inserted while playing and saving.

## Controller buttons

| Controller input | Action |
| --- | --- |
| D-pad or left stick | Move / choose menu item |
| A or X | Game A button / confirm |
| B or Y | Game B button / back |
| Select | Save |
| Start + Select, or LB + RB | Open system menu |
| LT / RT | Change display mode |
| LB / RB | Adjust brightness |
| Right-stick click (R3) | Sleep; press a controller button to wake |

In the system menu, use the D-pad to select Resume, Change Game, Save, Eject SD, or Sleep. Use A/Start to confirm and B to return.

## Source and legal notes

`peanut_gb.h` includes its upstream MIT license and attribution notices. Retain those notices when redistributing that file. This repository does not distribute Nintendo game data; ROMs and artwork are supplied by the user.
