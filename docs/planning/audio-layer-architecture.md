# MeshNet Audio: Audio Layer Architecture

**Date:** November 3, 2025  
**Status:** ✅ APPROVED - Target Architecture  
**Goal:** Define audio processing pipeline for mesh network streaming

## Audio Specification (v0.1 Target)

### Core Audio Format
- **Sample Rate:** 48 kHz (professional audio standard)
- **Bit Depth:** 24-bit PCM (professional dynamic range)
- **Channels:** Mono (bandwidth optimization for mesh)
- **Frame Size:** 5ms = 240 samples = 720 bytes
- **Frame Rate:** 200 frames/second
- **Bitrate:** 1.152 Mbps (manageable for multi-hop mesh)

### Rationale
- **48 kHz:** Professional standard, more universal than 44.1kHz (broadcast/video/USB)
- **Mono:** Saves 50% bandwidth vs stereo, enables more mesh nodes
- **5ms frames:** Lower per-hop latency than 10ms, better for mesh routing
- **24-bit:** Professional dynamic range (144dB), future-proof for high-quality inputs

## Audio Layer Architecture

### Principle: Layer Independence
**Audio layer is completely isolated from network and control layers:**
- Audio processing runs on **Core 1 (APP_CPU)**
- Network stack runs on **Core 0 (PRO_CPU)** 
- Audio layer **never** calls network APIs directly
- Network pushes data to audio via callbacks
- Control layer polls audio metrics read-only

### TX Audio Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│                        TX AUDIO PIPELINE                     │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐      ┌──────────────┐      ┌───────────┐ │
│  │ Input Source │──────▶│  Bit Depth   │──────▶│  Frame    │ │
│  │              │       │  Normalization│      │  Builder  │ │
│  └──────────────┘      └──────────────┘      └───────────┘ │
│         │                                            │       │
│         ├─ Tone (24-bit mono)                        │       │
│         ├─ ADC (12→24-bit mono) [DEFAULT]            │       │
│         └─ USB (stereo→mono downmix) [FUTURE]        │       │
│                                                      │       │
│                                                      ▼       │
│                                            ┌──────────────┐ │
│                                            │   Optional   │ │
│                                            │ Opus Encoder │ │
│                                            │  (ESP-ADF)   │ │
│                                            └──────────────┘ │
│                                                      │       │
│                                                      ▼       │
│                                            ┌──────────────┐ │
│                                            │   Network    │ │
│                                            │   Callback   │ │
│                                            └──────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### RX Audio Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│                        RX AUDIO PIPELINE                     │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐      ┌──────────────┐      ┌───────────┐ │
│  │   Network    │──────▶│   Optional   │──────▶│  Jitter   │ │
│  │   Callback   │       │ Opus Decoder │      │  Buffer   │ │
│  └──────────────┘      └──────────────┘      └───────────┘ │
│                                                      │       │
│                                                      ▼       │
│                                            ┌──────────────┐ │
│                                            │ Multi-Stream │ │
│                                            │    Mixer     │ │
│                                            │   [FUTURE]   │ │
│                                            └──────────────┘ │
│                                                      │       │
│                                                      ▼       │
│                                            ┌──────────────┐ │
│                                            │ Mono→Stereo  │ │
│                                            │  Duplication │ │
│                                            └──────────────┘ │
│                                                      │       │
│                                                      ▼       │
│                                            ┌──────────────┐ │
│                                            │   I2S DAC    │ │
│                                            │  (UDA1334)   │ │
│                                            └──────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## TX Input Sources

### 1. Tone Generator (Testing/Demo)

**Purpose:** Generate sine waves for testing mesh connectivity

**Specification:**
- **Sample Rate:** 48 kHz
- **Bit Depth:** 24-bit signed PCM
- **Channels:** Mono
- **Frequency Range:** 200-2000 Hz (controlled via ADC knob)
- **Implementation:** `lib/audio/src/tone_gen.c`

