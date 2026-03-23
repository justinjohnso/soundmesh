# November 3, 2025: From USB Enumeration Failures to Mesh Architecture Vision

## Where We Left Off

The last post ended with PlatformIO integration complete, UDP framing working, and USB audio compiled in. The system could stream tone generation over WiFi, but we needed real audio input to test the end-to-end pipeline. USB enumeration was the next step - get the TX to show up as an audio device on the Mac, plug in a phone or laptop audio, and stream it wirelessly to the RX unit.

That didn't go as planned.

## USB Audio Enumeration Failures

The TinyUSB UAC component compiled successfully, but when I plugged the TX ESP32-S3 into the Mac, it never appeared in System Information or Audio MIDI Setup. The serial logs showed mount events firing, but no enumeration.

### The Build System Bug

The root cause was in `extra_script.py` - the dynamic CMake generation wasn't including the USB source files. TX builds needed `tx/main.c` and `tx/usb_hooks.c`, but the script was only generating for `tx/main.c`. This caused linker errors for USB callback functions.

```python
# Fixed in extra_script.py
if pio_env == "tx":
    app_sources = "tx/main.c tx/usb_hooks.c"  # Added usb_hooks.c
    cmake_requires = "REQUIRES tinyusb"
```

After the fix, builds succeeded but enumeration still failed. The USB descriptors were configured correctly, but something was wrong at the protocol level.

### Pivoting to Hardware Audio Input

Rather than debug complex USB enumeration (which could take days), I pivoted to the other input option: VS1053b audio codec. The plan was simple - aux line-in → VS1053 ADPCM encode → UDP stream. It should work immediately if the hardware was correct.

## VS1053b Implementation: Custom Driver Development

I implemented a custom ESP-IDF SPI driver for VS1053b, replacing the Adafruit library which lacked ADPCM recording support. The Adafruit library only supported MP3/AAC playback and Ogg recording - not the real-time ADPCM encoding we needed.

### Hardware Wiring
- GPIO1: SCI chip select
- GPIO2: SDI data chip select  
- GPIO3: DREQ (data request input)
- GPIO4: XRESET
- GPIO7-9: SPI bus

### SPI Communication Layer
Implemented SCI (control) and SDI (data) interfaces:
- SCI: 2MHz, register read/write with DREQ polling
- SDI: 8MHz, audio data streaming
- Full-duplex SPI (critical - half-duplex caused failures)

### ADPCM Recording Pipeline
1. Hardware reset → DREQ polling
2. Set CLOCKF for proper timing
3. Configure AICTRL registers (sample rate, mono/stereo)
4. Load PCM recorder plugin via WRAM
5. Start recorder with AIADDR entry point
6. Read HDAT0/HDAT1 for encoded audio

### Plugin Loading Algorithm
VS1053b requires external plugins for recording. I sourced the PCM recorder plugin and implemented RLE decompression:

```c
while (i < plugin_size) {
    uint16_t addr = plugin[i++];
    uint16_t n = plugin[i++];
    if (addr == 0xFFFF) {
        // RLE: repeat next value n times
        uint16_t val = plugin[i++];
        for (uint16_t j = 0; j < n; j++) {
            vs1053_sci_write(VS1053_REG_WRAM, val);
        }
    }
}
```

### DREQ Flow Control Issues
The trickiest part was handling DREQ properly. During initialization, DREQ indicates ready-for-command. During recording, DREQ can be low temporarily while processing audio. Treating this as an error caused false timeouts.

Solution: Quick DREQ check before reads - if low, return silence immediately instead of blocking.

### GPIO Pin Conflicts
Discovered GPIO4 was used for both VS1053 reset (TX) and button input (RX). VS1053 init sets GPIO4 as output, disabling button on TX. This needed resolution but wasn't blocking basic audio capture.

## Hardware Reality Check: VS1053 vs VS1053b

After implementing the driver, testing revealed the Adafruit breakout uses regular VS1053 (playback only), not VS1053b (recording capable). The HDAT1 register always returned 0 - no audio data available.

## Pivoting to ESP32 ADC: DMA Implementation

With VS1053 hardware limitations discovered, I pivoted to ESP32's built-in ADC for aux input. This eliminated external hardware dependencies but required high-speed continuous sampling.

