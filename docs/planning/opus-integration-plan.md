# ESP-ADF + Opus Integration Plan for MeshNet Audio

**Date:** November 29, 2025  
**Status:** Implementation Plan  
**Goal:** Migrate to ESP-ADF with Opus codec for DJ-quality audio over ESP-WIFI-MESH

## Executive Summary

Uncompressed 24-bit/48kHz audio at 146 KB/s exceeds ESP-WIFI-MESH sustainable throughput for multi-TX/multi-RX scenarios. By migrating to **ESP-ADF (Espressif Audio Development Framework)** with Opus compression at 64-80 kbps, we:

1. **Reduce bandwidth** to ~10 KB/s (12-15× reduction)
2. **Replace custom ES8388 driver** with `esp_codec_dev` (official, maintained)
3. **Use audio_pipeline** for proper task-based streaming with ringbuffers
4. **Get Opus encoding/decoding** via `esp_audio_codec` component
5. **Keep mesh architecture** - rootless, self-healing as specified in `mesh-network-architecture.md`

## Why ESP-ADF?

| Feature | Current Custom Code | ESP-ADF |
|---------|---------------------|---------|
| ES8388 driver | Manual I2C commands | `esp_codec_dev` abstraction |
| Audio buffering | Custom ring buffer | Pipeline ringbuffers |
| Task management | Manual FreeRTOS | `audio_element` tasks |
| Opus codec | Not available | `opus_encoder`/`decoder` elements |
| Real-time handling | Ad-hoc | Designed for real-time audio |
| Maintenance | 100% on us | Community supported |

## Bandwidth Comparison

| Format | Frame Size | Packets/sec | Per-stream Bandwidth | Feasibility |
|--------|------------|-------------|---------------------|-------------|
| Raw 24-bit/48kHz | 5ms (720 bytes) | 200 | 146 KB/s | ❌ Queue overflow |
| Raw 16-bit/48kHz | 10ms (960 bytes) | 100 | 96 KB/s | ⚠️ Marginal |
| **Opus 64kbps** | **10ms (~80 bytes)** | **100** | **~10 KB/s** | ✅ Works great |
| Opus 80kbps | 10ms (~100 bytes) | 100 | ~12 KB/s | ✅ Works great |

## Espressif esp_audio_codec Performance (ESP32-S3)

From official benchmarks:

| Operation | Sample Rate | Channels | Memory | CPU Loading |
|-----------|-------------|----------|--------|-------------|
| Opus Encode | 48kHz | Stereo | 29.4 KB | 24.9% |
| Opus Decode | 48kHz | Stereo | 26.6 KB | 5.86% |

For **mono at 48kHz**, expect roughly half these values:
- Encode: ~15-20% CPU, ~20 KB RAM
- Decode: ~3-4% CPU, ~15 KB RAM

This leaves plenty of headroom for mesh networking and I2S tasks.

## ESP-ADF Pipeline Architecture

