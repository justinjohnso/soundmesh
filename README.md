# Meshnet Audio - Wireless Audio Streaming System

A wireless audio streaming system for XIAO ESP32-S3 devices using UDP broadcast over WiFi. Features real-time audio transmission with visual feedback via OLED displays and PWM audio output.

## 🎯 Project Overview

This project implements a minimal viable product (MVP) for wireless audio streaming between ESP32-S3 devices:

- **TX Device**: Generates 440Hz sine wave tone and broadcasts via WiFi SoftAP
- **RX Device**: Connects to TX SoftAP and receives audio packets with PWM output
- **Visual Feedback**: SSD1306 OLED displays show transmission status and packet counts
- **Real-time Performance**: 16kHz sample rate, 16-bit PCM, 10ms packet intervals

## 🔧 Hardware Requirements

### Target Hardware
- **XIAO ESP32-S3** boards (minimum 2 units)
- **SSD1306 0.91" OLED displays** (128x32, I2C interface) - SKU: MC091GX
- **Audio output components**: 1kΩ resistor, 10µF capacitor for PWM audio filter

### Wiring Diagrams

**OLED Display (Both TX and RX):**
```
XIAO ESP32-S3    SSD1306 OLED
-------------    ------------
3.3V          -> VCC
GND           -> GND  
GPIO5         -> SDA
GPIO6         -> SCL  ⚠️ Critical: Use GPIO6, not GPIO7
```

**PWM Audio Output (RX Device Only):**
```
GPIO1 -> 1kΩ resistor -> + side of 10µF capacitor -> Audio output (3.5mm jack tip)
                      -> - side of 10µF capacitor -> GND (3.5mm jack sleeve)
```

## 🏗️ Build System & Environment

### Current Build System: PlatformIO (Recommended)

**Prerequisites:**
- **PlatformIO Core** or **PlatformIO IDE** (VS Code extension)
- **macOS/Linux development environment**
- **Serial/USB drivers** for XIAO ESP32-S3

**Quick Start:**
```bash
# Install PlatformIO IDE extension in VS Code
code --install-extension platformio.platformio-ide

# Open the PlatformIO workspace
code meshnet-audio-pio.code-workspace

# Build and upload using VS Code tasks:
# - "pio: full tx workflow (101)"
# - "pio: full rx workflow (2101)"
```

See `firmware/platformio/README.md` for complete PlatformIO documentation.

### Legacy Build System: ESP-IDF

The original ESP-IDF build system is still available in `firmware/idf/` but is no longer the primary development method.

<details>
<summary>ESP-IDF Setup Instructions (Legacy)</summary>

**Prerequisites:**
- **ESP-IDF v5.5.1** (tested version)
- **Python 3.13.7** (or compatible)

**Setup:**
```bash
# Install ESP-IDF (if not already installed)
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh

# Source environment (required for every terminal session)
source ~/esp/esp-idf/export.sh
```

</details>

## 🔨 Building and Flashing

### Using PlatformIO (Recommended)

**VS Code Tasks:**

Open `meshnet-audio-pio.code-workspace` in VS Code and use the integrated tasks:

**Quick Workflow:**
- `pio: full tx workflow (101)` - Complete TX build and flash
- `pio: full rx workflow (2101)` - Complete RX build and flash

**Individual Tasks:**
- Build: `pio: build tx` / `pio: build rx`
- Upload: `pio: upload tx (101)` / `pio: upload rx (2101)`
- Monitor: `pio: monitor tx (101)` / `pio: monitor rx (2101)`
- Clean: `pio: clean tx` / `pio: clean rx`

**Command Line:**
```bash
# Build TX
cd firmware/platformio/tx
pio run

# Upload and monitor TX
pio run --target upload --target monitor

# Build RX
cd firmware/platformio/rx
pio run

# Upload and monitor RX
pio run --target upload --target monitor
```

### Using ESP-IDF (Legacy)

<details>
<summary>ESP-IDF Build Instructions</summary>

**⚠️ Critical Workarounds for ESP-IDF:**

Path issues with spaces require copying to `/tmp`:
```bash
# Copy projects to /tmp (avoid path issues)
cp -r "firmware/idf/apps/tx" /tmp/meshnet_tx
cp -r "firmware/idf/apps/rx" /tmp/meshnet_rx

# Build and flash
cd /tmp/meshnet_tx
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

**VS Code Tasks (ESP-IDF):**
- `idf: full tx workflow (101)` - Complete build and flash for TX
- `idf: full rx workflow (2101)` - Complete build and flash for RX

See legacy documentation in `firmware/idf/` for complete ESP-IDF instructions.

</details>

### Build Commands
```bash
# Source ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Copy projects to /tmp (avoid path issues)
cp -r "firmware/idf/apps/tx" /tmp/meshnet_tx
cp -r "firmware/idf/apps/rx" /tmp/meshnet_rx

# Build TX device
cd /tmp/meshnet_tx
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash

# Build RX device  
cd /tmp/meshnet_rx
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

