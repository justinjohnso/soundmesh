# Stereo ADC Implementation & High-Bandwidth Audio Streaming

**Date:** November 3, 2025  
**Status:** In Progress - Audio working at 48 kHz, optimizing for higher rates

## Overview

Implemented native stereo ADC audio capture on TX using ESP32-S3's built-in 12-bit ADC, replacing the external PCF8591 I2C ADC. Upgraded network bandwidth from ~1.4 Mbps to target 6+ Mbps for high-quality audio streaming.

## Hardware Changes

### Pin Assignments
- **AUX Stereo Input:**
  - Left: A0 (GPIO1, ADC1_CH0)
  - Right: A1 (GPIO2, ADC1_CH1)
- **Tone Frequency Knob:** A2 (GPIO3, ADC1_CH3)

### Circuit Configuration
- 1kΩ pull-down resistors on each AUX input for initial testing
- Stereo aux jack wired directly to A0/A1

**Note:** For better audio quality, proper biasing (1.65V DC + AC coupling capacitors) is recommended but not required for functional testing.

## Software Implementation

### ADC Continuous Mode

Implemented ESP-IDF's ADC continuous DMA driver for robust stereo capture:

**Key Features:**
- Dual-channel scanning (ADC1_CH0 and ADC1_CH1)
- 83 kHz total sample rate (41.5 kHz per channel - ESP32-S3 ADC maximum)
- TYPE2 output format for ESP32-S3
- Automatic DC bias removal (subtract mid-code 2048)
- 12-bit → 16-bit PCM conversion (left shift 4 bits)
- Linear interpolation upsampling to match network sample rate

**Implementation:** `lib/audio/src/adc_audio.c`
- Non-blocking reads (timeout = 0) to prevent main loop stalls
- Static buffers for demultiplexing to avoid stack overflow
- Interleaved stereo output ready for network transmission

### Signal Detection

Implemented variance-based signal detection to prevent transmitting ADC noise:

```c
// Calculate AC variance (standard deviation)
// Only transmit if STD > 500 (~1.5% of full scale)
```

This distinguishes actual audio signals from DC offset and low-level noise, preventing unnecessary bandwidth usage when no audio source is connected.

### High-Bandwidth Network Streaming

**Target:** 6.144 Mbps (192 kHz / 16-bit / stereo)

#### Initial Configuration
- Sample rate: 192 kHz
- Frame size: 1ms (192 samples = 768 bytes)
- Packet rate: 1000 pps
- Total bandwidth: 6.24 Mbps (with UDP/IP headers)

#### Challenges Encountered

**Problem 1: TX Loop Blocking**
- Root cause: ADC reads with 100ms timeout blocked main loop
- Symptom: Packets sent every 20-100ms instead of 1ms
- Fix: Non-blocking ADC reads (timeout = 0) + smaller buffer size (1024 bytes)

**Problem 2: Logging Overhead**
- Root cause: Per-frame INFO logs at 1000 pps saturated UART and CPU
- Symptom: ~200 kbps actual throughput despite 6 Mbps target
- Fix: Rate-limited logging (1 every 128 packets)

**Problem 3: Timing Precision**
- Root cause: FreeRTOS tick @ 100Hz → vTaskDelay(1) = 10ms, not 1ms
- Symptom: Only ~100 packets/sec = 780 kbps
- Fix: Implemented esp_timer with 1ms periodic callback

**Problem 4: UDP Queue Saturation**
- Root cause: No delay between sends overwhelmed lwIP UDP queue
- Symptom: Continuous "Send failed" errors
- Fix: Pacing with semaphore + esp_timer (proper backpressure)

**Problem 5: I2S DAC at 192 kHz**
- Root cause: UDA1334 couldn't lock to 192 kHz with default 32-bit slot config
- Symptom: RX receiving packets but no audio output
- Fix: Temporarily reduced to 48 kHz for testing; needs explicit 16-bit slot config for higher rates

### esp_timer Implementation

Replaced FreeRTOS vTaskDelay with high-precision timer for accurate 1ms pacing:

```c
static SemaphoreHandle_t tx_timer_sem = NULL;
static uint32_t ms_tick = 0;

static void tx_timer_callback(void* arg) {
    xSemaphoreGive(tx_timer_sem);
}

// Setup
esp_timer_create_args_t timer_args = {
    .callback = &tx_timer_callback,
    .name = "tx_pacer",
    .dispatch_method = ESP_TIMER_TASK
};
esp_timer_start_periodic(tx_timer, 1000); // 1ms

// Main loop
while (1) {
    xSemaphoreTake(tx_timer_sem, portMAX_DELAY);
    ms_tick++;
    // ... audio processing and send
}
```

