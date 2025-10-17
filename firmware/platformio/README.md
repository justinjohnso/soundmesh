# MeshNet Audio - PlatformIO Build System

This directory contains the PlatformIO-based build system for the MeshNet Audio project. PlatformIO provides a more streamlined development experience compared to ESP-IDF directly.

## 📁 Directory Structure

```
firmware/platformio/
├── tx/                     # Transmitter project
│   ├── platformio.ini     # TX build configuration
│   ├── src/main.c         # TX application code
│   └── sdkconfig.defaults # ESP-IDF configuration
├── rx/                     # Receiver project
│   ├── platformio.ini     # RX build configuration
│   ├── src/main.c         # RX application code
│   └── sdkconfig.defaults # ESP-IDF configuration
└── lib/                    # Shared component libraries
    ├── audio_board/       # Audio hardware abstraction
    ├── ctrl_plane/        # Mesh control messaging
    ├── mesh_stream/       # Audio streaming over mesh
    └── usb_audio/         # USB audio device support
```

## 🚀 Quick Start

### Prerequisites
- **PlatformIO Core** or **PlatformIO IDE** (VS Code extension)
- **macOS/Linux** development environment
- **Serial/USB drivers** for XIAO ESP32-S3

### Installation

**Option 1: PlatformIO IDE (Recommended)**
```bash
# Install VS Code extension
code --install-extension platformio.platformio-ide
```

**Option 2: PlatformIO Core**
```bash
# Install via pip
pip install platformio

# Or via homebrew (macOS)
brew install platformio
```

## 🔨 Building and Flashing

### Using VS Code Tasks

Open the workspace file `meshnet-audio-pio.code-workspace` and use the integrated tasks:

**Build Tasks:**
- `pio: build tx` - Build transmitter firmware
- `pio: build rx` - Build receiver firmware

**Upload Tasks:**
- `pio: upload tx (101)` - Flash TX to /dev/cu.usbmodem101
- `pio: upload rx (2101)` - Flash RX to /dev/cu.usbmodem2101

**Monitor Tasks:**
- `pio: monitor tx (101)` - Monitor TX serial output
- `pio: monitor rx (2101)` - Monitor RX serial output

**Full Workflow Tasks:**
- `pio: full tx workflow (101)` - Clean, build, upload, and monitor TX
- `pio: full rx workflow (2101)` - Clean, build, upload, and monitor RX

### Using Command Line

```bash
# Build TX
cd firmware/platformio/tx
pio run

# Build RX
cd firmware/platformio/rx
pio run

# Upload TX to device
cd firmware/platformio/tx
pio run --target upload

# Upload and monitor TX
cd firmware/platformio/tx
pio run --target upload --target monitor

# Clean build
pio run --target clean
```

## 🔧 Configuration

### Serial Ports

The default serial ports are configured in `platformio.ini`:
- **TX**: `/dev/cu.usbmodem101`
- **RX**: `/dev/cu.usbmodem2101`

To change ports, edit the `upload_port` setting in the respective `platformio.ini` file.

### Hardware Pin Configuration

Pin assignments are defined via build flags in `platformio.ini`:

**TX Device:**
- I2C SDA: GPIO5
- I2C SCL: GPIO6
- Button: GPIO4

**RX Device:**
- I2S BCK: GPIO7
- I2S WS: GPIO8
- I2S DOUT: GPIO9
- I2C SDA: GPIO5
- I2C SCL: GPIO6
- Button: GPIO4

### ESP-IDF Configuration

Additional ESP-IDF settings can be configured in `sdkconfig.defaults` files within each project directory.

## 📚 Library Dependencies

### Registry Dependencies
- **k0i05/esp_ssd1306** (v1.0.2) - SSD1306 OLED driver
- **espressif/esp_tinyusb** (v1.4.0) - USB audio support (TX only)

### ESP-ADF Integration
ESP-ADF (Audio Development Framework) v2.6 is automatically fetched via `platform_packages`:
```ini
platform_packages = 
    framework-esp-idf-adf @ https://github.com/espressif/esp-adf.git#v2.6
```

