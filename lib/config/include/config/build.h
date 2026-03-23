#pragma once

#include <freertos/FreeRTOS.h>
#include <assert.h>

// ============================================================================
// Audio Configuration - 48 kHz, 16-bit, mono pipeline
// ============================================================================
// 
// CONFIGURATION HIERARCHY:
// 
// BASE VALUES (manually chosen):
//   - AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE
//   - AUDIO_FRAME_MS (Opus/codec frame duration)
//   - I2S_DMA_CHUNK_MS (low-level DMA timing - INDEPENDENT of codec frame)
//   - Buffer frame counts, task stack sizes
//
// DERIVED VALUES (calculated from base):
//   - Sample counts, byte sizes, DMA parameters
//   - Buffer sizes in bytes
//
// CRITICAL: I2S DMA chunking is intentionally DECOUPLED from Opus frame size.
// DMA uses small 10ms chunks regardless of whether Opus uses 10/20/40ms frames.
// ============================================================================

// ---- Base Stream Parameters ----
#define AUDIO_SAMPLE_RATE          48000
#define AUDIO_BITS_PER_SAMPLE      16
#define AUDIO_BYTES_PER_SAMPLE     (AUDIO_BITS_PER_SAMPLE / 8)   // 2 bytes
#define AUDIO_CHANNELS_MONO        1    // Internal pipeline (Opus)
#define AUDIO_CHANNELS_STEREO      2    // I2S / ES8388

// ---- High-Level Codec/Pipeline Frame (Opus frame == PCM frame) ----
// This is the frame size Opus encodes/decodes per call.
// Changing this affects Opus latency and packets-per-second.
#define AUDIO_FRAME_MS             20   // 20ms = 50 fps capture/encode (proven clean for local output)

// Derived: samples and bytes per codec frame
#define AUDIO_FRAME_SAMPLES        ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)  // 1920
#define AUDIO_FRAME_BYTES_MONO     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_MONO)    // 3840
#define AUDIO_FRAME_BYTES_STEREO   (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_STEREO)  // 7680

#define AUDIO_FRAME_BYTES          AUDIO_FRAME_BYTES_MONO

// ---- Low-Level I2S / DMA Configuration ----
// IMPORTANT: DMA chunking MUST stay small (10ms) regardless of AUDIO_FRAME_MS.
// The I2S driver handles accumulating small DMA chunks into larger codec frames.
#define I2S_DMA_CHUNK_MS           10   // 10ms chunks - good balance for latency
#define I2S_DMA_DESC_NUM           8    // 8 descriptors = 80ms total DMA buffer

// Derived: DMA samples and timing
#define I2S_DMA_CHUNK_SAMPLES      ((AUDIO_SAMPLE_RATE * I2S_DMA_CHUNK_MS) / 1000)  // 480
#define I2S_DMA_BUFFER_MS          (I2S_DMA_CHUNK_MS * I2S_DMA_DESC_NUM)            // 80ms

// This goes into chan_cfg.dma_frame_num - NEVER use AUDIO_FRAME_SAMPLES here!
#define I2S_DMA_FRAME_NUM          I2S_DMA_CHUNK_SAMPLES

// ============================================================================
// Opus Codec Configuration
// ============================================================================

#define OPUS_BITRATE               24000     // 24 kbps (lower airtime for GROUP multicast stability)
#define OPUS_COMPLEXITY            2         // Low complexity to reduce stack usage
#define OPUS_EXPECTED_LOSS_PCT     8         // Hint for in-band FEC tuning under mesh burst loss
#define OPUS_ENABLE_INBAND_FEC     1         // Improves concealment for isolated packet loss

// Opus frame duration is tied to the pipeline PCM frame duration
#define OPUS_FRAME_DURATION_MS     AUDIO_FRAME_MS

// Target bytes per Opus frame (for documentation/sizing estimates)
#define OPUS_TARGET_FRAME_BYTES    ((OPUS_BITRATE * OPUS_FRAME_DURATION_MS) / (8 * 1000))  // ~60 at 24kbps/20ms

// Max Opus frame size with headroom (~320 bytes typical at 64kbps/40ms)
#define OPUS_MAX_FRAME_BYTES       512