**Key Points:**
- Uses `ESP_TIMER_TASK` dispatch (not ISR)
- Callback uses `xSemaphoreGive` (not `FromISR`)
- Independent `ms_tick` counter for scheduling (not `tx_seq`)
- Button polling every 5ms, display every 16ms

### RX Optimizations

**Jitter Buffer Improvements:**
- Increased from 8 to 64 frames
- Prefill threshold: 32 frames (prevents immediate underruns)
- Fixed prefill logic: bytes → frames conversion
- Removed per-frame logging

**Stack Overflow Fix:**
- Increased latency measurement task stack from 2KB to 4KB
- Prevents crash when network logging occurs

## Current Performance

**At 48 kHz (current configuration):**
- Bandwidth: ~1.5 Mbps
- Packet rate: 1000 pps
- Frame size: 192 bytes (48 samples × 2 channels × 2 bytes)
- Latency: ~32ms (prefill buffer)
- **Status: Audio working ✓**

**Target 192 kHz (requires additional work):**
- Bandwidth: 6.144 Mbps
- Packet rate: 1000 pps  
- Frame size: 768 bytes (192 samples × 2 channels × 2 bytes)
- **Status: Needs I2S configuration fixes**

## Technical Details

### ADC Characteristics
- ESP32-S3 SAR ADC: 12-bit resolution
- Maximum continuous rate: ~83 kHz (both channels combined)
- Effective per-channel rate: 41.5 kHz
- ADC1 channels: Compatible with WiFi (ADC2 would conflict)

### Network Protocol
- UDP broadcast on port 3333
- 12-byte frame header (magic, version, type, seq, timestamp, payload length)
- Total packet: 780 bytes (12 header + 768 payload)
- Well under UDP MTU (1472 bytes) - no fragmentation

### Audio Processing Pipeline

**TX:**
```
[ADC @ 41.5kHz] → [Demux L/R] → [Remove DC] → [Scale 12→16 bit] → 
  [Upsample to target rate] → [Variance detection] → [UDP send]
```

**RX:**
```
[UDP recv] → [Parse header] → [Jitter buffer] → [Prefill check] → 
  [I2S @ target rate] → [UDA1334 DAC]
```

## Known Issues

### High Priority
1. **Button/Knob Not Responding**
   - Buttons and ADC knob stopped working after esp_timer implementation
   - Need to debug semaphore/timing interaction
   - Possibly related to GPIO state sampling in tight 1ms loop

2. **192 kHz I2S Configuration**
   - UDA1334 DAC doesn't produce audio at 192 kHz
   - Likely needs explicit 16-bit slot width configuration
   - May require APLL clock source or MCLK output
   - Alternative: Stay at 96 kHz or lower

### Medium Priority
3. **AUX Audio Quality**
   - Large DC offsets (-25k to -32k) indicate biasing issues
   - Low AC variance (500-4000) suggests weak signal coupling
   - Needs proper analog front-end (bias, AC coupling, filtering)

4. **Network Bandwidth**
   - Currently limited to ~1.5 Mbps (48 kHz)
   - Need to validate 6+ Mbps sustained throughput
   - May require lwIP buffer tuning and WiFi driver optimization

### Low Priority
5. **Display Update Lag**
   - Rate-limited updates can feel sluggish
   - Consider separate display update task

## Next Steps

### Immediate (Debug Session)
- [ ] Fix button and knob responsiveness
- [ ] Add debug logging to confirm esp_timer is firing
- [ ] Verify ms_tick is incrementing
- [ ] Check GPIO state in tight loop

### Short Term
- [ ] Implement 16-bit I2S slot width for 96/192 kHz support
- [ ] Test incremental sample rates: 48 → 96 → 192 kHz
- [ ] Add APLL clock source configuration if needed
- [ ] Implement non-blocking UDP socket with ENOBUFS handling

### Medium Term
- [ ] Token bucket rate limiter for precise bandwidth control
- [ ] Increase lwIP pbuf pools and WiFi TX buffers
- [ ] Enable WiFi HT40 mode for higher throughput
- [ ] Proper analog front-end for AUX input (bias, AC coupling, anti-alias filter)

