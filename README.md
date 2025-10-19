# Meshnet Audio - Wireless Audio Streaming System

A wireless audio streaming system for XIAO ESP32-S3 devices using UDP broadcast over WiFi with real-time audio transmission and visual feedback via OLED displays.

## 🎯 Project Overview

MeshNet Audio implements a modular wireless audio streaming prototype with TX (transmitter) and RX (receiver) units:

- **TX Device**: Captures audio (USB/Aux/Tone) and broadcasts via WiFi SoftAP
- **RX Device**: Connects to TX and outputs audio via I2S DAC (UDA1334)
- **Visual Feedback**: SSD1306 OLED displays show network stats and audio status
- **Real-time Performance**: 16kHz sample rate, 16-bit mono PCM, 10ms packets

## 🔧 Hardware Requirements

### Target Hardware
- **XIAO ESP32-S3** boards (minimum 2 units)
- **SSD1306 OLED displays** (128x64, I2C interface, address 0x3C)
- **UDA1334 I2S DAC** (RX only) for audio output
- **PCF8591 ADC** (TX only, future) for aux input

### Wiring

**OLED Display (Both TX and RX):**
```
XIAO ESP32-S3    SSD1306 OLED
-------------    ------------
3.3V          -> VCC
GND           -> GND  
GPIO5         -> SDA
GPIO6         -> SCL
```

**UDA1334 I2S DAC (RX Only):**
```
XIAO ESP32-S3    UDA1334
-------------    -------
3.3V          -> VIN
GND           -> GND
GPIO7         -> BCK (bit clock)
GPIO8         -> WS (word select)
GPIO9         -> DIN (data in)
```

**Button (Both TX and RX):**
```
GPIO1 -> Button -> GND (with internal pull-up)
```

## 🏗️ Build System - PlatformIO

**Prerequisites:**
- **PlatformIO Core** or **VS Code + PlatformIO IDE extension**
- **macOS/Linux/Windows** development environment
- **USB drivers** for XIAO ESP32-S3

**Quick Start:**
```bash
# Clone repository
git clone https://github.com/justinjohnso-itp/meshnet-audio.git
cd meshnet-audio

# Build TX
pio run -e tx

# Build RX  
pio run -e rx

# Upload TX (adjust port as needed)
pio run -e tx -t upload --upload-port /dev/cu.usbmodem101

# Upload RX
pio run -e rx -t upload --upload-port /dev/cu.usbmodem2101

# Monitor serial output
pio device monitor -b 115200
```

See [AGENTS.md](AGENTS.md) for complete build commands and development guidelines.

## 📋 Project Structure

```
meshnet-audio/
├── firmware/
│   ├── tx/
│   │   ├── src/main.c           # TX orchestration
│   │   └── sdkconfig.defaults   # TX-specific config
│   ├── rx/
│   │   ├── src/main.c           # RX orchestration
│   │   └── sdkconfig.defaults   # RX-specific config
│   └── lib/                     # Shared libraries
│       ├── network/             # WiFi mesh & UDP (Network Layer)
│       ├── audio/               # USB, I2S, tone gen (Audio Layer)
│       ├── control/             # Display, buttons (Control Layer)
│       └── config/              # Pin definitions & constants
├── platformio.ini               # PlatformIO configuration
├── AGENTS.md                    # Development guide
└── README.md                    # This file
```

## 🎯 Architecture

### 3-Layer Design

**Network Layer** (`firmware/lib/network/`):
- WiFi SoftAP (TX) and STA (RX) configuration
- UDP broadcast/receive on port 3333
- RSSI and latency monitoring

**Audio Layer** (`firmware/lib/audio/`):
- USB audio input (TinyUSB UAC - in progress)
- Tone generator (440Hz test signal)
- I2S audio output (UDA1334 DAC)
- Ring buffers for audio streaming

**Control Layer** (`firmware/lib/control/`):
- SSD1306 display management
- Button handling (short/long press)
- Status management and UI