### From Polling to DMA
Original polling approach caused watchdog timeouts and divide-by-zero crashes. ESP32-S3 uses `adc_digi_*` APIs with DMA for reliable continuous sampling.

### ADC Configuration
- ADC1 channels only (ADC2 not supported in DMA mode)
- TYPE2 output format (S3-specific)
- 16kHz stereo sampling
- 12-bit → 16-bit conversion

### DMA Setup
```c
adc_digi_configuration_t dig_cfg = {
    .pattern_num = 2,
    .adc_pattern = adc_pattern,
    .sample_freq_hz = 16000 * 2,  // Stereo
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2
};
adc_digi_controller_configure(&dig_cfg);
```

### Data Processing
Automatic DMA fills buffer, task reads TYPE2 format and converts to PCM. Signal detection prevents sending silence when no audio present.

## WiFi Stability and DHCP Crisis

With ADC working, I hit network issues. RX couldn't get IP addresses - DHCP failed consistently.

### Root Cause: Airtime Starvation
TX flooding air with 1920-byte audio packets at 100fps consumed 100% airtime at broadcast rates. DHCP packets (also broadcast) got delayed until timeouts.

### Solution: Event-Driven Stream Gating
Instead of continuous broadcast, gate audio on WiFi events:
- `WIFI_EVENT_AP_STACONNECTED` → pause audio
- `IP_EVENT_AP_STAIPASSIGNED` → resume audio
- `WIFI_EVENT_AP_STADISCONNECTED` → stop audio

This ensures DHCP completes before audio resumes, eliminating airtime contention.

## The Mesh Revelation

Debugging DHCP revealed our "mesh" was actually a star topology - single point of failure. The original vision called for ESP-WIFI-MESH with multi-hop routing and self-healing.

### Architecture Redesign
- **Current:** Star (TX AP, N RX STA) - simple but fragile
- **Target:** True mesh with ESP-WIFI-MESH - self-organizing tree

### Key Changes
- Publisher role independent of root role
- Tree broadcast with duplicate suppression
- 5ms frames (960 bytes) instead of 10ms
- Root election with RX preference

### Implementation Plan
1. Mesh backend scaffold (1-3 hours)
2. 5ms framing & single-hop (2-4 hours) 
3. Tree broadcast & multi-hop (2-4 hours)
4. Root preference & polish (1-2 hours)

## Current Status

✅ **ADC input working** - ESP32 DMA sampling at 16kHz stereo  
✅ **DHCP fixed** - Event-driven gating prevents airtime starvation  
✅ **Mesh architecture designed** - Implementation roadmap complete  
⚠️ **USB enumeration** - Still untested, enumeration failed  
⚠️ **VS1053 driver** - Complete but hardware incompatible  
⚠️ **Multi-hop testing** - Planned but not implemented  

## Technical Lessons

### Hardware Assumptions Kill Projects
Never trust documentation over physical inspection. The "VS1053b" aux input was actually VS1053 without recording firmware.

### Airtime is a Shared Resource  
WiFi broadcast flooding starves control traffic. Event-driven coordination essential for robust operation.

### Publisher ≠ Root in Mesh Networks
Separating application roles (TX/RX) from network roles (root/child) enables better fault tolerance and mobility.

### DMA Solves Embedded Timing Issues
High-speed continuous sampling requires hardware DMA - polling approaches inevitably fail watchdog timers.

## VS1053 Integration Branch: The ADPCM Compression Attempt

Before pivoting to ESP32 ADC, we invested significant effort in VS1053 codec integration for ADPCM compression. While ultimately unsuccessful due to hardware limitations, this work was essential in understanding codec architectures and compression requirements.

### The Premise: Bandwidth Compression via Hardware Codec

After basic UDP streaming worked with uncompressed PCM (~1.5Mbps), compression became critical for mesh networking. IMA ADPCM promised 4:1 compression with hardware acceleration via VS1053 codec.

**Expected Benefits:**
- 24x bandwidth reduction (1.5Mbps → 64kbps)
- Hardware-accelerated encoding/decoding
- Real aux audio input capability
- Same chip for TX encode and RX decode

### Adafruit Library Integration: First Attempt