### Shared Local Libraries
Located in `firmware/platformio/lib/`:
- **audio_board** - Hardware abstraction layer
- **ctrl_plane** - Control plane messaging
- **mesh_stream** - Audio streaming implementation
- **usb_audio** - USB audio device configuration

## 🐛 Troubleshooting

### Build Issues

**Problem**: `Library not found: audio_board`
```bash
# Solution: Verify lib_extra_dirs in platformio.ini
lib_extra_dirs = ../lib
```

**Problem**: ESP-ADF components not found
```bash
# Solution: Clean and rebuild to fetch ESP-ADF
pio run --target clean
pio run
```

**Problem**: Serial port not found
```bash
# Solution: Check available ports
ls /dev/cu.usb*

# Update platformio.ini with correct port
upload_port = /dev/cu.usbmodem<YOUR_PORT>
```

### Upload Issues

**Problem**: Upload fails with "connection refused"
```bash
# Solution 1: Unplug and replug the device
# Solution 2: Lower upload speed in platformio.ini
upload_speed = 115200
```

**Problem**: Device not recognized
```bash
# Solution: Check USB drivers and cable
# macOS: Install CH340/CP2102 drivers if needed
```

### Monitor Issues

**Problem**: Garbled output in monitor
```bash
# Solution: Verify monitor_speed matches device
monitor_speed = 115200
```

**Problem**: Monitor doesn't show exception traces
```bash
# Solution: Verify esp32_exception_decoder filter is enabled
monitor_filters = esp32_exception_decoder
```

## 🔄 Migration from ESP-IDF

If you're migrating from the ESP-IDF build system:

1. **No need for `idf.py` commands** - PlatformIO handles toolchain setup
2. **No `/tmp` workarounds** - PlatformIO handles path escaping correctly
3. **Automatic dependency management** - Libraries are fetched automatically
4. **Integrated debugging** - Built-in debug support with `debug_tool = esp-builtin`

## 📊 Comparison: PlatformIO vs ESP-IDF

| Feature | PlatformIO | ESP-IDF |
|---------|-----------|---------|
| Setup | Single `pio run` | Manual `idf.py build` + environment sourcing |
| Dependencies | Automatic via `lib_deps` | Manual via `idf_component.yml` |
| Build Location | Direct in project | Requires `/tmp` workaround for paths with spaces |
| IDE Integration | Native VS Code support | Requires custom tasks |
| Library Management | PlatformIO Registry | Component manager + manual copying |
| Debugging | Built-in | Requires additional setup |

## 🎯 Development Workflow

Recommended workflow for rapid iteration:

1. **Initial Setup:**
   ```bash
   # Open workspace in VS Code
   code meshnet-audio-pio.code-workspace
   ```

2. **First Build:**
   - Run task: `pio: full tx workflow (101)`
   - Run task: `pio: full rx workflow (2101)`

3. **Iterative Development:**
   - Make code changes
   - Run task: `pio: build tx` or `pio: build rx`
   - Run task: `pio: upload and monitor tx (101)` or similar

4. **Clean Rebuild (if needed):**
   - Run task: `pio: clean tx` or `pio: clean rx`
   - Rebuild with `pio: build tx` or `pio: build rx`

## 🔗 Additional Resources

- [PlatformIO Documentation](https://docs.platformio.org/)
- [PlatformIO ESP32 Platform](https://docs.platformio.org/en/latest/platforms/espressif32.html)
- [ESP-IDF Framework Integration](https://docs.platformio.org/en/latest/frameworks/espidf.html)
- [ESP-ADF Documentation](https://docs.espressif.com/projects/esp-adf/en/latest/)

---

**Build System**: PlatformIO + ESP-IDF Framework  
**ESP-ADF Version**: v2.6  
**Hardware**: XIAO ESP32-S3, SSD1306 OLED  
**Status**: PlatformIO Migration Complete ✅