**API:**
```c
esp_err_t tone_gen_init(uint32_t freq_hz);
void tone_gen_set_frequency(uint32_t freq_hz);
void tone_gen_generate_samples(int16_t *buffer, size_t num_samples);
```

**Frame Generation:**
```c
// Every 5ms (240 samples @ 48kHz)
int16_t mono_frame[240];
tone_gen_generate_samples(mono_frame, 240);
network_send_audio((uint8_t*)mono_frame, 720);
```

### 2. ADC Aux Input (Default/Primary)

**Purpose:** Analog audio input via ESP32-S3 ADC (aux cable, mic, instrument)

**Specification:**
- **Sample Rate:** 48 kHz
- **Native Bit Depth:** 12-bit (ESP32-S3 ADC limitation)
- **Transmitted Bit Depth:** 24-bit (scaled up for professional quality)
- **Channels:** Mono (ADC1 channel)
- **Input Mode:** Continuous DMA sampling
- **Implementation:** `lib/audio/src/adc_audio.c`

**Bit Depth Conversion:**
```c
// ADC returns 12-bit samples (0-4095)
// Left-shift by 12 to fill 24-bit range
int32_t sample_24bit = ((int32_t)(adc_12bit_sample - 2048)) << 12;  // Center at zero

// Pack to 3 bytes for network transmission (little-endian)
uint8_t packed[3] = {
    sample_24bit & 0xFF,
    (sample_24bit >> 8) & 0xFF,
    (sample_24bit >> 16) & 0xFF
};
```

**Challenges:**
- **DC Offset:** ADC may have DC bias → High-pass filter (1st order, ~20 Hz cutoff)
- **Sample Rate:** ESP32-S3 ADC max ~83 kHz total → Use 48 kHz for single channel
- **DMA Buffer Management:** Continuous mode requires careful buffer handling

**API:**
```c
esp_err_t adc_audio_init(void);
esp_err_t adc_audio_start(void);
esp_err_t adc_audio_read_mono(int16_t *mono_buffer, size_t num_samples, size_t *samples_read);
esp_err_t adc_audio_stop(void);
```

**Frame Generation:**
```c
// Every 5ms (240 samples @ 48kHz)
int16_t mono_frame[240];
size_t samples_read;
adc_audio_read_mono(mono_frame, 240, &samples_read);

// Apply DC blocking (optional)
dc_block_filter(mono_frame, 240);

network_send_audio((uint8_t*)mono_frame, 720);
```

### 3. USB Audio Input (Future/v0.2)

**Purpose:** Computer audio streaming via TinyUSB UAC1 device

**Specification:**
- **Sample Rate:** 48 kHz (strict - no SRC in v0.1)
- **Bit Depth:** 24-bit
- **Channels:** Stereo → downmix to mono for transmission
- **USB Mode:** Audio Class 1.0 (UAC1) OUT endpoint
- **Implementation:** `lib/audio/src/usb_audio.c` (stubbed)

**Stereo to Mono Downmix:**
```c
// Average left and right channels
int16_t mono_sample = (stereo_left + stereo_right) / 2;
```

**API:**
```c
esp_err_t usb_audio_init(void);
bool usb_audio_is_active(void);
esp_err_t usb_audio_read_frames(int16_t *frames, size_t frame_count, size_t *frames_read);
```

**Frame Generation:**
```c
// Every 5ms (240 stereo samples = 480 samples total)
int16_t stereo_frame[480];  // L,R,L,R,...
int16_t mono_frame[240];
size_t frames_read;

if (usb_audio_is_active()) {
    usb_audio_read_frames(stereo_frame, 240, &frames_read);
    
    // Downmix stereo to mono
    for (int i = 0; i < 240; i++) {
        mono_frame[i] = (stereo_frame[i*2] + stereo_frame[i*2+1]) / 2;
    }
    
    network_send_audio((uint8_t*)mono_frame, 720);
}
```

