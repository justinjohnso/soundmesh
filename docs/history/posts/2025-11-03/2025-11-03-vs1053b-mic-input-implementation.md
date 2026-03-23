# VS1053b Mic Input Implementation: From PCF8591 to Working Audio Capture

**Date:** November 3, 2025  
**Status:** Partial Success - Plugin Loaded, DREQ Timing Issues Remain  
**Hardware:** VS1053b Audio Codec on XIAO ESP32-S3

## Context: The Audio Input Problem

Returning to the MeshNet Audio project after time away, the goal was clear: get audio input working on the TX device for the MVP. The system already had a working audio pipeline - tone generator → UDP broadcast → I2S DAC output on RX - but without real audio input, it was just streaming test tones.

The original plan called for three input sources:
1. **Tone Generator** - 440Hz test signal (working)
2. **USB Audio** - TinyUSB UAC device (partially implemented, untested)
3. **Aux Input** - External audio via ADC (stubbed with PCF8591)

The question: which path gets us to working music playback fastest?

## The PCF8591 Detour

Initial analysis suggested the PCF8591 I2C ADC would be the quickest path:
- I2C bus already working (shared with OLED display)
- Simple 8-bit sampling
- Estimated 1-2 hours to implementation

I implemented a frame-based reader with DC blocking filter, converted the 8-bit samples to 16-bit PCM, duplicated mono to stereo. Build succeeded. Flash succeeded. Runtime? Continuous I2C failures:

```
E (26764) adc_audio: Failed to configure PCF8591: ESP_FAIL
```

The PCF8591 wasn't responding on the bus. Hardware wasn't connected.

Turns out, the project was never using PCF8591. The aux input was actually a **VS1053b audio codec** - a much more capable chip with built-in ADC, microphone preamp, and even audio encoding capabilities.

## Understanding VS1053b Architecture

The VS1053b is an audio codec chip from VLSI Solutions that communicates over SPI. Unlike simple ADCs, it requires:

**SPI Interfaces:**
- **SCI (Serial Command Interface)**: Control registers via SPI
  - Pins: xCS (chip select), SCK, MOSI, MISO
  - Protocol: 16-bit register read/write operations
- **SDI (Serial Data Interface)**: Audio data streaming
  - Pins: xDCS (data chip select)
  - Used for playing audio; not needed for recording

**Key Registers:**
- `SCI_MODE` (0x00): Operating mode bits
- `SCI_STATUS` (0x01): Chip version and status
- `SCI_CLOCKF` (0x03): Clock multiplier configuration
- `SCI_HDAT0` (0x08): Recorded audio data output
- `SCI_HDAT1` (0x09): Available words count
- `SCI_AIADDR` (0x0A): Application entry point
- `SCI_AICTRL0-3` (0x0C-0x0F): Application-specific control
- `SCI_WRAMADDR/WRAM` (0x07/0x06): Plugin loading

**DREQ Flow Control:**
- DREQ (Data Request) GPIO signals when chip is ready
- Must wait for DREQ=HIGH before SCI operations
- Behavior changes during recording vs playback

## Wiring: ESP32-S3 to VS1053b

Based on available GPIOs on the XIAO ESP32-S3:

```
GPIO 1 → xCS   (SCI chip select)
GPIO 2 → xDCS  (SDI chip select) 
GPIO 3 → DREQ  (data request input)
GPIO 4 → RST   (reset output)
GPIO 7 → SCK   (SPI clock)
GPIO 8 → MISO  (SPI data in)
GPIO 9 → MOSI  (SPI data out)
```

**Pin Conflict Discovered:**
GPIO 4 is shared between button (RX) and VS1053 reset (TX). This causes issues:
- TX: VS1053 init sets GPIO 4 as output → no button
- Button init was still being called on TX → conflicts with VS1053

## Implementation: SPI Communication Layer

### SPI Bus Configuration

ESP-IDF SPI setup for VS1053b:

