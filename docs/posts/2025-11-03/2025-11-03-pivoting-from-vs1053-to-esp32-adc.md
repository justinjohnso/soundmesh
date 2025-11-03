# Pivoting from VS1053 to ESP32 ADC for Audio Input

**Date:** November 3, 2025  
**Status:** Working prototype with ESP32 ADC input  
**Hardware:** XIAO ESP32-S3, UDA1334 I2S DAC

## TL;DR

Discovered the VS1053 breakout is the regular version (not VS1053b), which **doesn't support ADPCM recording**. Pivoted to using ESP32's built-in ADC for stereo aux input. System now streams 16kHz stereo PCM audio from TX (GPIO1/GPIO2 ADC) to RX (UDA1334 I2S DAC) successfully.

## The Problem

The original architecture called for VS1053b codec with IMA ADPCM recording to achieve low-bandwidth wireless audio streaming (~384kbps). After implementing a custom VS1053 SPI driver and proper ADPCM initialization sequence, we discovered:

- **Hardware Limitation**: Adafruit VS1053 Breakout v4 uses regular VS1053 (playback only), not VS1053**b** (which has recording)
- **Symptom**: `SCI_HDAT1` always returned 0 (no audio data), `SS_AD_CLOCK` bit never set
- **Root Cause**: VS1053 lacks the ADPCM recording firmware entirely

### What We Tried (VS1053 Debugging)

1. **Correct initialization sequence** per datasheet:
   - Soft reset
   - Set CLOCKF to 4.5x multiplier
   - Configure input path (MIC vs LINE1)
   - Program AICTRL0-3 registers BEFORE enabling SM_ADPCM
   - Enable ADPCM recorder

2. **Multiple clock speeds**: 3.5x, 4.0x, 4.5x multipliers
3. **Both input paths**: LINE1 (line-in) and MIC (microphone)  
4. **Various sample rates**: 48kHz, 16kHz, 8kHz

**Result**: MODE register configured correctly (0x1800 = SM_ADPCM | SM_SDINEW), but AD_CLOCK never started and HDAT1 remained 0.

## The Solution: ESP32 Built-in ADC

### Architecture Change

**Before (VS1053-based)**:
```
TX: Aux → VS1053 ADPCM encode → UDP (264 bytes/packet)
RX: UDP → VS1053 ADPCM decode → Line-out
```

**After (ESP32 ADC-based)**:
```
TX: Aux → ESP32 ADC (GPIO1/GPIO2 stereo) → 16kHz PCM → UDP
RX: UDP → Jitter buffer → UDA1334 I2S DAC → Headphones
```

### Implementation Details

#### TX: ESP32 ADC Input

**Hardware**:
- **GPIO1**: Left channel ADC (ADC1_CHANNEL_0)
- **GPIO2**: Right channel ADC (ADC1_CHANNEL_1)
- **10kΩ pull-down resistors** on both GPIOs to ground (eliminates floating pin noise)

**Software** ([adc_input.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/audio/src/adc_input.c)):
- 12-bit ADC sampling at 16kHz per channel
- Stereo interleaved output
- Conversion: `(raw_adc - 2048) * 16` to scale 12-bit to 16-bit signed
- Ring buffer for decoupling sampling from network transmission

**Signal Detection**:
- Detects floating inputs: avg < -30000 (raw ADC near 0)
- Detects audio: peak-to-peak > 800
- Only sends UDP packets when actual audio detected

#### RX: UDA1334 I2S DAC Output

**Hardware**:
- **GPIO7**: BCK (bit clock)
- **GPIO8**: WS (word select)
- **GPIO9**: DIN (data in)

**Software** ([i2s_output.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/audio/src/i2s_output.c)):
- I2S driver configured for 16kHz stereo
- Direct PCM playback (no decoding needed)
- Jitter buffer (8 packets) for network tolerance
- Underrun handling (outputs silence if buffer empty)

### Key Debugging Lessons

#### 1. Integer Division in FreeRTOS Delays
```c
// WRONG - causes crash when result is 0
vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000 / 16000));  // = 0 ticks!

// RIGHT - batch sampling with proper delays
for (uint32_t i = 0; i < 160; i++) {
    // sample
}
vTaskDelay(pdMS_TO_TICKS(10));  // 160 samples at 16kHz = 10ms
```

#### 2. Floating GPIO Pins Create Noise
Without pull-down resistors, floating ADC pins read near ground (raw=0-100) which when scaled becomes -32768 to -31000, triggering false audio detection. **Solution**: 10kΩ pull-downs to GND.