### TX/COMBO Pipeline
```
┌─────────────────────────────────────────────────────────────────┐
│ ESP-ADF Audio Pipeline (TX)                                     │
│                                                                  │
│  ┌────────────┐     ┌──────────────┐     ┌─────────────────┐   │
│  │ i2s_stream │ RB  │ opus_encoder │ RB  │ mesh_output     │   │
│  │ (READER)   ├────►│ (element)    ├────►│ (custom elem)   │   │
│  └────────────┘     └──────────────┘     └─────────────────┘   │
│        ↑                                          ↓             │
│   esp_codec_dev                          network_send_audio()  │
│   (ES8388 ADC)                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### RX Pipeline
```
┌─────────────────────────────────────────────────────────────────┐
│ ESP-ADF Audio Pipeline (RX)                                     │
│                                                                  │
│  ┌────────────────┐     ┌──────────────┐     ┌────────────┐    │
│  │ mesh_input     │ RB  │ opus_decoder │ RB  │ i2s_stream │    │
│  │ (custom elem)  ├────►│ (element)    ├────►│ (WRITER)   │    │
│  └────────────────┘     └──────────────┘     └────────────┘    │
│        ↑                                          ↓             │
│   audio_rx_callback                          UDA1334 DAC       │
│   from mesh                                  or ES8388 DAC     │
└─────────────────────────────────────────────────────────────────┘
```

### Key ESP-ADF Components

| Component | Role | Source |
|-----------|------|--------|
| `audio_pipeline` | Manages element tasks and ringbuffers | ESP-ADF core |
| `audio_element` | Base class for all processing blocks | ESP-ADF core |
| `i2s_stream` | I2S input/output with DMA | ESP-ADF streams |
| `opus_encoder` | Opus compression element | `esp_audio_codec` |
| `opus_decoder` | Opus decompression element | `esp_audio_codec` |
| `esp_codec_dev` | Hardware codec abstraction (ES8388) | ESP-ADF |
| `mesh_output` | **Custom** - sends to ESP-WIFI-MESH | We write this |
| `mesh_input` | **Custom** - receives from mesh | We write this |

### Current Flow (Broken)
```
TX: I2S → 24-bit PCM → 732 byte packets → Mesh (QUEUE FULL!) → RX
```

### New Flow (ESP-ADF + Opus)
```
TX: esp_codec_dev → i2s_stream → opus_encoder → mesh_output → Mesh
RX: Mesh → mesh_input → opus_decoder → i2s_stream → DAC
```

### Frame Timing

| Parameter | Current (Raw) | New (Opus) |
|-----------|---------------|------------|
| Frame duration | 5ms | 10ms |
| Packets/second | 200 | 100 |
| Audio latency (codec) | 0ms | ~5ms (encoder lookahead) |
| Jitter buffer | 20-50ms | 30-50ms |
| **End-to-end target** | <50ms | <50ms |

### Network Packet Format

```c
// Updated header for Opus frames
typedef struct __attribute__((packed)) {
    uint8_t magic;          // 0xA5 (NET_FRAME_MAGIC)
    uint8_t version;        // 2 (bump for Opus format)
    uint8_t type;           // NET_PKT_TYPE_AUDIO_OPUS (new type)
    uint8_t stream_id;      // Stream identifier
    uint16_t seq;           // Sequence number (network byte order)
    uint32_t timestamp;     // Sender timestamp in ms
    uint16_t payload_len;   // Opus frame size (variable, typ. 60-120 bytes)
    uint8_t ttl;            // Hop limit
    uint8_t flags;          // Reserved for future (FEC, etc.)
    // Payload follows: Opus encoded frame
} net_audio_header_t;

#define NET_PKT_TYPE_AUDIO_OPUS 0x11  // New type for Opus-compressed audio
```

## Implementation Steps

### Step 1: Add ESP-ADF as Git Submodule

```bash
cd meshnet-audio
git submodule add https://github.com/espressif/esp-adf.git components/esp-adf
git submodule update --init --recursive
```

Or add to `idf_component.yml`:
```yaml
dependencies:
  espressif/esp_audio_codec: "^2.3.0"
  espressif/esp-adf-libs: "^2.0.0"
```

### Step 2: Update platformio.ini

```ini
[env]
platform = espressif32
framework = espidf, arduino  ; if needed
board = esp32-s3-devkitc-1

; ESP-ADF component paths
build_flags = 
    -DCONFIG_AUDIO_BOARD_CUSTOM=1
    -I${PROJECT_DIR}/components/esp-adf/components/audio_pipeline/include
    -I${PROJECT_DIR}/components/esp-adf/components/audio_stream/include
    -I${PROJECT_DIR}/components/esp-adf/components/esp_codec_dev/include

lib_extra_dirs = 
    ${PROJECT_DIR}/components/esp-adf/components
```

### Step 3: New File Structure

```
lib/audio/
├── include/audio/
│   ├── opus_encoder.h      # TX-side Opus encoder wrapper
│   └── opus_decoder.h      # RX-side Opus decoder wrapper
└── src/
    ├── opus_encoder.c      # Encoder implementation
    └── opus_decoder.c      # Decoder implementation

lib/config/include/config/
└── build.h                 # Updated with Opus parameters
```

### 3. Configuration Updates (build.h)

```c
// Audio configuration - 48 kHz, 24-bit, mono with 10ms Opus frames
#define AUDIO_SAMPLE_RATE      48000
#define AUDIO_BITS_PER_SAMPLE  24       // Internal processing
#define AUDIO_CHANNELS         1        // Mono
#define AUDIO_FRAME_MS         10       // 10ms for Opus (was 5ms)
#define AUDIO_FRAME_SAMPLES    (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 480 samples

