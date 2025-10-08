# Mesh Audio — Getting the OLED Displays Working and Setting Up a Multi-Project Workspace

*October 7, 2025*

After getting the basic UDP audio streaming working between TX and RX boards (documented in the previous post), I ran into a frustrating issue: both OLED displays were completely blank. The firmware flashed successfully, the audio streaming worked perfectly, but the displays showed nothing. This post documents the debugging journey that led to discovering a PSRAM configuration issue, implementing a robust OLED driver, and setting up an efficient VS Code multi-project workspace for simultaneous TX/RX development.

## The Problem: Blank OLEDs Despite Successful Flashing

Both XIAO ESP32-S3 boards had SSD1306 128×32 OLED displays connected via I2C:
- **SDA**: GPIO 5
- **SCL**: GPIO 6
- **Address**: 0x3C

The firmware flashed without errors, the serial monitor showed the boards booting and running their main loops, and the UDP audio streaming was working. But both displays remained completely dark. No initialization sequence, no pixels, nothing.

## The Root Cause: PSRAM Boot Loop

The first clue came from examining the serial output more carefully. Instead of seeing the normal boot sequence, the boards were stuck in a boot loop with this error repeating:

```
E (255) quad_psram: PSRAM chip is not connected, or wrong PSRAM line mode
E cpu_start: Failed to init external RAM!
abort() was called at PC 0x... on core 0
```

The boards would boot, hit the PSRAM initialization, fail, and reboot. Over and over. This explained why nothing was displaying — the code never actually reached the OLED initialization because the boot process kept aborting.

### What Was Happening

The `sdkconfig` files for both TX and RX had `CONFIG_SPIRAM=y` enabled. This tells ESP-IDF to initialize external PSRAM (SPI RAM) during boot. The problem: the XIAO ESP32-S3 boards I'm using either don't have PSRAM installed, or it's not configured correctly in the hardware.

When ESP-IDF tried to initialize PSRAM during boot, it failed and called `abort()`, triggering a reboot. This happened before any of my application code ran, so the OLED initialization code never executed.

## The Fix: Disabling PSRAM

The solution was straightforward once I identified the problem. I needed to disable PSRAM in the `sdkconfig` files for both projects:

```bash
# TX project
cd ~/Library/CloudStorage/Dropbox/NYU/Semester\ 3\ \(\'25\ Fall\)/Project\ Development\ Studio/meshnet-audio/firmware/idf/apps/tx
sed -i '' 's/^CONFIG_SPIRAM=y/# CONFIG_SPIRAM is not set/' sdkconfig

# RX project
cd ~/Library/CloudStorage/Dropbox/NYU/Semester\ 3\ \(\'25\ Fall\)/Project\ Development\ Studio/meshnet-audio/firmware/idf/apps/rx
sed -i '' 's/^CONFIG_SPIRAM=y/# CONFIG_SPIRAM is not set/' sdkconfig
```

This single line change in each config file transformed:
```
CONFIG_SPIRAM=y
```

into:
```
# CONFIG_SPIRAM is not set
```

After rebuilding both projects with the corrected configuration, the boards booted successfully and stayed running. The boot loop was gone.

### Impact on Binary Size

Disabling PSRAM had a beneficial side effect — it reduced the binary sizes:
- **TX**: Reduced from 0xc2540 bytes to 0xc05d0 bytes (about 25% partition free)
- **RX**: 0xc1020 bytes (about 25% partition free)

The firmware fit comfortably in the available flash partition with room for future additions.

## Building the OLED Driver: Page-Addressing Mode

With the boot loop fixed, I could finally tackle the actual OLED display code. I needed a reliable way to draw to the 128×32 pixel SSD1306 displays over I2C.

### Why Page-Addressing?

SSD1306 displays support multiple addressing modes:
- **Horizontal addressing** (0x00): Auto-increments across columns and rows
- **Vertical addressing** (0x01): Auto-increments down columns first
- **Page addressing** (0x02): Manual page selection for each write

I chose **page addressing mode** because it proved more reliable and predictable for the update patterns I needed. For a 128×32 display, there are 4 pages (each page is 128 columns × 8 rows of pixels).

### The Core Helper Functions

I implemented a set of page-addressing helper functions that became the foundation of all display operations:

```c
// Set the display to write to a specific page (0-3 for 128×32)
static void oled_set_page(uint8_t page)
{
    uint8_t cmd[] = { 0x00, 0xB0 + page };  // 0xB0 = PAGE_ADDR command
    i2c_master_write_to_device(I2C_NUM_0, OLED_ADDR, cmd, 2, pdMS_TO_TICKS(50));
}

// Write a buffer of pixel data to the current page
static void oled_write_page(const uint8_t *buf, size_t len)
{
    uint8_t *txbuf = malloc(len + 1);
    txbuf[0] = 0x40;  // Data mode
    memcpy(txbuf + 1, buf, len);
    i2c_master_write_to_device(I2C_NUM_0, OLED_ADDR, txbuf, len + 1, pdMS_TO_TICKS(50));
    free(txbuf);
}

// Clear all pages to black
static void oled_clear_all(void)
{
    uint8_t blank[128];
    memset(blank, 0x00, 128);
    for (int page = 0; page < 4; page++) {
        oled_set_page(page);
        oled_write_page(blank, 128);
    }
}

// Fill all pages with a pattern
static void oled_fill(uint8_t pattern)
{
    uint8_t fill[128];
    memset(fill, pattern, 128);
    for (int page = 0; page < 4; page++) {
        oled_set_page(page);
        oled_write_page(fill, 128);
    }
}
```

These functions provide a clean abstraction: I can target any page and write exactly 128 bytes to it, representing 128 columns × 8 rows of pixels.

### Adding I2C Diagnostics

To make debugging easier, I added an I2C bus scanner that runs during initialization:

```c
static void i2c_scan_log(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        esp_err_t ret = i2c_master_write_to_device(
            I2C_NUM_0, addr, &dummy, 0, pdMS_TO_TICKS(50)
        );
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at address 0x%02X", addr);
        }
    }
}
```

This scans addresses 0x03 to 0x77 and logs any responding devices. When I ran this on both boards, it confirmed:
```
I2C device found at address 0x3C
```

Perfect — the displays were responding on the expected address.

### Initialization with Visual Confirmation

The updated `init_oled()` function now includes a visual test to confirm the display is actually working:

```c
static esp_err_t init_oled(void)
{
    // ... I2C initialization ...
    
    // Set page addressing mode
    uint8_t init_cmds[] = { 0x00, 0x20, 0x02 };  // 0x20 = addressing mode, 0x02 = page mode
    i2c_master_write_to_device(I2C_NUM_0, OLED_ADDR, init_cmds, 3, pdMS_TO_TICKS(100));
    
    // Run I2C scan for diagnostics
    i2c_scan_log();
    
    // Visual confirmation: fill white for 200ms, then clear
    oled_fill(0xFF);
    vTaskDelay(pdMS_TO_TICKS(200));
    oled_clear_all();
    
    return ESP_OK;
}
```

When the boards boot now, both displays flash bright white for 200 milliseconds before clearing to black. This provides immediate visual confirmation that:
1. The I2C communication is working
2. The display is responding to commands
3. The pixel data is being written correctly
4. The initialization sequence completed successfully

## Implementing the Progress Bar Display

With the OLED driver working reliably, I implemented a simple but effective status display: an animated progress bar that shows audio packet transmission/reception activity.

### The Design

The display is split into two halves:
- **Top half (pages 0-1)**: Currently cleared, reserved for future status text
- **Bottom half (pages 2-3)**: Animated progress bar

The bar cycles over 100 packets, growing from left to right across the 128-pixel width. Each packet increments the bar by ~1.28 pixels, creating smooth animation as packets flow through the system.

### Implementation

```c
static void update_oled_display(uint32_t packet_count)
{
    // Clear top half (pages 0-1)
    uint8_t blank[128];
    memset(blank, 0x00, 128);
    oled_set_page(0);
    oled_write_page(blank, 128);
    oled_set_page(1);
    oled_write_page(blank, 128);
    
    // Draw progress bar on bottom half (pages 2-3)
    uint8_t step = packet_count % 100;  // Cycle every 100 packets
    int bar_len = (step * 128) / 100;
    if (bar_len < 2) bar_len = 2;  // Minimum 2 pixels
    
    uint8_t bar[128];
    memset(bar, 0x00, 128);
    for (int i = 0; i < bar_len && i < 128; i++) {
        bar[i] = 0xFF;  // All 8 vertical pixels on
    }
    
    oled_set_page(2);
    oled_write_page(bar, 128);
    oled_set_page(3);
    oled_write_page(bar, 128);
}
```

The bar fills pages 2 and 3 (covering 16 vertical pixels), creating a prominent horizontal bar that's easy to see at a glance.

### Update Frequency

To avoid overwhelming the I2C bus and maintain smooth UDP streaming, the display updates every 10 packets:

```c
// In the UDP task loop
if (packet_count % 10 == 0) {
    update_oled_display(packet_count);
}
```

This provides fluid animation (updating roughly 10 times per second at 16 kHz / 160 samples per packet) without impacting audio performance.

## The Result: Visual Confirmation of System Health

With both TX and RX boards now displaying animated progress bars, I have immediate visual feedback about system operation:

- **TX bar animating**: Audio packets being generated and broadcast via UDP
- **RX bar animating**: Audio packets being received and processed
- **Bars in sync**: UDP transmission is working reliably
- **Bars out of sync**: Network congestion or packet loss
- **No bar movement**: System hung or network disconnected

The displays transform debugging from "staring at serial logs" to "glancing at two glowing progress bars." It's satisfying and practical.

## Setting Up the Multi-Project VS Code Workspace

With both projects fully operational, I wanted to streamline the development workflow. Working with TX and RX simultaneously meant constantly switching between project directories, changing ESP-IDF settings, and juggling USB ports.

The solution: a VS Code multi-root workspace.

### The Problem with Single-Project Workspaces

The VS Code ESP-IDF extension expects to work with a single project at a time. It needs to know:
- Which project directory to use (`idf.projectPath`)
- Where to put build artifacts (`idf.buildPath`)
- Which USB port to flash to (`idf.port`)
- Where to find compile_commands.json for IntelliSense (`clangd.arguments`)

When you open the repository root, the extension doesn't know which project you want to work with. Manually switching these settings back and forth between TX and RX was tedious and error-prone.

### Multi-Root Workspace to the Rescue

VS Code supports "multi-root workspaces" where you can open multiple project folders simultaneously, each with its own settings. I created `meshnet-audio.code-workspace`:

```json
{
  "folders": [
    {
      "name": "TX (Transmitter)",
      "path": "firmware/idf/apps/tx"
    },
    {
      "name": "RX (Receiver)",
      "path": "firmware/idf/apps/rx"
    },
    {
      "name": "Root",
      "path": "."
    }
  ],
  "settings": {
    // Global ESP-IDF settings shared by both projects
    "idf.pythonInstallPath": "/opt/homebrew/bin/python3",
    "idf.espIdfPath": "/Users/justin/esp/esp-idf",
    "idf.toolsPath": "/Users/justin/.espressif",
    // ... file exclusions, etc ...
  },
  "TX (Transmitter)": {
    "settings": {
      "idf.port": "/dev/cu.usbmodem1101",
      "idf.buildPath": "/tmp/tx_build",
      "clangd.arguments": [
        "--compile-commands-dir=/tmp/tx_build"
      ]
    }
  },
  "RX (Receiver)": {
    "settings": {
      "idf.port": "/dev/cu.usbmodem101",
      "idf.buildPath": "/tmp/rx_build",
      "clangd.arguments": [
        "--compile-commands-dir=/tmp/rx_build"
      ]
    }
  }
}
```

### How It Works

The workspace file defines three folders:
1. **TX (Transmitter)**: Points to `firmware/idf/apps/tx`
2. **RX (Receiver)**: Points to `firmware/idf/apps/rx`
3. **Root**: The entire repository for documentation, scripts, etc.

Global settings (ESP-IDF paths, file exclusions) are shared. Folder-specific settings (USB ports, build directories) are separate.

When I open a file from the TX folder, the ESP-IDF extension automatically:
- Uses `/tmp/tx_build` as the build directory
- Flashes to `/dev/cu.usbmodem1101`
- Points clangd at TX's compile_commands.json for IntelliSense

When I switch to an RX file, it automatically switches to RX's settings. No manual configuration changes needed.

### The External Build Directory Strategy

Notice that both projects use external build directories (`/tmp/tx_build` and `/tmp/rx_build`) rather than `build/` subdirectories within each project. This serves two purposes:

1. **Path escaping**: The repository path contains spaces and parentheses (`'25 Fall`), which some ESP-IDF tooling doesn't handle well. External build directories sidestep this issue entirely.

2. **Clean separation**: Build artifacts are completely separate from source code, making version control cleaner and reducing the risk of accidentally committing build outputs.

### Benefits of the Multi-Root Setup

With this workspace configuration, I can:
- **Edit TX and RX code side-by-side** in split editors
- **Build either project** with the ESP-IDF extension's build command
- **Flash either project** to its specific USB port with one click
- **Monitor either board** with the extension's monitor command
- **Get accurate IntelliSense** for both projects simultaneously
- **Access shared documentation** in the Root folder

The ESP-IDF extension seamlessly switches context based on which file I'm editing. It's effortless.

## External Build Directories: A Pragmatic Workaround

The repository path issue deserves a bit more explanation. The full path is:

```
/Users/justin/Library/CloudStorage/Dropbox/NYU/Semester 3 ('25 Fall)/Project Development Studio/meshnet-audio
```

Notice the `'25` with a single quote and the parentheses around `Fall`. Some ESP-IDF build scripts invoke shell commands that don't properly escape these characters, leading to cryptic build failures during bootloader compilation or partition table generation.

Rather than fighting with shell escaping or renaming the entire Dropbox folder structure (which would break other projects and cloud sync), I opted for external build directories. By building to `/tmp/tx_build` and `/tmp/rx_build`, the source path never appears in build commands. The build system only sees safe, Unix-friendly paths.