### Monitoring Devices
```bash
# Monitor TX device
cd /tmp/meshnet_tx
idf.py -p /dev/cu.usbmodem1101 monitor

# Monitor RX device
cd /tmp/meshnet_rx  
idf.py -p /dev/cu.usbmodem101 monitor
```

### VS Code Tasks (macOS-friendly)

Tasks in `.vscode/tasks.json` automatically copy the TX/RX apps to `/tmp` (to avoid path-with-spaces issues) and source `~/esp/esp-idf/export.sh` before running any `idf.py` command.

- Build in /tmp:
    - idf: build tmp tx
    - idf: build tmp rx

- Flash + auto-monitor (prompts for serial port):
    - idf: flash tmp tx
    - idf: flash tmp rx

- Flash + auto-monitor (preset common ports):
    - idf: flash tmp tx (1101)
    - idf: flash tmp rx (101)

- Monitor only (prompts):
    - idf: monitor tmp tx
    - idf: monitor tmp rx

- Monitor only (preset ports):
    - idf: monitor tmp tx (1101)
    - idf: monitor tmp rx (101)

- Maintenance:
    - idf: fullclean tx
    - idf: fullclean rx

Notes

- The “flash” tasks automatically open `idf.py monitor` after a successful flash.
- The “build tmp …” tasks ensure a fresh configure by cleaning `/tmp/.../build` first.
- If your serial device IDs change, use the prompting variants; otherwise the preset (1101/101) tasks are one-click.

Recommended flow (multi-board)

1) Build both in /tmp:
     - Run idf: build tmp tx
     - Run idf: build tmp rx
2) Flash each and auto-open monitors:
     - Run idf: flash tmp tx (1101)
     - Run idf: flash tmp rx (101)
3) Iterate quickly:
     - Re-run the build tmp task that changed, then re-run the matching flash task. The monitor will re-open automatically.

Troubleshooting

- If you see mbedtls symlink "File exists" errors, run:
    - idf: fullclean rx (or tx) in the original app folder, then use the /tmp build tasks again.
    - Or remove `/tmp/meshnet_rx/build` (or tx) and rebuild.

## 📋 Project Structure

```
meshnet-audio/
├── firmware/
│   ├── platformio/           # 🔧 PlatformIO build system (PRIMARY)
│   │   ├── tx/              # Transmitter project
│   │   │   ├── platformio.ini
│   │   │   ├── src/main.c
│   │   │   └── sdkconfig.defaults
│   │   ├── rx/              # Receiver project
│   │   │   ├── platformio.ini
│   │   │   ├── src/main.c
│   │   │   └── sdkconfig.defaults
│   │   ├── lib/             # Shared component libraries
│   │   │   ├── audio_board/
│   │   │   ├── ctrl_plane/
│   │   │   ├── mesh_stream/
│   │   │   └── usb_audio/
│   │   └── README.md        # PlatformIO documentation
│   └── idf/                 # Legacy ESP-IDF build system
│       ├── apps/
│       │   ├── tx/
│       │   └── rx/
│       └── components/
├── Documentation/
│   └── Posts/               # Development blog posts
├── meshnet-audio-pio.code-workspace  # VS Code workspace (PRIMARY)
├── meshnet-audio.code-workspace       # Legacy ESP-IDF workspace
└── README.md                # This file
```

## 🎛️ System Configuration

### Network Configuration
- **WiFi Mode**: SoftAP (TX) + STA (RX)
- **Network Name**: `MeshAudioAP`
- **Password**: `meshpass123`
- **Channel**: 6
- **IP Range**: 192.168.4.x (DHCP)

### Audio Configuration
- **Sample Rate**: 16kHz
- **Bit Depth**: 16-bit PCM
- **Packet Size**: 320 bytes (160 samples × 2 bytes)
- **Packet Interval**: 10ms (real-time streaming)
- **Test Tone**: 440Hz sine wave

### Display Configuration
- **OLED Type**: SSD1306 128x32
- **I2C Address**: 0x3C
- **I2C Frequency**: 400kHz
- **Update Rate**: TX every 50 packets (~500ms), RX every 25 packets (~250ms)

## 🔧 Hardware-Specific Notes

### XIAO ESP32-S3 Pinout
```
⚠️ CRITICAL PIN ASSIGNMENTS:
- SDA: GPIO5 (confirmed working)
- SCL: GPIO6 (NOT GPIO7 - common mistake!)
- PWM Audio: GPIO1
- Available GPIOs: 2, 3, 4, 8, 9, 10 (for expansion)
```

### SSD1306 OLED Display (MC091GX)
- **Voltage**: Use 3.3V (NOT 5V)
- **Size**: 128x32 pixels (4 pages × 8 pixels each)
- **Interface**: I2C only
- **Addressing**: Horizontal mode, column 0-127, page 0-3

### Power Requirements
- **Operating Voltage**: 3.3V logic
- **Current Draw**: ~200mA during WiFi transmission
- **USB Power**: Sufficient for development/demo

## ⚠️ Common Issues & Solutions

### Build Issues