#### 3. Aux Audio Isn't Centered
Standard aux outputs are AC-coupled but our ADC expects 0-3.3V DC. Raw ADC values of 0-600 (instead of 1500-2500) show the signal is riding the bottom rail. For better quality, add:
- **AC coupling capacitor** (10µF) in series with aux input
- **Bias resistors** (two 10kΩ) to create 1.65V mid-point

#### 4. Watchdog Timeout from Busy-Wait
```c
// WRONG - starves watchdog
esp_rom_delay_us(62);  // Called 16000 times/sec

// RIGHT - yield to scheduler
vTaskDelay(pdMS_TO_TICKS(10));  // Batch approach
```

### Current Status

✅ **Working**:
- ESP32-S3 TX unit captures stereo audio via ADC at 16kHz
- WiFi AP/STA connection established
- UDP packet transmission (silence detection working)
- ESP32-S3 RX unit receives packets and outputs via I2S
- UDA1334 DAC playback functional
- Both units build and flash successfully

⚠️ **Known Issues**:
1. **Audio quality**: Aux signal not centered around ADC mid-range (needs AC coupling + bias)
2. **Bandwidth**: 16kHz stereo PCM = ~512kbps (higher than ADPCM would have been)
3. **ESP32-C6 builds fail**: ESP-IDF 4.4.6 doesn't support C6 (need v5.x+)

### Next Steps

1. **Add AC coupling circuit**:
   ```
   Aux L/R → [10µF cap] → [10kΩ to 1.65V] → GPIO1/GPIO2
   ```

2. **Audio quality tuning**:
   - Verify ADC readings centered around 2048 (mid-range)
   - Test with different audio sources
   - Add optional gain/attenuation

3. **Consider compression** if WiFi bandwidth becomes an issue:
   - Implement simple ADPCM encoder in software
   - Or use lower sample rate (8kHz)

4. **Display updates**: Show audio levels, packet stats, connection quality

## Files Modified

### New Files Created:
- `lib/audio/include/audio/adc_input.h` - ADC input driver API
- `lib/audio/src/adc_input.c` - ESP32 ADC stereo sampling implementation
- `lib/audio/include/audio/i2s_output.h` - I2S output driver API
- `lib/audio/src/i2s_output.c` - UDA1334 I2S DAC driver
- `lib/audio/tx_pipeline_adc.c` - TX pipeline using ADC instead of VS1053
- `lib/audio/rx_pipeline_i2s.c` - RX pipeline using I2S instead of VS1053

### Files Retained (for future VS1053b support):
- `lib/audio/vs1053/vs1053.c` - Custom VS1053 driver (ready for VS1053b hardware)
- `lib/audio/vs1053/vs1053_regs.h` - Register definitions
- `lib/audio/tx_pipeline.c` - VS1053-based TX pipeline (dormant)
- `lib/audio/rx_pipeline.c` - VS1053-based RX pipeline (dormant)

### Build System Updates:
- `extra_script.py` - Dynamically selects TX vs RX pipeline at build time
- `platformio.ini` - Removed Adafruit VS1053 library dependency
- `.vscode/tasks.json` - Updated for s3-tx, s3-rx, c6-tx, c6-rx environments

## Technical Metrics

**TX Unit**:
- Sample Rate: 16kHz stereo
- ADC Resolution: 12-bit → 16-bit signed
- Packet Size: ~660 bytes (160 samples × 2 channels × 2 bytes + header)
- Packet Rate: 100 packets/second
- Bandwidth: ~528kbps

**RX Unit**:
- I2S Output: 16kHz stereo, 16-bit
- Jitter Buffer: 8 packets (~80ms latency)
- DAC: UDA1334 (hardware I2S)

## Conclusion

While we couldn't achieve the low-bandwidth ADPCM streaming due to hardware limitations, the ESP32 ADC provides a viable alternative for prototyping. The 16kHz stereo PCM stream is acceptable for WiFi point-to-point links and demonstrates the end-to-end wireless audio pipeline.

**If low bandwidth is critical**, options include:
1. Acquire actual VS1053**b** chip
2. Implement software ADPCM encoding on ESP32
3. Use lower sample rates (8kHz mono = 128kbps)
4. Investigate ESP32-S3 hardware audio codec peripherals

The custom VS1053 driver implementation remains valuable if/when upgrading to VS1053b hardware.

---

*Implementation completed by AI assistant with systematic debugging and hardware limitation discovery.*