This is a pragmatic workaround that acknowledges real-world constraints. In an ideal world, all tooling would handle arbitrary paths correctly. In practice, working around known issues is often faster than fixing upstream toolchains.

## The Complete Development Workflow

Here's what the development workflow looks like now:

### 1. Open the Multi-Root Workspace
```bash
# Open VS Code with the workspace file
code meshnet-audio.code-workspace
```

VS Code opens with three folders in the sidebar: TX, RX, and Root.

### 2. Edit Code in Split View
- Left editor: `firmware/idf/apps/tx/main/main.c`
- Right editor: `firmware/idf/apps/rx/main/main.c`

Both files have full IntelliSense, clangd analysis, and error highlighting.

### 3. Build Projects
- Click in the TX file → Press `Cmd+Shift+P` → "ESP-IDF: Build Project"
- Click in the RX file → Press `Cmd+Shift+P` → "ESP-IDF: Build Project"

The extension automatically uses the correct build directory for each project.

### 4. Flash and Monitor
- TX builds → "ESP-IDF: Flash Device" → Flashes to `/dev/cu.usbmodem1101`
- RX builds → "ESP-IDF: Flash Device" → Flashes to `/dev/cu.usbmodem101`
- "ESP-IDF: Monitor Device" → Opens serial monitor for the active project

### 5. See Results
Both OLED displays show animated progress bars. Serial monitors show packet statistics. Audio streams from TX to RX.

Everything just works.

## Lessons Learned: Debugging Embedded Displays

This debugging session reinforced several important lessons about embedded development:

### 1. Boot Loops Hide Everything

When a board is stuck in a boot loop, you don't see any application behavior because the application never runs. The PSRAM error was causing reboots before my OLED initialization code even executed. Checking boot logs first is essential.

### 2. Visual Confirmation Is Powerful

The white flash test during OLED initialization provides instant feedback that the display is working. Without it, I'd be left wondering if the problem was I2C communication, display initialization, or pixel data format.

### 3. Page-Addressing Is Reliable

While horizontal addressing mode seems more convenient (auto-incrementing across the entire display), page-addressing mode proved more predictable and easier to debug. When something goes wrong, it's easier to reason about "writing 128 bytes to page 2" than "writing 512 bytes in horizontal mode and hoping it wraps correctly."

### 4. I2C Diagnostics Save Time

The I2C scanner takes 2 seconds to run during boot and provides definitive proof that the display is present and responding. Without it, I'd waste time checking wiring, power, and pull-up resistors before confirming communication.

### 5. External Build Directories Are a Lifesaver

Rather than fighting with toolchain path escaping issues, using external build directories completely sidesteps the problem. It's a pragmatic solution that acknowledges real-world constraints.

### 6. Multi-Root Workspaces Scale Better

Once you're working with more than one related project, the overhead of manual context switching becomes a significant productivity drain. Multi-root workspaces eliminate this friction entirely.

## What's Next: Audio Output and Refinement

With reliable OLED status displays and an efficient development environment, the foundation is solid for continuing work:

1. **Audio output testing**: Connect the RX board's PWM output (GPIO 1) to an aux jack and verify the 440 Hz tone is audible through headphones or speakers.

2. **Display status information**: Use the top half of the OLED (pages 0-1) to show packet counts, network status, sample rate, or other system info.

3. **Mesh network integration**: Revisit the ESP-WIFI-MESH + ESP-ADF work from earlier and integrate the Opus codec for compressed streaming.

4. **Multi-hop testing**: Set up three or more nodes to test mesh routing and audio quality across multiple hops.

5. **Latency measurements**: Implement precise timing to measure end-to-end audio latency from TX input to RX output.

The displays are working, the workspace is configured, and the development flow is smooth. Time to push forward with audio refinement and mesh networking.

## Conclusion: From Blank Screens to Visual Feedback

What started as a frustrating "nothing is displaying" problem turned into an opportunity to build robust OLED drivers, discover and fix PSRAM configuration issues, and optimize the development environment for efficient multi-project work.

The journey from blank OLEDs to animated progress bars required:
- Systematic debugging to find the boot loop root cause
- Disabling incompatible PSRAM configuration
- Implementing page-addressing OLED helpers
- Adding I2C diagnostics for hardware verification
- Creating visual confirmation tests (white flash)
- Building an efficient update mechanism (progress bars)
- Configuring a multi-root VS Code workspace

The result is two boards with glowing animated displays, a development environment that makes simultaneous TX/RX work effortless, and a solid foundation for the next phase of mesh audio development.

*Next up: plugging in some headphones and hearing that 440 Hz tone come through the RX board...*
