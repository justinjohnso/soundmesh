# AGENTS.md - Meshnet Audio Development Guide

## Build Commands (PlatformIO Primary)
**Important**: Due to spaces in the project path, projects must be copied to /tmp before building.
- **Build TX**: `rsync -a --exclude .pio firmware/platformio/tx/ /tmp/meshnet_tx/ && rsync -a firmware/platformio/lib/ /tmp/meshnet_tx/../lib/ && cd /tmp/meshnet_tx && pio run`
- **Build RX**: `rsync -a --exclude .pio firmware/platformio/rx/ /tmp/meshnet_rx/ && rsync -a firmware/platformio/lib/ /tmp/meshnet_rx/../lib/ && cd /tmp/meshnet_rx && pio run`
- **Upload TX**: `cd /tmp/meshnet_tx && pio run --target upload --upload-port /dev/cu.usbmodem101`
- **Upload RX**: `cd /tmp/meshnet_rx && pio run --target upload --upload-port /dev/cu.usbmodem2101`
- **Monitor**: `pio device monitor --baud 115200`
- **VS Code Tasks**: Use `pio: full tx workflow (101)` or `pio: full rx workflow (2101)` for complete build/flash cycles

## Architecture
- **Dual Projects**: TX (transmitter) and RX (receiver) as separate PlatformIO projects in `firmware/platformio/tx` and `firmware/platformio/rx`
- **Shared Libraries**: Common components in `firmware/platformio/lib/` (audio_board, ctrl_plane, mesh_stream, usb_audio)
- **Framework**: ESP-IDF via PlatformIO, ESP-ADF v2.6 for audio processing
- **Hardware**: XIAO ESP32-S3 boards, SSD1306 OLED (I2C 0x3C), UDA1334 I2S DAC on RX
- **Network**: WiFi SoftAP (TX) + STA (RX), UDP broadcast on port 3333, 16kHz/16-bit PCM audio
- **Audio Pipeline**: USB Audio (TinyUSB) → Ring Buffer → UDP Broadcast (TX), UDP Receive → I2S Output (RX)

## Code Style (C/C++ - ESP-IDF)
- **Naming**: `camelCase` for variables/functions, `UPPER_CASE` for constants/macros, `PascalCase` for classes
- **Headers**: Use `#pragma once` or include guards
- **Memory**: Minimize dynamic allocation, check return values, use RAII principles
- **GPIO**: Define pins in central config (`I2C_MASTER_SCL_IO`, `I2S_BCK_IO`, etc.), use `volatile` for ISR variables
- **Formatting**: ESP-IDF conventions, descriptive names, comment only complex audio/network logic
- **Error Handling**: Always check hardware operation return values, log with `ESP_LOG*` macros

## GitHub Copilot Instructions
Include guidelines from `.github/copilot-instructions.md` for autonomous development patterns, documentation standards, and security practices.