// Opus codec configuration
#define OPUS_BITRATE           64000    // 64 kbps (transparent for mono music)
#define OPUS_COMPLEXITY        5        // Balance of quality vs CPU (0-10)
#define OPUS_APPLICATION       OPUS_APPLICATION_AUDIO  // Music optimized
#define OPUS_MAX_FRAME_BYTES   256      // Max encoded frame size

// Jitter buffer (adjusted for 10ms frames)
#define JITTER_BUFFER_FRAMES   5        // 5 frames = 50ms
#define JITTER_PREFILL_FRAMES  3        // Prefill 3 frames = 30ms
```

### 4. Encoder API (opus_encoder.h)

```c
#pragma once
#include <stdint.h>
#include "esp_err.h"

// Opus encoder handle (opaque)
typedef struct opus_encoder_ctx* opus_encoder_handle_t;

// Initialize Opus encoder
// Returns handle or NULL on failure
opus_encoder_handle_t opus_encoder_init(void);

// Encode PCM samples to Opus
// - pcm_in: Input PCM samples (AUDIO_FRAME_SAMPLES × 16-bit)
// - opus_out: Output buffer (OPUS_MAX_FRAME_BYTES)
// - out_len: Actual encoded length (output)
// Returns ESP_OK on success
esp_err_t opus_encoder_process(opus_encoder_handle_t enc,
                                const int16_t *pcm_in,
                                uint8_t *opus_out,
                                size_t *out_len);

// Cleanup
void opus_encoder_deinit(opus_encoder_handle_t enc);
```

### 5. Decoder API (opus_decoder.h)

```c
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct opus_decoder_ctx* opus_decoder_handle_t;

// Initialize Opus decoder
opus_decoder_handle_t opus_decoder_init(void);

// Decode Opus frame to PCM
// - opus_in: Opus frame data
// - opus_len: Opus frame length
// - pcm_out: Output PCM buffer (AUDIO_FRAME_SAMPLES × 16-bit)
// Returns number of samples decoded, or negative on error
int opus_decoder_process(opus_decoder_handle_t dec,
                         const uint8_t *opus_in,
                         size_t opus_len,
                         int16_t *pcm_out);

// Packet loss concealment (generate replacement frame)
int opus_decoder_plc(opus_decoder_handle_t dec, int16_t *pcm_out);

void opus_decoder_deinit(opus_decoder_handle_t dec);
```

## TX/COMBO Changes

### combo/main.c - Audio Pipeline

```c
// Before (current):
void audio_task(void *arg) {
    uint8_t pcm_24bit[AUDIO_FRAME_BYTES];  // 720 bytes
    while (1) {
        es8388_read_frame(pcm_24bit);
        network_send_audio(pcm_24bit, AUDIO_FRAME_BYTES);
        // ...
    }
}

// After (with Opus):
void audio_task(void *arg) {
    opus_encoder_handle_t encoder = opus_encoder_init();
    int16_t pcm_16bit[AUDIO_FRAME_SAMPLES];     // 480 samples
    uint8_t opus_frame[OPUS_MAX_FRAME_BYTES];   // ~100 bytes typical
    size_t opus_len;
    
    while (1) {
        // Read 10ms of audio (480 samples @ 48kHz)
        es8388_read_frame_16bit(pcm_16bit);
        
        // Encode to Opus
        esp_err_t err = opus_encoder_process(encoder, pcm_16bit, opus_frame, &opus_len);
        if (err == ESP_OK) {
            // Send compressed frame over mesh (~100 bytes instead of 720)
            network_send_audio_opus(opus_frame, opus_len);
        }
    }
}
```

## RX Changes

### rx/main.c - Audio Pipeline

```c
// Audio receive callback (with Opus decoding)
void audio_rx_callback(const uint8_t *opus_data, size_t opus_len, 
                       uint16_t seq, uint32_t timestamp) {
    static opus_decoder_handle_t decoder = NULL;
    static int16_t pcm_16bit[AUDIO_FRAME_SAMPLES];
    
    if (!decoder) {
        decoder = opus_decoder_init();
    }
    
    // Decode Opus to PCM
    int samples = opus_decoder_process(decoder, opus_data, opus_len, pcm_16bit);
    
    if (samples > 0) {
        // Write to jitter buffer
        jitter_buffer_write(pcm_16bit, samples);
    }
}