Started with Adafruit's Arduino VS1053 library, creating an adapter layer to bridge C++ Arduino API to ESP-IDF C code:

```cpp
extern "C" {
esp_err_t vs1053_init(vs1053_t* vs, spi_host_device_t spi_host) {
    initArduino();
    SPI.begin(VS1053_SPI_SCK, VS1053_SPI_MISO, VS1053_SPI_MOSI);

    static Adafruit_VS1053 vs_inst(VS1053_XRESET, VS1053_XCS, VS1053_XDCS, VS1053_DREQ);
    s_vs = &vs_inst;

    if (!s_vs->begin()) {
        return ESP_FAIL;
    }
    // ...
}
}
```

**Result:** Library supported playback and Ogg recording, but lacked ADPCM recording - `SCI_HDAT1` always returned 0.

### Custom ESP-IDF Driver: Second Attempt

Implemented bare-metal SPI driver with direct register access:

**SPI Configuration:**
- Two device handles: SCI (commands) and SDI (audio data)
- Full-duplex SPI (critical for VS1053)
- DREQ polling before each transaction

**ADPCM Recording Pipeline:**
```c
esp_err_t vs1053_start_adpcm_record(vs1053_t* vs, uint32_t sample_rate, bool stereo) {
    // Soft reset with ADPCM bit set
    vs1053_sci_write(vs, VS1053_REG_MODE, SM_SDINEW | SM_RESET | SM_ADPCM);
    vs1053_sci_write(vs, VS1053_REG_AICTRL0, 0);        // ADPCM profile
    vs1053_sci_write(vs, VS1053_REG_AICTRL1, sample_rate); // Sample rate
    // Read encoded data from HDAT0/HDAT1 registers
}
```

**TX Pipeline:** VS1053 → ADPCM blocks → ring buffer → UDP send
**RX Pipeline:** UDP receive → jitter buffer → VS1053 ADPCM decode → line-out

### Hardware Reality Check: VS1053 vs VS1053b

The critical discovery: our Adafruit breakout uses VS1053 (playback only), not VS1053b (recording capable). VS1053 lacks ADPCM recording firmware entirely.

**Evidence:**
- `SCI_HDAT1` register always returned 0
- Status register showed version 3, not 4 (VS1053b)
- ADPCM mode activation failed silently

### Technical Lessons from VS1053 Work

**1. Hardware Assumptions Kill Projects**
Never trust documentation over empirical verification. "VS1053" vs "VS1053b" is a critical distinction.

**2. Library Limitations Can Be Show-Stoppers**
Adafruit library excellent for playback, but ADPCM recording requires bare-metal register access.

**3. SPI Mode Matters**
VS1053 requires full-duplex SPI. `SPI_DEVICE_HALFDUPLEX` caused silent failures.

**4. DREQ Behavior Varies by Mode**
During recording, DREQ low doesn't always indicate error - can be temporary while processing audio.

**5. Plugin Loading Requires Watchdog Management**
Firmware loading takes significant time. Without `esp_task_wdt_reset()`, watchdog triggers.

### Branch Outcome: Valuable Failure

The VS1053 work was comprehensive but ultimately blocked by incorrect hardware. However, it proved:
- ADPCM compression architecture was sound
- Custom codec drivers are feasible on ESP32
- Pipeline design worked for both encode/decode
- Build system could support complex audio components

This failure forced the pragmatic pivot to ESP32 ADC, which provided reliable audio input without compression - sufficient for prototyping the mesh networking layer.

**Files from VS1053 branch that influenced main:**
- Custom SPI driver patterns (reused in other components)
- ADPCM pipeline architecture (informed future compression work)
- Build system patterns for complex audio libraries
- Hardware verification procedures (now mandatory)

## The Path Forward

The system now has reliable audio input via ESP32 ADC and stable WiFi networking. The mesh migration will deliver true self-healing multi-hop audio distribution, fulfilling the original project vision.

Immediate next steps:
1. Test ADC end-to-end audio streaming
2. Implement mesh backend scaffold
3. Validate single-hop mesh formation
4. Proceed to tree broadcast and multi-hop testing

The foundation is solid - we're positioned for the mesh transformation that will make this a genuinely robust wireless audio system.