**TinyUSB Integration:**
- UAC1 descriptor: 48 kHz, stereo, 24-bit
- Isochronous OUT endpoint (2ms packets typical)
- Ring buffer to decouple USB packets from network frames

## RX Audio Output

### I2S DAC (UDA1334)

**Purpose:** High-quality audio output to headphones/speakers

**Specification:**
- **Sample Rate:** 48 kHz
- **Bit Depth:** 24-bit
- **Channels:** Stereo (mono duplicated to L+R)
- **Interface:** I2S standard mode
- **Implementation:** `lib/audio/src/i2s_audio.c`

**Hardware Connection:**
```
ESP32-S3        UDA1334
GPIO43 (I2S_SCK)  → BCLK
GPIO44 (I2S_WS)   → WSEL (LRCLK)
GPIO2  (I2S_DOUT) → DIN
3.3V              → VIN
GND               → GND
```

**Mono to Stereo Duplication:**
```c
// Duplicate mono sample to both channels
void mono_to_stereo(int16_t *stereo_out, const int16_t *mono_in, size_t num_mono_samples) {
    for (size_t i = 0; i < num_mono_samples; i++) {
        stereo_out[i*2]     = mono_in[i];  // Left
        stereo_out[i*2 + 1] = mono_in[i];  // Right
    }
}
```

**API:**
```c
esp_err_t i2s_audio_init(void);
esp_err_t i2s_audio_write_samples(const int16_t *samples, size_t num_samples);
esp_err_t i2s_audio_write_mono_as_stereo(const int16_t *mono_samples, size_t num_mono_samples);
```

**Playback Loop:**
```c
// Every 5ms
int16_t mono_frame[240];
int16_t stereo_frame[480];

// Read from jitter buffer
ring_buffer_read(jitter_buffer, (uint8_t*)mono_frame, 720);

// Duplicate to stereo
mono_to_stereo(stereo_frame, mono_frame, 240);

// Write to I2S DAC
i2s_audio_write_samples(stereo_frame, 480);
```

## Jitter Buffer Management

### Purpose
Smooth out network timing variations and prevent audio glitches

### Configuration
- **Capacity:** 10 frames (50ms @ 5ms/frame)
- **Prefill Threshold:** 4 frames (20ms startup latency)
- **Underrun Behavior:** Write silence frame, increment counter
- **Overrun Behavior:** Drop oldest frame, increment counter

### Clock Drift Correction (Elastic Buffer)

**Purpose:** Compensate for crystal oscillator mismatches between TX and RX

**Problem:**
- TX clock: 48.000 kHz
- RX clock: 48.005 kHz (50 PPM error, typical)
- Result: Buffer slowly fills (240 samples/sec difference over time)

**Solution (v0.1): Elastic Buffer**
- Monitor buffer fill level every 5 seconds
- If fill >80%: Drop one sample (TX faster than RX)
- If fill <20%: Duplicate one sample (RX faster than TX)
- Perform at zero-crossing to minimize audible artifact

**Implementation:**
```c
// lib/audio/include/audio/drift_control.h

esp_err_t drift_control_init(ring_buffer_t *jitter_buffer);
void drift_control_check(void);  // Call every 5 seconds
uint32_t drift_control_get_corrections(void);  // Total corrections since boot
float drift_control_get_ppm_error(void);  // Estimated PPM drift

// lib/audio/src/drift_control.c

void drift_control_check(void) {
    size_t available = ring_buffer_available(jitter_buffer);
    size_t capacity = ring_buffer_get_capacity(jitter_buffer);
    float fill_ratio = (float)available / capacity;
    
    if (fill_ratio > 0.80) {
        // Buffer filling up - TX faster than RX
        // Drop one sample at zero-crossing
        int16_t sample;
        ring_buffer_read(jitter_buffer, (uint8_t*)&sample, 2);
        drift_corrections++;
        ESP_LOGD(TAG, "Drift correction: dropped 1 sample (fill=%.2f)", fill_ratio);
    }
    else if (fill_ratio < 0.20) {
        // Buffer draining - RX faster than TX
        // Duplicate last sample
        int16_t last_sample = get_last_sample();
        ring_buffer_write(jitter_buffer, (uint8_t*)&last_sample, 2);
        drift_corrections++;
        ESP_LOGD(TAG, "Drift correction: inserted 1 sample (fill=%.2f)", fill_ratio);
    }
}
```