// Input activity detection (TX/COMBO waveform gating)
// Peak is measured on mono PCM samples (0..32767 absolute amplitude).
#define AUDIO_INPUT_ACTIVITY_PEAK_THRESHOLD  80
#define AUDIO_INPUT_ACTIVITY_HOLD_MS         1200
#define AUDIO_INPUT_ACTIVITY_HOLD_FRAMES \
    ((AUDIO_INPUT_ACTIVITY_HOLD_MS + AUDIO_FRAME_MS - 1) / AUDIO_FRAME_MS)

// ============================================================================
// Audio FFT Analysis (Portal Telemetry)
// ============================================================================
// FFT runs on local PCM frames and is exposed to the portal as 28 normalized bins.
#define FFT_ANALYSIS_SIZE              512       // Must be power-of-two and <= AUDIO_FRAME_SAMPLES
#define FFT_PORTAL_BIN_COUNT           28
#define FFT_MIN_FREQ_HZ                20
#define FFT_MAX_FREQ_HZ                20000
#define FFT_UPDATE_INTERVAL_FRAMES     2         // 20ms frames -> 25 FFT updates/sec
#define FFT_DB_FLOOR                   (-78.0f)
#define FFT_DB_CEIL                    (-12.0f)

// ============================================================================
// Network Configuration - ESP-WIFI-MESH
// ============================================================================

#define MESH_ID                "MeshNet-Audio-48"
#define MESH_SSID              MESH_ID
#define MESH_PASSWORD          "meshnet123"
#define MESH_CHANNEL           6
#define MESH_ROUTE_TABLE_SIZE  50        // Max nodes in routing table
#define MESH_AP_ASSOC_EXPIRE_S 90        // Longer auth window to reduce churn on relay links
#define MESH_MAX_LAYER         2         // Flat topology for close-range multi-OUT stability
#define MESH_XON_QSIZE         64        // Mesh RX queue size
// RF power in quarter-dBm units (80 = 20 dBm max, 52 = 13 dBm).
// In very close-range setups, reducing TX power can prevent receiver saturation/auth churn.
#define WIFI_TX_POWER_QDBM     52
#define UDP_PORT               3333
#define MAX_PACKET_SIZE        (NET_FRAME_HEADER_SIZE + OPUS_MAX_FRAME_BYTES)

// Mesh packet batching: combine N Opus frames per mesh packet to reduce mesh pps.
// With GROUP multicast: 50fps / N = total mesh packets/sec (no per-child multiply)
// CRITICAL TRADEOFF:
//   - Batch 1 → 50 pps, losing 1 packet = 20ms dropout (barely audible)
//   - Batch 2 → 25 pps, losing 1 packet = 40ms dropout (PLC can mask short gaps)
//   - Batch 6 → 8 pps, losing 1 packet = 120ms dropout (very audible)
// For 3+ OUT nodes, batch=2 reduces mesh packet rate while keeping losses concealable.
#define MESH_FRAMES_PER_PACKET     2   // 2 frames per packet = 25 pps

#define STREAM_SILENCE_TIMEOUT_MS  3000
// Require sustained silence beyond STREAM_SILENCE_TIMEOUT_MS before declaring loss.
// This avoids rapid stream state flapping under bursty packet delivery.
#define STREAM_SILENCE_CONFIRM_MS  1500

// TX continuity policy: when 1, TX keeps encoding/sending even during low input
// activity (continuous silent Opus frames) to avoid RX stream flap.
#define TX_CONTINUOUS_STREAMING     1


// USB networking (CDC-NCM)
// Portal IP is computed at runtime: 10.48.<mesh_hash>.<node_mac>/30
// See usb_portal_netif.c portal_netif_setup()
#define ENABLE_USB_PORTAL_NETWORK   1

// ============================================================================
// Buffer Configuration
// All sizes derived from base frame parameters
// ============================================================================

// Buffer depths in codec frames
// JITTER_BUFFER_FRAMES must be <= PCM_BUFFER_FRAMES (validated by static_assert below)
#define PCM_BUFFER_FRAMES          16   // 16 × 20ms = 320ms PCM buffer
#define OPUS_BUFFER_FRAMES         14   // 14 × 20ms = 280ms compressed burst tolerance