**Problem**: `pio: command not found`
```bash
# Solution: Install PlatformIO
pip install platformio
# Or via VS Code extension
code --install-extension platformio.platformio-ide
```

**Problem**: Library dependencies not found
```bash
# Solution: PlatformIO will auto-fetch on first build
cd firmware/platformio/tx
pio run  # Dependencies downloaded automatically
```

**Problem**: ESP-ADF components not available
```bash
# Solution: Clean rebuild to fetch ESP-ADF
pio run --target clean
pio run  # ESP-ADF v2.6 will be fetched automatically
```

**Problem**: Serial port permission denied (macOS/Linux)
```bash
# Solution: Add user to dialout group (Linux)
sudo usermod -a -G dialout $USER
# Or use sudo for upload (not recommended)
```

### Legacy ESP-IDF Build Issues

<details>
<summary>ESP-IDF Troubleshooting</summary>

**Problem**: `zsh: command not found: idf.py`
```bash
# Solution: Source ESP-IDF environment
source ~/esp/esp-idf/export.sh
```

**Problem**: CMake errors with "CONFIG_X is undefined"
```bash
# Solution: Clean rebuild
rm -f sdkconfig
idf.py build
```

**Problem**: Build fails in paths with spaces
```bash
# Solution: Use /tmp directory
cp -r path/with/spaces /tmp/project_name
cd /tmp/project_name
```

</details>

### Hardware Issues
**Problem**: OLED shows cursor/fuzz instead of content
```bash
# Check:
# 1. Wiring: GPIO5=SDA, GPIO6=SCL, 3.3V power
# 2. I2C address: 0x3C (most common)
# 3. Display initialization in logs
```

**Problem**: No audio output from PWM
```bash
# Check:
# 1. RC filter: 1kΩ + 10µF capacitor
# 2. GPIO1 connection
# 3. Packet reception in RX logs
```

**Problem**: Devices won't flash
```bash
# Solutions:
# 1. Unplug/replug USB
# 2. Check port with: ls /dev/cu.usbmodem*
# 3. Try lower baud: idf.py -b 115200 flash
# 4. Check USB cable (data, not just power)
```

### Network Issues
**Problem**: RX can't connect to TX
```bash
# Check:
# 1. TX SoftAP started (look for "Started SoftAP" in logs)
# 2. Both devices using same credentials
# 3. WiFi channel availability
```

## 🎯 Development Status

### ✅ Working Features
- [x] WiFi SoftAP + STA communication
- [x] Real-time UDP audio streaming (16kHz, 16-bit)
- [x] OLED display initialization and basic patterns
- [x] PWM audio output on RX
- [x] Stable packet transmission (verified 2000+ packets)
- [x] Clean build system (no PSRAM errors)

### 🚧 Future Enhancements
- [ ] I2S audio input/output for better quality
- [ ] Real microphone input on TX
- [ ] Multiple RX device support
- [ ] ESP-ADF integration for advanced audio processing
- [ ] True mesh networking capabilities
- [ ] Audio codec support (compression)

## 📝 Development Notes

### Build Performance (PlatformIO)
- **Initial build time**: ~45-60 seconds (fetches dependencies)
- **Incremental build**: ~5-15 seconds
- **Flash time**: ~10-15 seconds per device
- **Automatic dependency management**: Libraries fetched once and cached

### Build Performance (ESP-IDF Legacy)
- **Clean build time**: ~30-45 seconds
- **Incremental build**: ~5-10 seconds
- **Flash time**: ~10-15 seconds per device
- **Manual dependency management**: Requires component copying

### Memory Usage
- **Flash**: ~787KB used of 2MB available (39%)
- **RAM**: ~277KB available for dynamic allocation
- **PSRAM**: Disabled (caused boot issues on XIAO ESP32-S3)

### Known Warnings (Safe to Ignore)
```
W (331) spi_flash: Detected size(8192k) larger than header(2048k)
W (343) i2c: Old driver, migrate to driver/i2c_master.h
```

## 🤝 Contributing

### Code Style
- Follow ESP-IDF conventions
- Use descriptive variable names
- Comment complex audio/network logic
- Test on actual hardware before committing

### Testing Checklist
- [ ] Clean build without errors
- [ ] Both devices flash successfully
- [ ] OLED displays show expected patterns
- [ ] UDP packet transmission verified
- [ ] Audio output functional (PWM)
- [ ] No memory leaks or crashes during 10+ minute runs

## 📄 License

This project is developed for educational purposes as part of the NYU ITP Project Development Studio course.

## 📞 Support

For build issues or hardware questions, check the development logs in `docs/posts/` or refer to the ESP-IDF documentation.

---

**Last Updated**: October 16, 2025  
**Build System**: PlatformIO + ESP-IDF Framework (Primary), ESP-IDF Native (Legacy)  
**ESP-IDF Version**: v5.5.1  
**ESP-ADF Version**: v2.6 (via PlatformIO)  
**Hardware**: XIAO ESP32-S3, SSD1306 OLED  
**Status**: PlatformIO Migration Complete, MVP Audio Streaming Verified ✅