### Long Term (v0.2)
- [ ] Move to ESP-NOW for lower latency
- [ ] Implement external I2S stereo ADC for true audio-grade capture
- [ ] Add compression (Opus) to reduce bandwidth
- [ ] Adaptive jitter buffer with clock recovery

## Lessons Learned

### ESP32-S3 ADC Limitations
- 83 kHz total is the practical maximum for continuous mode
- Not truly simultaneous stereo (interleaved scanning)
- Requires upsampling for high-quality audio applications
- Consider external I2S ADC for >44.1 kHz stereo

### High-Throughput UDP on ESP-IDF
- Logging is catastrophic at >100 pps - must be disabled or heavily rate-limited
- FreeRTOS tick rate matters - 100Hz tick can't do 1ms delays
- esp_timer is essential for sub-10ms timing precision
- Non-blocking sockets + backpressure handling required for >5 Mbps
- lwIP and WiFi buffers need tuning for sustained high rates

### Debugging Embedded Audio Systems
- Start with low sample rates to prove the signal path
- Isolate issues: test tone → I2S before adding network
- Non-blocking I/O essential to prevent loop stalls
- Stack overflows appear in unexpected tasks (latency measurement!)
- DC bias removal critical for ADC audio (12-bit mid-code = 2048)

## Code References

### Key Files Modified
- `lib/audio/src/adc_audio.c` - ADC continuous mode implementation
- `lib/config/include/config/pins.h` - Pin definitions for stereo AUX
- `src/tx/main.c` - esp_timer pacing, signal detection, rate-limited logging
- `src/rx/main.c` - Jitter buffer fixes, reduced logging
- `lib/network/src/mesh_net.c` - Increased latency task stack size

### Configuration
- `lib/config/include/config/build.h` - Sample rate and frame size definitions
- Currently: 48 kHz for testing, target 192 kHz

## Performance Metrics

| Metric | Before | Current (48 kHz) | Target (192 kHz) |
|--------|--------|------------------|------------------|
| Sample Rate | 44.1 kHz (PCF8591 stub) | 48 kHz | 192 kHz |
| Bandwidth | ~1.4 Mbps | ~1.5 Mbps | 6.14 Mbps |
| Packet Rate | ~200 pps | 1000 pps | 1000 pps |
| Latency | ~25ms | ~32ms | ~32ms |
| ADC Resolution | 8-bit (external) | 12-bit (internal) | 12-bit |
| Audio Quality | Poor | Good | Excellent (target) |

## Architecture Decisions

### Why Native ADC vs External I2S ADC?
**Chose native ADC for v0.1:**
- ✅ Faster integration (no additional hardware)
- ✅ Lower cost
- ✅ 12-bit better than previous 8-bit PCF8591
- ✅ Acceptable quality for demo/prototype
- ❌ Limited to 41.5 kHz per channel (requires upsampling)
- ❌ Not audio-grade (noise, linearity issues)

**External I2S ADC for v0.2:**
- True 48+ kHz stereo without upsampling
- Better SNR and linearity
- Industry-standard quality
- Requires additional hardware cost/complexity

### Why esp_timer vs FreeRTOS vTaskDelayUntil?
- FreeRTOS tick @ 100Hz can't achieve 1ms precision
- vTaskDelayUntil requires tick ≥ 1000Hz
- esp_timer provides microsecond resolution
- Allows future sub-millisecond adjustments

### Why UDP Broadcast vs ESP-NOW?
- UDP simpler to debug with standard tools
- Works with existing WiFi infrastructure
- Can support multiple receivers naturally
- ESP-NOW better for v0.2 (lower latency, better for mesh)

## Resources & References

- [ESP-IDF ADC Continuous Mode](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_continuous.html)
- [XIAO ESP32-S3 Pinout](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [esp_timer Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/esp_timer.html)
- Oracle AI guidance on ADC implementation, ESP-ADF evaluation, and UDP optimization

## Summary

Successfully migrated from external 8-bit I2C ADC to internal 12-bit stereo ADC with continuous DMA capture. Implemented high-precision timing with esp_timer for 1000 pps packet rate. Current system delivers working audio at 48 kHz with 1.5 Mbps bandwidth. Next focus: resolving button/knob input issues and scaling to 192 kHz for maximum quality.

---

*Implementation by Amp AI with Oracle consultation*