// Derived: buffer sizes in bytes
#define PCM_BUFFER_SIZE            (AUDIO_FRAME_BYTES_MONO * PCM_BUFFER_FRAMES)  // 61440 (16×3840)

// Opus buffer includes 2-byte length prefix per frame
#define OPUS_BUFFER_ITEM_MAX       (2 + OPUS_MAX_FRAME_BYTES)  // 514
#define OPUS_BUFFER_SIZE           (OPUS_BUFFER_ITEM_MAX * OPUS_BUFFER_FRAMES)  // 4112

// Jitter buffer (in codec frames)
// Priority is smooth, uninterrupted playback under multi-node contention.
// Use a deeper prefill and buffer for resilience; this intentionally increases latency.
#define JITTER_BUFFER_FRAMES       16   // 16 × 20ms = 320ms max depth (matches PCM buffer)
#define JITTER_PREFILL_FRAMES      14   // 14 × 20ms = 280ms startup prefill for no-drop priority
// Packet-loss concealment safety cap: insert at most this many synthetic frames per gap.
// This prevents long loss bursts from flooding buffers while still smoothing short gaps.
#define RX_PLC_MAX_FRAMES_PER_GAP  3

#define JITTER_BUFFER_BYTES        (AUDIO_FRAME_BYTES_MONO * JITTER_BUFFER_FRAMES)
#define JITTER_PREFILL_BYTES       (AUDIO_FRAME_BYTES_MONO * JITTER_PREFILL_FRAMES)

#define RING_BUFFER_SIZE           PCM_BUFFER_SIZE

// ============================================================================
// Task Stack Configuration (CENTRALIZED)
// 
// FreeRTOS xTaskCreate stack size is in WORDS (4 bytes on ESP32)
// Define in BYTES for clarity, then convert with macro
// ============================================================================

#define STACK_BYTES_TO_WORDS(bytes)  ((bytes) / sizeof(StackType_t))

// TX pipeline tasks
#define CAPTURE_TASK_STACK_BYTES     (8 * 1024)   // Includes FFT processing headroom
#define ENCODE_TASK_STACK_BYTES      (32 * 1024)  // Opus encoder: 20ms @ 48kHz with headroom for analysis buffers

// RX pipeline tasks
#define DECODE_TASK_STACK_BYTES      (20 * 1024)  // Includes FFT processing headroom
#define PLAYBACK_TASK_STACK_BYTES    (4 * 1024)   // I2S write only

// Network tasks
#define MESH_RX_TASK_STACK_BYTES     (4 * 1024)
#define HEARTBEAT_TASK_STACK_BYTES   (3 * 1024)

// Convert to FreeRTOS words
#define CAPTURE_TASK_STACK   STACK_BYTES_TO_WORDS(CAPTURE_TASK_STACK_BYTES)
#define ENCODE_TASK_STACK    STACK_BYTES_TO_WORDS(ENCODE_TASK_STACK_BYTES)
#define DECODE_TASK_STACK    STACK_BYTES_TO_WORDS(DECODE_TASK_STACK_BYTES)
#define PLAYBACK_TASK_STACK  STACK_BYTES_TO_WORDS(PLAYBACK_TASK_STACK_BYTES)
#define MESH_RX_TASK_STACK   STACK_BYTES_TO_WORDS(MESH_RX_TASK_STACK_BYTES)
#define HEARTBEAT_TASK_STACK STACK_BYTES_TO_WORDS(HEARTBEAT_TASK_STACK_BYTES)

// Task priorities (higher = more important)
#define CAPTURE_TASK_PRIO    4
#define ENCODE_TASK_PRIO     3
#define DECODE_TASK_PRIO     4
#define PLAYBACK_TASK_PRIO   5         // Highest - must keep I2S fed
#define MESH_RX_TASK_PRIO    6         // Network receive is time-critical
#define HEARTBEAT_TASK_PRIO  2

// ============================================================================
// Mesh Network Memory Configuration
// ============================================================================