### TX Unit Features
- **Input Modes**: USB audio (future), Aux (future), Tone generator (440Hz)
- **Display Views**: Network (connected nodes) or Audio (input mode + status)
- **Button Control**: Short press = cycle views, Long press = change input mode

### RX Unit Features
- **Audio Output**: I2S via UDA1334 DAC to headphones/speakers
- **Display Views**: Network (RSSI/latency) or Audio (receiving/bandwidth)
- **Button Control**: Short press = cycle display views

## 🎛️ System Configuration

### Network Configuration
- **WiFi SSID**: `MeshNet-Audio`
- **Password**: `meshnet123`
- **UDP Port**: 3333
- **Protocol**: UDP broadcast

### Audio Configuration
- **Sample Rate**: 16kHz
- **Bit Depth**: 16-bit PCM
- **Channels**: Mono
- **Packet Size**: 320 bytes (160 samples × 2 bytes)
- **Packet Interval**: 10ms
- **Test Tone**: 440Hz sine wave

### Display Configuration
- **Type**: SSD1306 128x64
- **I2C Address**: 0x3C
- **I2C Frequency**: 400kHz
- **Pins**: GPIO5 (SDA), GPIO6 (SCL)

## ⚠️ Common Issues & Solutions

### Build Issues

**Problem**: `pio: command not found`
```bash
# Install PlatformIO
pip install platformio
```

**Problem**: ESP-IDF version mismatch
```bash
# Clean and rebuild
pio run -e tx -t clean
pio run -e tx
```

### Hardware Issues

**Problem**: OLED display not working
- Check wiring: GPIO5=SDA, GPIO6=SCL
- Verify I2C address: 0x3C
- Check 3.3V power connection

**Problem**: No audio output
- Verify UDA1334 wiring (BCK=GPIO7, WS=GPIO8, DIN=GPIO9)
- Check I2S initialization in logs
- Verify RX is receiving packets

**Problem**: Button not responding
- Check GPIO1 connection to button
- Verify button grounds to GND
- Check logs for button events

### Network Issues

**Problem**: RX can't connect to TX
- Verify TX SoftAP started (check logs)
- Ensure both devices use same SSID/password
- Check WiFi channel availability

## 🎯 Development Status

### ✅ Implemented
- [x] 3-layer architecture (network/audio/control)
- [x] WiFi SoftAP (TX) + STA (RX)
- [x] UDP broadcast audio streaming
- [x] I2S audio output (UDA1334)
- [x] Tone generator (440Hz)
- [x] SSD1306 display framework
- [x] Button handling (short/long press)
- [x] Ring buffer implementation
- [x] PlatformIO build system

### 🚧 In Progress
- [ ] TinyUSB UAC audio input
- [ ] SSD1306 display rendering
- [ ] PCF8591 ADC for aux input
- [ ] Network statistics (RSSI, latency)
- [ ] Bandwidth monitoring

### 🎯 Future Features
- [ ] Multiple RX device support
- [ ] Audio codec/compression
- [ ] True mesh networking
- [ ] Web-based configuration
- [ ] Battery power optimization

## 🤝 Contributing

### Code Style
- Follow ESP-IDF conventions
- Use 3-layer architecture (network/audio/control)
- Keep functions focused and modular
- Comment complex logic only
- See [AGENTS.md](AGENTS.md) for detailed guidelines

### Testing Checklist
- [ ] Clean build without errors
- [ ] Both devices flash successfully
- [ ] Network connection established
- [ ] Audio streaming functional
- [ ] Display updates correctly
- [ ] Button events handled
- [ ] No memory leaks (10+ minute test)

## 📄 License

Educational project developed for NYU ITP Project Development Studio.

## 📞 Support

- **Documentation**: See [AGENTS.md](AGENTS.md) for build commands
- **Hardware**: Check wiring diagrams above
- **Issues**: Review [Common Issues](#-common-issues--solutions)

---

**Last Updated**: January 2025
**Build System**: PlatformIO + ESP-IDF Framework (espressif32@~6.6.0)
**Hardware**: XIAO ESP32-S3, SSD1306 OLED, UDA1334 I2S DAC
**Status**: Core architecture implemented, USB audio in progress