**Performance:**
- Typical drift: 10-100 PPM (0.001%-0.01% error)
- At 50 PPM, 48 kHz: 2.4 samples/sec error
- Correction frequency: ~1 every 2-5 seconds
- **Inaudible** (single sample at zero-crossing)

**v0.2 Upgrade: ESP-ADF Resample Filter**
- Replace elastic buffer with proper SRC (sample rate conversion)
- Dynamic rate adjustment based on buffer fill PLL
- Smooth corrections (no discrete sample drops)
- Required for multi-stream mixing (different TX clocks)

### Implementation
**Type:** Lock-free circular buffer (FreeRTOS compatible)

```c
typedef struct ring_buffer_t {
    uint8_t *buffer;
    size_t capacity;
    volatile size_t write_pos;
    volatile size_t read_pos;
    SemaphoreHandle_t mutex;
} ring_buffer_t;
```

**API:**
```c
ring_buffer_t* ring_buffer_create(size_t size);
void ring_buffer_destroy(ring_buffer_t *rb);
esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);
esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len);
size_t ring_buffer_available(ring_buffer_t *rb);
```

### Adaptive Behavior (Future)
- Track underrun/overrun rates
- Adjust target depth dynamically (2-6 frames)
- Sample dropping/duplication for clock drift compensation

### Latency Budget

| Component | Latency | Notes |
|-----------|---------|-------|
| TX Frame Buffering | 5ms | Single frame in progress |
| Network Transmission (1 hop) | 2-5ms | WiFi + mesh stack |
| Network Transmission (2 hops) | 5-10ms | Relay forwarding |
| Jitter Buffer Prefill | 15-20ms | 3-4 frames |
| I2S Output Buffering | 5ms | Single frame in DMA |
| **Total (1 hop)** | **27-35ms** | Excellent for live audio |
| **Total (2 hops)** | **32-40ms** | Still very low latency |

## Opus Codec Integration (v0.2)

### Purpose
Reduce bandwidth by 10× to enable larger mesh networks and stereo transmission

### Specification
- **Library:** ESP-ADF (already scaffolded in codebase)
- **Bitrate:** 64-128 kbps (configurable)
- **Frame Size:** 5ms (2.5ms also supported)
- **Compression Ratio:** ~10:1 (1.152 Mbps → 120 kbps @ 128 kbps)
- **Latency:** 5-20ms algorithmic delay

### ESP-ADF Integration Strategy
**Use existing ESP-ADF pipeline components:**
```c
#include "audio_element.h"
#include "opus_encoder.h"
#include "opus_decoder.h"

// TX: Encoder element
audio_element_handle_t opus_enc = opus_encoder_init(&opus_enc_cfg);

// RX: Decoder element
audio_element_handle_t opus_dec = opus_decoder_init(&opus_dec_cfg);
```

### Frame Format Change
**Raw PCM (v0.1):**
- Header: 12 bytes
- Payload: 720 bytes (5ms @ 48kHz 24-bit mono)
- **Total: 452 bytes/frame**

**Opus Compressed (v0.2):**
- Header: 12 bytes
- Payload: ~40 bytes (64 kbps, 5ms frame)
- **Total: ~52 bytes/frame** (8.7× reduction)

### Bandwidth Impact
**Single Stream:**
- Raw PCM: 1.152 Mbps
- Opus @ 64 kbps: 70 kbps (16× reduction)
- Opus @ 128 kbps: 140 kbps (8× reduction)