#define MESH_RX_BUFFER_SIZE  1500      // MTU-sized receive buffer
#define DEDUPE_CACHE_SIZE    256       // Sequence deduplication cache

// ============================================================================
// Control Layer Configuration
// ============================================================================

#define CONTROL_TELEMETRY_RATE_MS    1000
#define CONTROL_HEARTBEAT_RATE_MS    2000
#define CONTROL_STATE_CACHE_TTL_MS   120000
#define CONTROL_STATE_CACHE_MAX_NODES 32

// ============================================================================
// Portal Configuration (TX/COMBO only)
// ============================================================================

#define PORTAL_HTTP_STACK_BYTES      (6 * 1024)
#define PORTAL_WS_PUSH_STACK_BYTES   (4 * 1024)
#define PORTAL_DNS_STACK_BYTES       (3 * 1024)
#define PORTAL_MIN_FREE_HEAP         (30 * 1024)  // Don't start portal if heap < 30KB

// ============================================================================
// Audio Output Configuration
// ============================================================================

// RX output volume scaling (0.0+) - amplifies/reduces level for DAC output
// UDA1334 and similar I2S DACs have no hardware volume control
// Increase if audio is too quiet, decrease if it clips (distorts)
// Start at 2.0x to compensate for quiet Opus decoder output
#define RX_OUTPUT_VOLUME           2.0f   // 200% = +6dB amplification

// ============================================================================
// Memory Monitoring Thresholds
// ============================================================================

#define MIN_FREE_HEAP_BYTES          (12 * 1024)  // Warn if heap < 12KB
#define MIN_STACK_HEADROOM_WORDS     256          // ~1KB safety margin

// ============================================================================
// Battery Monitoring
// Requires external voltage divider: BAT+ → R1 → GPIO4 → R2 → GND
// Use equal resistors (100K-1M). Higher = less drain.
// ============================================================================
#define BATTERY_ADC_GPIO           4            // A3 on XIAO ESP32-S3 (GPIO4)
#define BATTERY_ADC_CHANNEL        ADC_CHANNEL_3
#define BATTERY_VOLTAGE_FULL_MV    4200         // Fully charged LiPo
#define BATTERY_VOLTAGE_EMPTY_MV   3300         // Empty LiPo (safe cutoff)
#define BATTERY_DIVIDER_RATIO      2            // 1:1 divider = multiply by 2

// ============================================================================
// Configuration Sanity Checks (compile-time)
// ============================================================================

// Codec frame must be a multiple of DMA chunk for clean accumulation
_Static_assert((AUDIO_FRAME_MS % I2S_DMA_CHUNK_MS) == 0,
               "AUDIO_FRAME_MS must be a multiple of I2S_DMA_CHUNK_MS");

// FFT size must fit inside one codec PCM frame
_Static_assert(FFT_ANALYSIS_SIZE <= AUDIO_FRAME_SAMPLES,
               "FFT_ANALYSIS_SIZE must be <= AUDIO_FRAME_SAMPLES");

// FFT implementation expects a power-of-two size
_Static_assert((FFT_ANALYSIS_SIZE & (FFT_ANALYSIS_SIZE - 1)) == 0,
               "FFT_ANALYSIS_SIZE must be power-of-two");

// Jitter prefill can't exceed buffer depth
_Static_assert(JITTER_PREFILL_FRAMES <= JITTER_BUFFER_FRAMES,
               "JITTER_PREFILL_FRAMES must be <= JITTER_BUFFER_FRAMES");

// Jitter target must fit in the actual PCM ring buffer capacity
_Static_assert(JITTER_BUFFER_FRAMES <= PCM_BUFFER_FRAMES,
               "JITTER_BUFFER_FRAMES must be <= PCM_BUFFER_FRAMES");

// DMA buffer size per descriptor must fit in hardware limit (4092 bytes)
// For stereo 16-bit: 4 bytes per frame, so max ~1023 frames
_Static_assert((I2S_DMA_CHUNK_SAMPLES * AUDIO_CHANNELS_STEREO * AUDIO_BYTES_PER_SAMPLE) <= 4092,
               "I2S DMA buffer size exceeds hardware limit");