```c
spi_bus_config_t bus_cfg = {
    .mosi_io_num = GPIO_NUM_9,
    .miso_io_num = GPIO_NUM_8,
    .sclk_io_num = GPIO_NUM_7,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 32
};
spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED);

spi_device_interface_config_t dev_cfg = {
    .clock_speed_hz = 2000000,  // 2 MHz
    .mode = 0,                  // SPI mode 0
    .spics_io_num = -1,         // Manual CS control
    .queue_size = 1,
    .flags = 0                  // Full-duplex (critical!)
};
spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle);
```

**Critical lesson:** Initially used `SPI_DEVICE_HALFDUPLEX` flag. This caused constant errors:

```
E (860) spi_master: check_trans_valid(818): SPI half duplex mode is not supported 
when both MOSI and MISO phases are enabled.
```

VS1053b requires **full-duplex SPI**. Removing the flag fixed communication immediately.

### SCI Register Operations

Writing to VS1053 register:

```c
static bool vs1053_sci_write(uint8_t addr, uint16_t data) {
    if (!vs1053_wait_dreq(100)) {
        return false;
    }
    
    uint8_t tx_data[4] = {
        0x02,                    // SCI write command
        addr,                    // Register address
        (data >> 8) & 0xFF,      // MSB first (big-endian)
        data & 0xFF
    };
    
    gpio_set_level(VS1053_CS_PIN, 0);
    spi_device_transmit(spi_handle, &trans);
    gpio_set_level(VS1053_CS_PIN, 1);
    return true;
}
```

Reading from VS1053 register (symmetric pattern, command 0x03).

### DREQ Flow Control

The trickiest part was handling DREQ properly:

```c
static bool vs1053_wait_dreq(uint32_t timeout_ms) {
    uint32_t start = xTaskGetTickCount();
    while (gpio_get_level(VS1053_DREQ_PIN) == 0) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGE(TAG, "DREQ timeout after %lu ms", timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}
```

Without timeout protection, DREQ waits would hang the watchdog timer. Initial implementation caused 5-second watchdog resets during plugin loading.

## The Plugin System

VS1053b recording requires loading two components:

### 1. Patches (Bug Fixes)

Official VS1053b patches from VLSI fix silicon errata. Small payload (~2-8KB).

### 2. Recorder Plugin

Found the **PCM/ADPCM recorder plugin** via the libdriver/vs1053b repository on GitHub. This is the simplest recorder - outputs raw 16-bit PCM via SCI_HDAT registers.

Plugin data (40 words):

```c
#define VS1053B_PCM_RECORDER_PLUGIN_SIZE 40

static const uint16_t vs1053b_pcm_recorder_plugin[40] = {
    0x0007, 0x0001, 0x8010, 0x0006, 0x001c, 0x3e12, 0xb817, 0x3e14,
    0xf812, 0x3e01, 0xb811, 0x0007, 0x9717, 0x0020, 0xffd2, 0x0030,
    0x11d1, 0x3111, 0x8024, 0x3704, 0xc024, 0x3b81, 0x8024, 0x3101,
    0x8024, 0x3b81, 0x8024, 0x3f04, 0xc024, 0x2808, 0x4800, 0x36f1,
    0x9811, 0x0007, 0x0001, 0x8028, 0x0006, 0x0002, 0x2a00, 0x040e
};
```

Stored in `lib/audio/vs1053_plugins/vs1053b_pcm_recorder.h`.

### Plugin Loading Algorithm

Plugins use RLE (Run-Length Encoding) compression:

```c
static void vs1053_load_plugin(const uint16_t *plugin, size_t plugin_size) {
    size_t i = 0;
    while (i < plugin_size) {
        uint16_t addr = plugin[i++];
        uint16_t n = plugin[i++];
        
        if (addr == 0xFFFF) {
            // RLE: repeat next value n times
            uint16_t val = plugin[i++];
            for (uint16_t j = 0; j < n; j++) {
                vs1053_sci_write(VS1053_REG_WRAM, val);
            }
        } else {
            // Write n words starting at addr
            vs1053_sci_write(VS1053_REG_WRAMADDR, addr);
            for (uint16_t j = 0; j < n; j++) {
                vs1053_sci_write(VS1053_REG_WRAM, plugin[i++]);
            }
        }
    }
}
```