**Multi-Hop Scalability:**
- **3 hops, raw PCM:** ~3.5 Mbps (4-6 RX nodes)
- **3 hops, Opus 64k:** ~210 kbps (50+ RX nodes)
- **3 hops, Opus 128k:** ~420 kbps (30+ RX nodes)

### Implementation Path
1. Integrate ESP-ADF Opus encoder on TX
2. Integrate ESP-ADF Opus decoder on RX
3. Add codec negotiation to mesh frame header
4. Test quality at various bitrates (32k, 64k, 128k)
5. Make bitrate configurable per TX stream

## Multi-Stream Mixing (Future/v0.3)

### Vision
Multiple TX nodes stream simultaneously, RX nodes mix and spatialize streams

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   MULTI-STREAM RX MIXER                      │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Stream 1 (TX-A) ──┐                                         │
│                    │                                         │
│  Stream 2 (TX-B) ──┼──▶ ┌──────────────────┐               │
│                    │    │  Stream Mixer    │               │
│  Stream 3 (TX-C) ──┘    │  - Gain per TX   │──▶ Mix Out    │
│                         │  - Panning L/R   │               │
│                         │  - HPF/LPF       │               │
│                         └──────────────────┘               │
│                                                              │
│  Global Mix Config ──▶  Applied to ALL RX nodes             │
│  Per-RX Config     ──▶  Individual overrides (optional)     │
└─────────────────────────────────────────────────────────────┘
```

### Per-Stream Controls
**Global Mixer (applies to all RX):**
- **Gain:** -∞ to +12 dB per TX stream
- **Panning:** L/R stereo positioning (-1.0 = full left, +1.0 = full right)
- **Mute:** Per-stream on/off
- **HPF/LPF:** Crossover for subwoofer routing

**Per-RX Overrides:**
- Individual RX can override global mix
- Use case: Dedicated subwoofer RX (LPF only), monitor RX (specific stream solo)

### Mixing Algorithm
**Simple Additive Mixing (v0.3):**
```c
int32_t mixed_left = 0;
int32_t mixed_right = 0;

for (each active stream) {
    // Apply gain
    int32_t sample = stream_sample * stream_gain;
    
    // Apply panning (constant power)
    float left_gain = sqrt((1.0 - pan) / 2.0);
    float right_gain = sqrt((1.0 + pan) / 2.0);
    
    mixed_left += sample * left_gain;
    mixed_right += sample * right_gain;
}

// Clip to 24-bit range
mixed_left = CLAMP(mixed_left, -8388608, 8388607);
mixed_right = CLAMP(mixed_right, -8388608, 8388607);
```

### Mix Control Interface (v0.3)
**Option 1: MQTT Control Plane**
- TX/RX publish stream metadata to broker
- Control app (web/mobile) publishes mix settings
- RX nodes subscribe to global mix + per-RX configs

**Option 2: Mesh Control Messages**
- Dedicated control message type (priority queue)
- One RX elected as "mixer master"
- Mixer master broadcasts config to all RX

**Option 3: Web Interface**
- Each RX runs lightweight HTTP server
- Browser-based mixer UI (visualize streams, adjust faders)
- Config synced via mesh or external broker

### Subwoofer Routing Example
```c
// Global mix config
tx_stream_config_t stream_configs[3] = {
    {.stream_id = 1, .gain = 0.8, .pan = -0.5, .hpf = 80},   // TX-A (left)
    {.stream_id = 2, .gain = 0.8, .pan = 0.5,  .hpf = 80},   // TX-B (right)
    {.stream_id = 3, .gain = 1.0, .pan = 0.0,  .hpf = 0},    // TX-C (full range)
};

// Per-RX override for subwoofer unit
rx_override_config_t subwoofer_rx = {
    .rx_id = 5,
    .lpf = 80,          // Low-pass only
    .stream_mask = 0x07, // All streams summed
};
```

## Audio Configuration Files

### `lib/config/include/config/build.h` (Target State)

```c
#pragma once

