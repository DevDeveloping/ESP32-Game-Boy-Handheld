param(
    [string]$Fqbn = "esp32-bluepad32:esp32:esp32:UploadSpeed=115200,PartitionScheme=noota_3g",
    [string]$Port,
    [string]$ArduinoCli = "arduino-cli",
    [string]$Esptool = "esptool",
    [string]$Chip = "esp32",
    [switch]$SingleGame,
    [switch]$Pushbuttons,
    [string]$RomPath
)

$ErrorActionPreference = "Stop"
$workDir = Join-Path $env:TEMP ("esp32-gameboy-build-" + [guid]::NewGuid().ToString("N"))
$sourceRoot = Join-Path $workDir "source"
$sketchDir = Join-Path $sourceRoot "esp32_ili9341_bt_gb_glue"
$buildDir = Join-Path $workDir "build"

if ($RomPath -and -not $SingleGame) {
    throw "-RomPath requires -SingleGame."
}
if ($RomPath -and -not $Port) {
    throw "Provide -Port COMx to flash the single ROM after uploading the firmware."
}

New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "esp32_ili9341_bt_gb_glue") -Destination $sourceRoot -Recurse

$configPath = Join-Path $sketchDir "config.h"
$configText = Get-Content -LiteralPath $configPath -Raw
$sdMenuValue = if ($SingleGame) { "0" } else { "1" }
$buttonValue = if ($Pushbuttons) { "1" } else { "0" }
$configText = [regex]::Replace($configText, "(?m)^#define ENABLE_SD_ROM_MENU [01]\r?$", "#define ENABLE_SD_ROM_MENU $sdMenuValue")
$configText = [regex]::Replace($configText, "(?m)^#define ENABLE_GPIO_PUSHBUTTONS [01]\r?$", "#define ENABLE_GPIO_PUSHBUTTONS $buttonValue")
[System.IO.File]::WriteAllText($configPath, $configText, [System.Text.UTF8Encoding]::new($false))

if ($RomPath) {
    $romFullPath = (Resolve-Path -LiteralPath $RomPath).Path
    $romBytes = [System.IO.File]::ReadAllBytes($romFullPath)
    if ($romBytes.Length -lt 0x150) {
        throw "The ROM file is too small to contain a Game Boy header."
    }
    $romSizes = @(32768, 65536, 131072, 262144, 524288, 1048576, 2097152)
    $romSizeCode = [int]$romBytes[0x148]
    if ($romSizeCode -ge $romSizes.Count -or $romBytes.Length -ne $romSizes[$romSizeCode]) {
        throw "The ROM file size does not match its Game Boy header. Use an unmodified .gb ROM dump."
    }
    if ($romBytes[0x143] -eq 0xC0) {
        throw "This build supports original Game Boy ROMs, not Game Boy Color-only ROMs."
    }
}

& $ArduinoCli compile --fqbn $Fqbn --build-path $buildDir $sketchDir
if ($LASTEXITCODE -ne 0) {
    throw "Arduino compile failed with exit code $LASTEXITCODE"
}

if ($Port) {
    & $ArduinoCli upload --fqbn $Fqbn --port $Port --input-dir $buildDir
    if ($LASTEXITCODE -ne 0) {
        throw "Arduino upload failed with exit code $LASTEXITCODE"
    }
}

if ($RomPath) {
    & $Esptool --chip $Chip --port $Port --baud 115200 write_flash --flash_mode keep --flash_size keep 0x190000 $romFullPath
    if ($LASTEXITCODE -ne 0) {
        throw "ROM flash failed with exit code $LASTEXITCODE. Check -Esptool and the serial port."
    }
}