// Handle packet loss with PLC
void handle_packet_loss(opus_decoder_handle_t dec, uint16_t expected_seq) {
    int16_t plc_frame[AUDIO_FRAME_SAMPLES];
    int samples = opus_decoder_plc(dec, plc_frame);
    if (samples > 0) {
        jitter_buffer_write(plc_frame, samples);
    }
}
```

## Network Layer Changes

### mesh_net.c Updates

1. Add new packet type `NET_PKT_TYPE_AUDIO_OPUS`
2. Update `network_send_audio()` to accept variable-length Opus frames
3. Update RX callback to pass Opus data to decoder

### Mesh Throughput Analysis

With Opus @ 64kbps:
- **Per TX stream:** ~10 KB/s = ~80 kbps with headers
- **4 TX nodes:** ~40 KB/s = ~320 kbps total
- **8 RX nodes (unicast each):** ~320 KB/s max
- **ESP-WIFI-MESH capacity:** 1-2 Mbps sustained

**Headroom factor:** 3-6× margin for retries, control traffic, and interference.

## Migration Strategy

### Phase 1: Add Opus Infrastructure (No Breaking Changes)
1. Add `esp_audio_codec` dependency
2. Create `opus_encoder.c` and `opus_decoder.c` wrappers
3. Add new packet type `NET_PKT_TYPE_AUDIO_OPUS`
4. Test encode/decode in isolation

### Phase 2: TX Integration
1. Modify COMBO audio task to encode
2. Keep sending raw PCM in parallel (backward compat)
3. Test with RX still expecting raw

### Phase 3: RX Integration  
1. Add Opus decoder to RX audio callback
2. Handle both raw and Opus packets (version field)
3. Test end-to-end

### Phase 4: Remove Raw Path
1. Remove raw PCM transmission
2. Update frame timing to 10ms
3. Tune jitter buffer
4. Performance testing

## Testing Checklist

### Unit Tests
- [ ] Opus encoder produces valid frames
- [ ] Opus decoder reconstructs audio correctly
- [ ] Round-trip PCM → Opus → PCM quality acceptable
- [ ] PLC produces reasonable output

### Integration Tests
- [ ] COMBO encodes and sends over mesh
- [ ] RX decodes and plays audio
- [ ] Multiple RX nodes receive simultaneously
- [ ] Latency <50ms end-to-end
- [ ] No queue overflow errors

### Stress Tests
- [ ] 10+ minute continuous playback
- [ ] Multiple TX nodes (2-4)
- [ ] Multiple RX nodes (4-8)
- [ ] Node join/leave during stream
- [ ] Root migration during stream

## Quality Validation

Opus at 64kbps mono should be transparent for:
- Full-range music (20Hz - 20kHz)
- DJ mixes with complex transients
- Electronic/bass-heavy content

Subjective testing:
- [ ] A/B comparison vs. uncompressed
- [ ] High-frequency content (cymbals, hi-hats)
- [ ] Low-frequency content (bass, kick drums)
- [ ] Transient response (snare hits)

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Opus CPU too high | Audio dropouts | Use complexity 1-5, optimize task priority |
| Memory pressure | Crashes | Dedicated audio core, stack size 30KB |
| Clock drift TX/RX | Buffer over/underrun | Sample rate matching in jitter buffer |
| Packet loss spikes | Audio artifacts | Opus PLC + FEC in future |

## Timeline Estimate

| Phase | Duration | Notes |
|-------|----------|-------|
| Infrastructure | 2-3 hours | Dependency, wrappers |
| TX Integration | 2-3 hours | Encode pipeline |
| RX Integration | 2-3 hours | Decode pipeline |
| Testing & Tuning | 3-4 hours | End-to-end validation |
| **Total** | **~1 day** | For basic working implementation |

## References

- [ESP Audio Codec Component](https://components.espressif.com/components/espressif/esp_audio_codec)
- [Opus Codec](https://opus-codec.org/)
- [RFC 6716 - Opus Specification](https://tools.ietf.org/html/rfc6716)
- [ESP-ADF Opus Decoder API](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/codecs/opus_decoder.html)

---

*This plan maintains the ESP-WIFI-MESH architecture for self-healing, rootless mesh while solving the bandwidth problem through compression.*