// Audio configuration - 48 kHz, 24-bit mono with 5ms frames
#define AUDIO_SAMPLE_RATE      48000
#define AUDIO_BITS_PER_SAMPLE  24
#define AUDIO_CHANNELS         1     // Mono for v0.1
#define AUDIO_FRAME_MS         5     // 5ms frames
#define AUDIO_BYTES_PER_SAMPLE 3     // 24-bit = 3 bytes (packed format)
#define AUDIO_FRAME_SAMPLES    (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 240 samples
#define AUDIO_FRAME_BYTES      (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS)  // 720 bytes

// Opus compression (future)
#define OPUS_BITRATE_BPS       64000  // 64 kbps
#define OPUS_MAX_FRAME_BYTES   256    // Maximum Opus frame size

// Jitter buffer
#define JITTER_BUFFER_FRAMES   10     // 10 frames = 50ms @ 5ms/frame
#define JITTER_PREFILL_FRAMES  4      // Prefill 4 frames = 20ms startup latency
```

### sdkconfig.defaults (Audio-Specific Settings)

```ini
# Audio sample rate
CONFIG_AUDIO_SAMPLE_RATE_48000=y

# I2S Configuration
CONFIG_I2S_ENABLE_DEBUG_LOG=n
CONFIG_I2S_SUPPRESS_DEPRECATE_WARN=y

# ADC Configuration
CONFIG_ADC_SUPPRESS_DEPRECATE_WARN=y
CONFIG_ADC_CONTINUOUS_MODE_ENABLE=y

# TinyUSB (for future USB audio)
CONFIG_TINYUSB_ENABLED=y
CONFIG_TINYUSB_AUDIO_ENABLED=y

