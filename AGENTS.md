# AGENTS.md - Meshnet Audio Development Guide

## Build Commands (PlatformIO)
- **Build TX**: `pio run -e tx`
- **Build RX**: `pio run -e rx`
- **Upload TX**: `pio run -e tx -t upload --upload-port /dev/cu.usbmodem101`
- **Upload RX**: `pio run -e rx -t upload --upload-port /dev/cu.usbmodem2101`
- **Monitor**: `pio device monitor -b 115200`
- **Clean**: `pio run -e tx -t clean` or `pio run -e rx -t clean`

## Architecture
- **Single PlatformIO Project**: Two environments (tx, rx) sharing libraries in `firmware/lib/`
- **3-Layer Design**:
  - **Network Layer** (`firmware/lib/network/`): WiFi mesh, UDP broadcast/receive
  - **Audio Layer** (`firmware/lib/audio/`): USB input, I2S output, tone generation, ring buffers
  - **Control Layer** (`firmware/lib/control/`): SSD1306 display, button handling, status management
- **Config** (`firmware/lib/config/`): Pin definitions and build constants
- **Framework**: ESP-IDF via PlatformIO (espressif32@~6.6.0, IDF 5.1.x compatible with ESP-ADF v2.6)
- **Hardware**: XIAO ESP32-S3 boards, SSD1306 OLED (I2C 0x3C), UDA1334 I2S DAC on RX
- **Network**: WiFi SoftAP (TX) + STA (RX), UDP broadcast on port 3333, 16kHz/16-bit mono PCM
- **Audio Pipeline**: USB/Tone/AUX → UDP Broadcast (TX), UDP Receive → I2S Output (RX)

## TX Unit Features
- Input modes: USB audio, Aux (via PCF8591 ADC - future), Tone generator (440Hz)
- Display views: Network (connected nodes) or Audio (input mode + status)
- Button: Short press = cycle views, Long press = change input mode

## RX Unit Features
- I2S audio output via UDA1334 DAC to headphones/speakers
- Display views: Network (RSSI/latency) or Audio (receiving status/bandwidth)
- Button: Short press = cycle views

## Project Structure
```
firmware/
├── tx/
│   ├── src/main.c         # TX orchestration
│   └── sdkconfig.defaults # TX-specific config
├── rx/
│   ├── src/main.c         # RX orchestration
│   └── sdkconfig.defaults # RX-specific config
└── lib/
    ├── network/           # WiFi mesh, UDP
    ├── audio/             # USB, I2S, tone gen, buffers
    ├── control/           # Display, buttons, status
    └── config/            # Pins, build constants
```

## Code Style (C/C++ - ESP-IDF)
- **Naming**: `camelCase` for variables/functions, `UPPER_CASE` for constants/macros, `PascalCase` for classes
- **Headers**: Use `#pragma once` or include guards
- **Memory**: Minimize dynamic allocation, check return values, use RAII principles
- **GPIO**: Define pins in central config (`I2C_MASTER_SCL_IO`, `I2S_BCK_IO`, etc.), use `volatile` for ISR variables
- **Formatting**: ESP-IDF conventions, descriptive names, comment only complex audio/network logic
- **Error Handling**: Always check hardware operation return values, log with `ESP_LOG*` macros

## GitHub Copilot Instructions
Include guidelines from `.github/copilot-instructions.md` for autonomous development patterns, documentation standards, and security practices.