Pattern: `{addr, count, data...}` with `addr=0xFFFF` indicating RLE compression.

## Recorder Configuration and Startup

Complete initialization sequence:

```c
esp_err_t adc_audio_init(void) {
    // 1. GPIO setup (CS, DCS, RST outputs; DREQ input with pullup)
    
    // 2. SPI bus initialization
    
    // 3. Hardware reset
    gpio_set_level(VS1053_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(VS1053_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // 4. Verify chip version
    uint16_t status = vs1053_sci_read(VS1053_REG_STATUS);
    uint8_t version = (status >> 4) & 0x0F;
    // Should read: version=4, status=0x0048
    
    // 5. Set clock multiplier
    vs1053_sci_write(VS1053_REG_CLOCKF, 0x6000);  // 3.5x multiplier
    
    // 6. Software reset
    vs1053_sci_write(VS1053_REG_MODE, SM_SDINEW | SM_RESET);
    
    // 7. Load PCM recorder plugin
    vs1053_load_plugin(vs1053b_pcm_recorder_plugin, 40);
    
    // 8. Configure for mic input (SM_LINE1 cleared = mic path)
    vs1053_sci_write(VS1053_REG_MODE, SM_SDINEW);
    
    // 9. Set recorder parameters
    vs1053_sci_write(VS1053_REG_AICTRL0, 44100);  // Sample rate
    vs1053_sci_write(VS1053_REG_AICTRL1, 0);      // Mono
    vs1053_sci_write(VS1053_REG_AICTRL2, 0);      // Auto gain
    vs1053_sci_write(VS1053_REG_AICTRL3, 0x0000); // Linear PCM mode
    vs1053_sci_write(VS1053_REG_AUDATA, 44100);
    
    // 10. Start recorder application
    vs1053_sci_write(VS1053_REG_AIADDR, 0x0034);  // Entry point
    vTaskDelay(pdMS_TO_TICKS(50));
    
    return ESP_OK;
}
```

## Reading Audio Samples

Frame-based reading from HDAT registers:

```c
esp_err_t adc_audio_read_frame(int16_t *stereo_frame, size_t frame_samples) {
    // Quick check: if DREQ low, recorder busy - return silence
    if (gpio_get_level(VS1053_DREQ_PIN) == 0) {
        memset(stereo_frame, 0, frame_samples * 2 * sizeof(int16_t));
        return ESP_OK;
    }
    
    // Check how many samples available
    uint16_t words_available = vs1053_sci_read(VS1053_REG_HDAT1);
    
    if (words_available == 0) {
        memset(stereo_frame, 0, frame_samples * 2 * sizeof(int16_t));
        return ESP_OK;
    }
    
    // Read samples
    size_t to_read = (words_available < frame_samples) ? words_available : frame_samples;
    for (size_t i = 0; i < to_read; i++) {
        int16_t sample = (int16_t)vs1053_sci_read(VS1053_REG_HDAT0);
        
        // Duplicate mono to stereo
        stereo_frame[i * 2] = sample;
        stereo_frame[i * 2 + 1] = sample;
    }
    
    // Fill rest with silence
    for (size_t i = to_read; i < frame_samples; i++) {
        stereo_frame[i * 2] = 0;
        stereo_frame[i * 2 + 1] = 0;
    }
    
    return ESP_OK;
}
```

## Issues Encountered

### 1. SPI Half-Duplex Mode Error

**Problem:** Initial SPI configuration used `SPI_DEVICE_HALFDUPLEX` flag.