# ESP-ADF (for Opus codec)
CONFIG_ESP_ADF_ENABLE=y
```

## Testing Strategy

### Unit Tests (Per Component)

**Tone Generator:**
- [ ] Generate 440 Hz at 48 kHz, verify frequency accuracy
- [ ] Test frequency sweep 200-2000 Hz
- [ ] Check for DC offset, clipping

**ADC Input:**
- [ ] Verify 48 kHz sampling rate (measure via oscilloscope)
- [ ] Test 12→24-bit conversion (no clipping)
- [ ] Measure DC offset before/after HPF
- [ ] SNR measurement (pure tone input)

**I2S Output:**
- [ ] Verify 48 kHz output rate
- [ ] Test mono→stereo duplication (L==R)
- [ ] Oscilloscope check for I2S timing
- [ ] THD measurement with sine input

**Jitter Buffer:**
- [ ] Fill/drain at correct rate (no underrun)
- [ ] Handle simulated network jitter (random delays)
- [ ] Prefill behavior (wait for threshold)
- [ ] Underrun recovery (silence insertion)

### Integration Tests

**TX→RX Audio Path:**
- [ ] Tone generator → network → I2S output
- [ ] ADC input → network → I2S output
- [ ] Measure end-to-end latency (<40ms target)
- [ ] Continuous playback (10+ minutes, no glitches)

**Multi-Hop Audio:**
- [ ] 3-node chain (TX → Relay → RX)
- [ ] Measure latency increase per hop
- [ ] Verify no audio degradation
- [ ] Test under WiFi congestion

**Opus Codec (v0.2):**
- [ ] Compare raw PCM vs Opus quality (subjective)
- [ ] Measure codec latency
- [ ] Test various bitrates (32k, 64k, 128k)
- [ ] Bandwidth measurement (confirm 10× reduction)

## Performance Metrics

### Target Metrics (v0.1 - Raw PCM)

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| TX→RX Latency (1 hop) | <35ms | Timestamp in frame header |
| TX→RX Latency (2 hops) | <50ms | Timestamp + hop count |
| Jitter Buffer Underruns | <1/min | Counter in RX |
| Audio Dropout Duration | <100ms | Gap detection |
| THD+N (Tone→I2S) | <1% | Audio analyzer |
| CPU Usage (TX) | <30% | FreeRTOS stats |
| CPU Usage (RX) | <40% | FreeRTOS stats |

### Target Metrics (v0.2 - Opus)

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Codec Latency | <20ms | Benchmark encoder/decoder |
| Bandwidth Reduction | >8× | Compare PCM vs Opus sizes |
| Perceptual Quality | MOS >4.0 | Subjective listening test |
| CPU Usage (TX) | <50% | With encoder overhead |
| CPU Usage (RX) | <60% | With decoder overhead |

## Known Limitations & Constraints

### ESP32-S3 Hardware
- **ADC:** 12-bit resolution, 83 kHz max aggregate sample rate
- **I2S:** Stereo hardware, mono requires software duplication
- **CPU:** 240 MHz dual-core (shared with network/control layers)
- **RAM:** Limited heap for large buffers (use static allocation)

### Audio Quality Tradeoffs
- **Mono Transmission:** Saves bandwidth, but no stereo imaging
- **48 kHz:** Professional baseline, good for music, broadcast standard
- **12-bit ADC:** Adequate for line-level, marginal for mic-level
- **Jitter Buffer:** Adds latency (required for wireless stability)

### Scalability Limits (Raw PCM)
- **Single Hop:** 10-15 RX nodes before airtime saturation
- **Two Hops:** 6-8 RX nodes
- **Three Hops:** 4-6 RX nodes
- **Solution:** Opus compression unlocks 50+ nodes

## Future Enhancements

### v0.2: Opus Compression
- ESP-ADF integration (leverage existing scaffold)
- 64-128 kbps bitrate (10× bandwidth reduction)
- Adaptive bitrate based on hop count
- Multiple concurrent TX streams viable

### v0.3: Stereo Transmission
- 48 kHz stereo @ 2.3 Mbps (raw PCM)
- 48 kHz stereo @ 256 kbps (Opus 256k)
- True stereo imaging on RX output
- Prerequisite: Stereo input hardware (USB or I2S ADC)

### v0.4: Multi-Stream Mixing
- Stream ID routing (RX selects which TX to play)
- Additive mixing (multiple TX summed)
- Per-stream gain, panning, filtering
- Global mixer + per-RX overrides
- Control interface (MQTT, web UI, or mesh messages)

### v0.5: Advanced DSP
- **Spatial Audio:** Positional metadata, HRTF rendering
- **Adaptive EQ:** Per-room acoustic compensation
- **Packet Loss Concealment:** Interpolate missing frames
- **Clock Synchronization:** Network-wide sample-accurate timing
- **Multiband Compression:** Dynamic range control
- **Reverb/Effects:** Mesh-wide shared effects processing

### v0.6: High-Resolution Audio
- 96 kHz / 24-bit support (requires better ADC hardware)
- FLAC compression as alternative to Opus
- External I2S ADC/DAC (ES8388, PCM5102A)

## References

### ESP-IDF Documentation
- [I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
- [ADC Continuous Mode](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_continuous.html)
- [TinyUSB Audio Class](https://docs.tinyusb.org/en/latest/)

### ESP-ADF Documentation
- [ESP-ADF Programming Guide](https://docs.espressif.com/projects/esp-adf/en/latest/)
- [Opus Encoder Element](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/codecs/opus_encoder.html)
- [Opus Decoder Element](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/codecs/opus_decoder.html)

### Hardware Datasheets
- [UDA1334 I2S DAC](https://www.nxp.com/docs/en/data-sheet/UDA1334ATS.pdf)
- [ESP32-S3 Technical Reference](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [XIAO ESP32-S3 Pinout](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)

### Audio Processing
- [Opus Codec Overview](https://opus-codec.org/)
- [Jitter Buffer Design](https://datatracker.ietf.org/doc/html/rfc3550) (RTP/RTCP)
- [Constant Power Panning](https://www.cs.cmu.edu/~music/icm-online/readings/panlaws/)

---

*Document created for audio layer architecture planning. Ready for implementation alongside mesh network backend.*