**Symptom:**
```
E (860) spi_master: SPI half duplex mode is not supported when both 
MOSI and MISO phases are enabled.
```

**Fix:** Remove `SPI_DEVICE_HALFDUPLEX`, use full-duplex (flags=0). VS1053b SCI requires bidirectional communication.

### 2. Watchdog Timeouts During Initialization

**Problem:** Plugin loading takes significant time. Without watchdog reset, 5-second timeout would trigger.

**Symptom:**
```
E (5910) task_wdt: Task watchdog got triggered. The following tasks/users 
did not reset the watchdog in time:
E (5910) task_wdt:  - main (CPU 0)
```

**Fix:** Call `esp_task_wdt_reset()` during long initialization sequences. Also added timeout protection to `vs1053_wait_dreq()`.

### 3. DREQ Timeout During Frame Reads

**Problem:** DREQ behavior differs between initialization and runtime recording. During frame reads, DREQ can be low (recorder busy) without being an error.

**Symptom:**
```
E (1050) adc_audio: DREQ timeout after 100 ms
```

**Fix:** Added quick DREQ check before attempting reads. If DREQ low, return silence immediately instead of blocking:

```c
if (gpio_get_level(VS1053_DREQ_PIN) == 0) {
    // Recorder busy - return silence, don't block
    memset(stereo_frame, 0, frame_samples * 2 * sizeof(int16_t));
    return ESP_OK;
}
```

### 4. GPIO Pin Conflict: Button vs VS1053 Reset

**Problem:** GPIO 4 is used for both:
- Button input (RX device)
- VS1053 reset output (TX device)

When VS1053 initialization configures GPIO 4 as output, it disables button functionality on TX.

**Current Status:** Unresolved. Button presses on TX do nothing.

**Options:**
- Disable button initialization on TX (if button not needed)
- Move VS1053 reset to different GPIO (requires rewiring)
- Move button to different GPIO on TX

## Current State

### ✅ Working
- SPI communication with VS1053b verified (chip version 4, status 0x0048)
- Plugin loading completes without errors
- Recorder starts successfully
- No watchdog timeouts or crashes
- TX broadcasts audio packets

### ⚠️ Partial
- DREQ timeout warnings during reads (not blocking, just noisy)
- Button disabled on TX due to GPIO conflict
- Unknown if audio samples are actually being captured (HDAT1 value not logged)

### ❌ Not Working
- No confirmation that mic input is producing audio data
- Button input mode switching broken on TX
- Haven't verified audio quality or that actual sound is being captured

## Technical Decisions

### Why VS1053b Over USB Audio?

**USB Audio Pros:**
- Higher quality (16-bit native)
- No additional hardware needed
- Computer audio routing

**USB Audio Cons:**
- TinyUSB descriptor complexity
- Host enumeration can be finicky
- Debugging requires USB analyzer or trial-and-error
- Previous attempts mentioned "main.c corruption" and linker conflicts

**VS1053b Pros:**
- Hardware guaranteed present and wired
- Well-documented SPI protocol
- Plugin system mature and stable
- Microphone preamp built-in

**VS1053b Cons:**
- Requires plugin loading
- More complex initialization
- Plugin files need to be sourced externally

Given time constraints and desire for "any working audio," VS1053b was the pragmatic choice.

### Plugin Choice: PCM vs ADPCM vs Ogg

Available options from libdriver/vs1053b repository:
- **PCM Recorder** (40 words): Raw 16-bit PCM, no decoding needed
- **ADPCM Recorder** (~40 words): 4:1 compression, requires IMA ADPCM decoder on ESP32
- **Ogg Vorbis Encoders** (12,000+ words): High compression, complex decoding

**Decision:** PCM recorder for simplicity. Direct 16-bit samples from HDAT0, no codec overhead on ESP32.

### Sample Rate: 44.1kHz

Current audio pipeline configured for 44.1kHz stereo. VS1053b recorder configured to match:

```c
vs1053_sci_write(VS1053_REG_AICTRL0, 44100);  // Sample rate
```

Note: Documentation suggests 16kHz might be more appropriate for wireless streaming (lower bandwidth), but keeping 44.1kHz for now to avoid resampling complexity.

## Lessons Learned

### 1. Documentation Lies: Verify Hardware Reality

The v0.1 implementation plan, README, and multiple docs referenced PCF8591 as the aux input ADC. None of them mentioned VS1053b. Only when I2C failed and the user corrected me did the actual hardware become clear.

**Lesson:** When returning to a project, verify physical hardware before trusting documentation.

### 2. SPI Configuration Details Matter

Half-duplex vs full-duplex isn't just a performance optimization. For bidirectional peripherals like VS1053b, it's a hard requirement. The error message was clear, but easy to miss if not looking for it.

### 3. Watchdog Protection is Not Optional

Embedded systems with watchdog timers enabled require discipline:
- Long blocking operations need periodic `esp_task_wdt_reset()` calls
- Flow control waits (like DREQ) need timeout protection
- Initialization sequences can take hundreds of milliseconds

Without this, systems crash mysteriously after "working fine" for a few seconds.

### 4. Plugin Loading Requires Correct Endianness

VS1053b expects big-endian 16-bit words. ESP32 is little-endian. All SCI operations require explicit byte-order handling:

```c
// Write data (MSB first)
tx_data[2] = (data >> 8) & 0xFF;
tx_data[3] = data & 0xFF;

// Read data (MSB first)
return (rx_data[2] << 8) | rx_data[3];
```

### 5. Runtime DREQ != Initialization DREQ

During initialization and configuration, DREQ high means "ready for next command."

During recording, DREQ can be low temporarily while the recorder processes audio. Treating this as an error causes false timeouts. The fix: non-blocking check and graceful silence return.

## Next Steps

### Immediate: Verify Audio Capture

Add logging to confirm audio samples are arriving:

```c
if (words_available > 0) {
    ESP_LOGI(TAG, "VS1053b: %u words available", words_available);
}
```

Test with actual audio (music playing near mic, voice, clapping). Verify HDAT1 returns non-zero values.

### Short-term: Resolve GPIO Conflict

Options:
1. Disable button on TX (simplest if not needed)
2. Move button to GPIO 10 on TX
3. Add build flag to conditionally initialize button only on RX

### Medium-term: Audio Quality Tuning

Once audio is confirmed working:
- Test input gain (AICTRL2)
- Verify sample rate matches (44.1kHz vs actual capture rate)
- Check for clipping or distortion
- Consider switching to 16kHz to reduce bandwidth

### Long-term: Alternative Plugins

If PCM quality is insufficient:
- Try ADPCM recorder (4:1 compression, add IMA ADPCM decoder)
- Consider Ogg Vorbis encoder for network bandwidth savings

## Current Status Summary

**Goal:** Get music playback working for MVP

**Progress:**
- ✅ VS1053b SPI communication working
- ✅ Plugin loaded successfully
- ✅ Recorder started without crashes
- ❌ Audio capture not yet verified
- ❌ Button input broken on TX

**Time Invested:** ~2-3 hours (research, implementation, debugging)

**Estimated Time to Working Audio:** 1-2 hours (verify capture, fix DREQ handling, test quality)

## The Irony

We went down the PCF8591 path because it looked simpler. Turned out the hardware wasn't even connected. The "more complex" VS1053b ended up being the correct choice all along - better quality, built-in preamp, more features.

Sometimes the harder path is faster if it's the path that actually exists in reality.

---

**References:**
- [libdriver/vs1053b](https://github.com/libdriver/vs1053b) - Plugin data source
- [VS1053b Datasheet](https://www.vlsi.fi/fileadmin/datasheets/vs1053.pdf) - Official documentation
- [Adafruit VS1053 Library](https://github.com/adafruit/Adafruit_VS1053_Library) - Arduino reference implementation
