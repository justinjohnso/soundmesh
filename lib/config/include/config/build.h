#pragma once

#include <freertos/FreeRTOS.h>
#include <assert.h>

// ============================================================================
// Audio Configuration - 48 kHz, 24-bit-capable internal pipeline
// ============================================================================
// 
// CONFIGURATION HIERARCHY:
// 
// BASE VALUES (manually chosen):
//   - AUDIO_SAMPLE_RATE, AUDIO_BOUNDARY_BITS_PER_SAMPLE, AUDIO_INTERNAL_BITS_PER_SAMPLE
//   - AUDIO_FRAME_TARGET_MS / fallback knobs
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
#define AUDIO_BOUNDARY_BITS_PER_SAMPLE 16  // Opus + codec/I2S boundary depth
#define AUDIO_INTERNAL_BITS_PER_SAMPLE 16  // Internal processing target (pure 16-bit for CPU relief)
#define AUDIO_BITS_PER_SAMPLE      AUDIO_BOUNDARY_BITS_PER_SAMPLE
#define AUDIO_BYTES_PER_SAMPLE     (AUDIO_BOUNDARY_BITS_PER_SAMPLE / 8)   // 2 bytes
#define AUDIO_INTERNAL_BYTES_PER_SAMPLE 2  // Stored in int16_t lanes
#define AUDIO_CHANNELS_MONO        1    // Internal pipeline (Opus)
#define AUDIO_CHANNELS_STEREO      2    // I2S / ES8388

// ---- High-Level Codec/Pipeline Frame (Opus frame == PCM frame) ----
// This is the frame size Opus encodes/decodes per call.
// Changing this affects Opus latency and packets-per-second.
#define AUDIO_FRAME_TARGET_MS      20   // Stable cadence for current pilot
#define AUDIO_FRAME_FALLBACK_MS    20   // Keep deterministic fallback equal to target
#define AUDIO_ALLOW_5MS_CADENCE    0    // Disable 5ms rollout during stability recovery
#define AUDIO_CODEC_SUPPORTS_5MS   0    // Gate 5ms path off while recovering audio quality
#if AUDIO_ALLOW_5MS_CADENCE && AUDIO_CODEC_SUPPORTS_5MS
#define AUDIO_FRAME_MS             AUDIO_FRAME_TARGET_MS
#else
#define AUDIO_FRAME_MS             AUDIO_FRAME_FALLBACK_MS
#endif
#define AUDIO_FRAME_EFFECTIVE_MS   AUDIO_FRAME_MS
#define AUDIO_FRAME_FALLBACK_ACTIVE (AUDIO_FRAME_EFFECTIVE_MS != AUDIO_FRAME_TARGET_MS)

// Derived: samples and bytes per codec frame
#define AUDIO_FRAME_SAMPLES        ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)  // 1920
#define AUDIO_FRAME_BYTES_MONO     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_MONO)    // 3840
#define AUDIO_FRAME_BYTES_STEREO   (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_STEREO)  // 7680
#define AUDIO_FRAME_BYTES_INTERNAL_MONO (AUDIO_FRAME_SAMPLES * AUDIO_INTERNAL_BYTES_PER_SAMPLE * AUDIO_CHANNELS_MONO)
#define AUDIO_FRAME_BYTES_INTERNAL_STEREO (AUDIO_FRAME_SAMPLES * AUDIO_INTERNAL_BYTES_PER_SAMPLE * AUDIO_CHANNELS_STEREO)

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

#define OPUS_BITRATE               64000     // 64 kbps for 'crystal clear' audio; multicast keeps airtime constant
#define OPUS_COMPLEXITY            0         // Minimal complexity for maximum CPU relief on S3
#define OPUS_EXPECTED_LOSS_PCT     20         // conservative retune: modest FEC hint bump for GROUP|NONBLOCK loss bursts
#define OPUS_ENABLE_INBAND_FEC     1         // Improves concealment for isolated packet loss

// Opus frame duration is tied to the pipeline PCM frame duration
#define OPUS_FRAME_DURATION_MS     AUDIO_FRAME_EFFECTIVE_MS

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

// USB input capture/runtime policy (SRC USB mode)
#define USB_AUDIO_BUFFER_MS                  200
#define USB_AUDIO_FRAME_BYTES_STEREO         (AUDIO_CHANNELS_STEREO * AUDIO_BYTES_PER_SAMPLE)
#define USB_AUDIO_BUFFER_BYTES \
    (((AUDIO_SAMPLE_RATE * USB_AUDIO_BUFFER_MS) / 1000) * USB_AUDIO_FRAME_BYTES_STEREO)
#define USB_AUDIO_INACTIVITY_TIMEOUT_MS      1500
#define USB_AUDIO_INACTIVITY_CONFIRM_MS      300
#define USB_AUDIO_INACTIVITY_TIMEOUT_FRAMES \
    ((USB_AUDIO_INACTIVITY_TIMEOUT_MS + AUDIO_FRAME_MS - 1) / AUDIO_FRAME_MS)
#define USB_AUDIO_INACTIVITY_CONFIRM_FRAMES \
    ((USB_AUDIO_INACTIVITY_CONFIRM_MS + AUDIO_FRAME_MS - 1) / AUDIO_FRAME_MS)

// ============================================================================
// Audio FFT Analysis (Portal Telemetry)
// ============================================================================
// FFT runs on local PCM frames and is exposed to the portal as 28 normalized bins.
#if AUDIO_FRAME_SAMPLES >= 512
#define FFT_ANALYSIS_SIZE              512
#elif AUDIO_FRAME_SAMPLES >= 256
#define FFT_ANALYSIS_SIZE              256
#else
#define FFT_ANALYSIS_SIZE              128
#endif
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
#define MESH_DISABLED_ROUTER_SSID "MESHNET_DISABLED"  // Valid non-empty placeholder required by esp_mesh_set_config
#define MESH_CHANNEL           11        // Channel 11 for reduced 2.4GHz interference
#define MESH_ROUTE_TABLE_SIZE  50        // Max nodes in routing table
#define MESH_AP_ASSOC_EXPIRE_S 300       // Extended auth window (5 min) to prevent audio-load disconnects
#define MESH_MAX_LAYER         6         // Allow relay paths for weak/far OUT nodes
#define MESH_XON_QSIZE         128       // Increased mesh RX queue for burst tolerance
// RF power in quarter-dBm units (80 = 20 dBm max, 52 = 13 dBm).
// In very close-range setups, reducing TX power can prevent receiver saturation/auth churn.
#define WIFI_TX_POWER_QDBM     52
#define UDP_PORT               3333
// Mesh packet batching: combine N Opus frames per mesh packet to reduce mesh pps.
// With GROUP multicast: 50fps / N = total mesh packets/sec (no per-child multiply)
// CRITICAL TRADEOFF:
//   - Batch 1 → 50 pps, losing 1 packet = 20ms dropout (barely audible)
//   - Batch 2 → 25 pps, losing 1 packet = 40ms dropout (PLC can mask short gaps)
//   - Batch 6 → 8 pps, losing 1 packet = 120ms dropout (very audible)
#define MESH_FRAMES_PER_PACKET     2   // 2 frames per packet = 25 pps; sweet spot for reliability vs overhead
#define MESH_OPUS_BATCH_MAX_BYTES  (MESH_FRAMES_PER_PACKET * (2 + OPUS_MAX_FRAME_BYTES))  // 1028 bytes
#define MAX_PACKET_SIZE            (NET_FRAME_HEADER_SIZE + MESH_OPUS_BATCH_MAX_BYTES)
// Demo transport baseline selected from 2026-03-29 send-mode A/B artifact.
// Root fanout runs with MESH_DATA_GROUP | MESH_DATA_NONBLOCK in mesh_tx.c.
#define TRANSPORT_SETTINGS_PROFILE_ID "baseline-current"
#define TRANSPORT_ROOT_FANOUT_MODE    "GROUP|NONBLOCK"
#define TRANSPORT_TO_ROOT_MODE        "TODS|NONBLOCK"

#define STREAM_SILENCE_TIMEOUT_MS  3000
// Require sustained silence beyond STREAM_SILENCE_TIMEOUT_MS before declaring loss.
// This avoids rapid stream state flapping under bursty packet delivery.
#define STREAM_SILENCE_CONFIRM_MS  1500

// OUT self-heal rejoin policy (stream-starvation recovery)
// Circuit breaker prevents endless reconnect churn on persistent upstream faults.
#define OUT_REJOIN_STARVATION_MS     60000   // 60s no-data timeout triggers rejoin
#define OUT_REJOIN_MIN_INTERVAL_MS   2500    // Minimum 2.5s between rejoin attempts
#define OUT_REJOIN_MAX_ATTEMPTS      3       // Max retries before cooldown
#define OUT_REJOIN_WINDOW_MS         (15 * 60 * 1000)  // 15-minute eligibility window
#define OUT_REJOIN_COOLDOWN_MS       (15 * 60 * 1000)  // 15-minute cooldown after max attempts

// TX continuity policy: when 1, TX keeps encoding/sending even during low input
// activity (continuous silent Opus frames) to avoid RX stream flap.
#define TX_CONTINUOUS_STREAMING     1


// USB portal rollout toggles (phased re-enable safety controls)
// - SRC flag gates TX/COMBO portal startup.
// - OUT flag gates RX portal startup (kept OFF during current recovery phase).
// Portal IP is computed at runtime: 10.48.<mesh_hash>.<node_mac>/30
// See usb_portal_netif.c portal_netif_setup()
#define ENABLE_SRC_USB_PORTAL_NETWORK   0
#define ENABLE_OUT_USB_PORTAL_NETWORK   0

// ============================================================================
// Buffer Configuration
// All sizes derived from base frame parameters
// ============================================================================

// Buffer depths in codec frames
// JITTER_BUFFER_FRAMES must be <= PCM_BUFFER_FRAMES (validated by static_assert below)
#define PCM_BUFFER_FRAMES          16    // 16 × 20ms = 320ms PCM buffer (memory-safe limit)
#define OPUS_BUFFER_FRAMES         16    // 16 × 20ms = 320ms compressed burst tolerance

// Derived: buffer sizes in bytes
#define PCM_BUFFER_SIZE            (AUDIO_FRAME_BYTES_INTERNAL_MONO * PCM_BUFFER_FRAMES)

// Opus buffer includes 2-byte length prefix per frame
#define OPUS_BUFFER_ITEM_MAX       (3 + OPUS_MAX_FRAME_BYTES)  // flags + len + payload
#define OPUS_BUFFER_SIZE           (OPUS_BUFFER_ITEM_MAX * OPUS_BUFFER_FRAMES)  // 4112

// Jitter buffer (in codec frames)
// Priority is smooth, uninterrupted playback under multi-node contention.
// Use a deeper prefill and buffer for resilience; this intentionally increases latency.
#define JITTER_BUFFER_FRAMES       12    // 12 × 20ms = 240ms max depth
#define JITTER_PREFILL_FRAMES      6    // restored to 6 (120ms) for test compatibility; adaptive logic handles growth
#define JITTER_HYSTERESIS_HOLD_FRAMES 3
#define JITTER_ADAPTIVE_DECAY_FRAMES 500
// Packet-loss concealment safety cap: insert at most this many synthetic frames per gap.
// This prevents long loss bursts from flooding buffers while still smoothing short gaps.
#define RX_PLC_MAX_FRAMES_PER_GAP  5
#define RX_BURST_LOSS_THRESHOLD     2
// Drop packets that arrive this many frames behind the latest accepted sequence.
// Larger backward jumps are treated as sequence discontinuities (new baseline).
#define RX_MAX_STALE_FRAMES_TO_DROP 24
// Decode fairness cap: limit RX decode bursts per scheduler slice to avoid CPU monopolization.
#define RX_DECODE_MAX_ITEMS_PER_CYCLE 8
// Stop decoding when PCM queue is near full so playback cadence can drain without catch-up bursts.
#define RX_PCM_HIGH_WATER_FRAMES   10
#define RX_PCM_HIGH_WATER_BYTES    (AUDIO_FRAME_BYTES_INTERNAL_MONO * RX_PCM_HIGH_WATER_FRAMES)
// RX underrun smoothing policy:
// - hold last good frame briefly to mask isolated misses
// - then fade toward silence for sustained misses
// - finally force rebuffer to avoid playing stale tails indefinitely
#define RX_UNDERRUN_CONCEAL_FRAMES  1
#define RX_UNDERRUN_FADE_FRAMES     4
#define RX_UNDERRUN_REBUFFER_MISSES 15

#define JITTER_BUFFER_BYTES        (AUDIO_FRAME_BYTES_INTERNAL_MONO * JITTER_BUFFER_FRAMES)
#define JITTER_PREFILL_BYTES       (AUDIO_FRAME_BYTES_INTERNAL_MONO * JITTER_PREFILL_FRAMES)

#define RING_BUFFER_SIZE           PCM_BUFFER_SIZE

// ============================================================================
// Task Stack Configuration (CENTRALIZED)
// 
// ESP-IDF FreeRTOS stack size arguments are in BYTES.
// ============================================================================

// TX pipeline tasks
#define CAPTURE_TASK_STACK_BYTES     (8 * 1024)   // Includes FFT processing headroom
// 24KB keeps encode task creation reliable with mesh+portal-linked builds on S3 internal RAM.
#define ENCODE_TASK_STACK_BYTES      (32 * 1024)  // Recovery baseline stack budget

// RX pipeline tasks
#define DECODE_TASK_STACK_BYTES      (20 * 1024)  // Includes FFT processing headroom
#define PLAYBACK_TASK_STACK_BYTES    (4 * 1024)   // I2S write only

// Network tasks
#define MESH_RX_TASK_STACK_BYTES     (4 * 1024)
#define HEARTBEAT_TASK_STACK_BYTES   (3 * 1024)

// Task stack sizes passed directly to xTaskCreate* (in bytes)
#define CAPTURE_TASK_STACK   CAPTURE_TASK_STACK_BYTES
#define ENCODE_TASK_STACK    ENCODE_TASK_STACK_BYTES
#define DECODE_TASK_STACK    DECODE_TASK_STACK_BYTES
#define PLAYBACK_TASK_STACK  PLAYBACK_TASK_STACK_BYTES
#define MESH_RX_TASK_STACK   MESH_RX_TASK_STACK_BYTES
#define HEARTBEAT_TASK_STACK HEARTBEAT_TASK_STACK_BYTES

// Task priorities (higher = more important)
#define CAPTURE_TASK_PRIO    5
#define ENCODE_TASK_PRIO     4
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
#define CONTROL_TIMER_JITTER_MS      500
#define CONTROL_STATE_CACHE_TTL_MS   120000
#define CONTROL_STATE_CACHE_MAX_NODES 32

// ============================================================================
// Portal Configuration (TX/COMBO only)
// ============================================================================

#define PORTAL_HTTP_STACK_BYTES      (6 * 1024)
#define PORTAL_WS_PUSH_STACK_BYTES   (4 * 1024)
#define PORTAL_DNS_STACK_BYTES       (3 * 1024)
#define PORTAL_OTA_STACK_BYTES       (8 * 1024)
#define PORTAL_MIN_FREE_HEAP         (36 * 1024)  // Skip portal if free heap is too low
#define PORTAL_MIN_LARGEST_BLOCK     (16 * 1024)  // Guard against fragmented heap at startup
#define PORTAL_INIT_SETTLE_MS        400          // Let audio tasks settle before portal startup

// Portal API schema versions and rate limits
#define PORTAL_STATUS_SCHEMA_VERSION 1            // /api/status schema version
#define PORTAL_CONTROL_METRICS_SCHEMA_VERSION 1   // /api/control/metrics schema version
#define PORTAL_CONTROL_METRICS_JSON_BUF_SIZE 512  // /api/control/metrics response buffer (worst-case payload < 400B)
#define PORTAL_CONTROL_RATE_LIMIT_WINDOW_MS 5000  // 5s rate limit window
#define PORTAL_CONTROL_RATE_LIMIT_MAX_REQUESTS 10 // Max 10 requests per window
#define PORTAL_CDC_LOG_MIRROR_ENABLED 1           // Mirror ESP logs to TinyUSB CDC ACM channel 0

// Portal Security
#define PORTAL_REQUIRE_CONTROL_AUTH    1          // Enable token-based auth for POST/WS
#define PORTAL_ADMIN_TOKEN             "soundmesh2026" // Default pilot token

// ============================================================================
// Audio Output Configuration
// ============================================================================

// RX output volume scaling (0.0+) - amplifies/reduces level for DAC output
// UDA1334 and similar I2S DACs have no hardware volume control
// Increase if audio is too quiet, decrease if it clips (distorts)
// Start at 2.0x to compensate for quiet Opus decoder output
#define RX_OUTPUT_VOLUME           2.0f   // baseline-current profile: 200% (+6dB) amplification

// Output gain control (mixer feature) - range 0-400% (0.0x to 4.0x multiplier)
#define OUT_OUTPUT_GAIN_MIN_PCT    0     // 0% = mute
#define OUT_OUTPUT_GAIN_DEFAULT_PCT 200  // baseline-current profile: 2.0x default gain
#define OUT_OUTPUT_GAIN_MAX_PCT    400   // baseline-current profile: 4.0x max amplification

// Mixer gain range constants (dB). Used by portal API and adf_pipeline gain setters.
// Values <= MIXER_MIN_GAIN_DB are treated as -inf (silence).
#define MIXER_MIN_GAIN_DB          (-60.0f)
#define MIXER_MAX_OUTPUT_GAIN_DB   (12.0f)
#define MIXER_MIN_INPUT_GAIN_DB    (-18.0f)
#define MIXER_MAX_INPUT_GAIN_DB    (18.0f)
// Epsilon for float unity-gain check: skip gain loop when linear ≈ 1.0
#define MIXER_GAIN_UNITY_EPSILON   (0.0001f)

// Mixer control/state schema (multi-stream command plane)
#define MIXER_SCHEMA_VERSION           2
#define MIXER_MAX_STREAMS              4
#define MIXER_STREAM_ID_MIN            1
#define MIXER_STREAM_ID_MAX            MIXER_MAX_STREAMS
#define MIXER_STREAM_GAIN_MIN_PCT      0
#define MIXER_STREAM_GAIN_DEFAULT_PCT  100
#define MIXER_STREAM_GAIN_MAX_PCT      400

// ============================================================================
// Memory Monitoring Thresholds
// ============================================================================

#define MIN_FREE_HEAP_BYTES          (12 * 1024)  // Warn if heap < 12KB
#define MIN_STACK_HEADROOM_BYTES     1024         // 1KB minimum free stack safety margin
#define MEMORY_MONITOR_TASK_STACK_BYTES (16 * 1024)
#define MEMORY_MONITOR_TASK_STACK    MEMORY_MONITOR_TASK_STACK_BYTES
#define MEMORY_MONITOR_TASK_PRIO     1

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
_Static_assert(((AUDIO_FRAME_MS % I2S_DMA_CHUNK_MS) == 0) ||
               ((I2S_DMA_CHUNK_MS % AUDIO_FRAME_MS) == 0),
               "AUDIO_FRAME_MS and I2S_DMA_CHUNK_MS must be integer-aligned");

_Static_assert(sizeof(MESH_DISABLED_ROUTER_SSID) > 1,
               "MESH_DISABLED_ROUTER_SSID must be non-empty");

// FFT size must fit inside one codec PCM frame
_Static_assert(FFT_ANALYSIS_SIZE <= AUDIO_FRAME_SAMPLES,
               "FFT_ANALYSIS_SIZE must be <= AUDIO_FRAME_SAMPLES");

// FFT implementation expects a power-of-two size
_Static_assert((FFT_ANALYSIS_SIZE & (FFT_ANALYSIS_SIZE - 1)) == 0,
               "FFT_ANALYSIS_SIZE must be power-of-two");

// Jitter prefill can't exceed buffer depth
_Static_assert(JITTER_PREFILL_FRAMES <= JITTER_BUFFER_FRAMES,
               "JITTER_PREFILL_FRAMES must be <= JITTER_BUFFER_FRAMES");

_Static_assert(JITTER_HYSTERESIS_HOLD_FRAMES >= 1,
               "JITTER_HYSTERESIS_HOLD_FRAMES must be >= 1");

_Static_assert(JITTER_ADAPTIVE_DECAY_FRAMES >= 1,
               "JITTER_ADAPTIVE_DECAY_FRAMES must be >= 1");

_Static_assert(RX_UNDERRUN_CONCEAL_FRAMES >= 1,
               "RX_UNDERRUN_CONCEAL_FRAMES must be >= 1");

_Static_assert(RX_UNDERRUN_FADE_FRAMES >= 1,
               "RX_UNDERRUN_FADE_FRAMES must be >= 1");

_Static_assert(RX_UNDERRUN_REBUFFER_MISSES >= (RX_UNDERRUN_CONCEAL_FRAMES + RX_UNDERRUN_FADE_FRAMES),
               "RX_UNDERRUN_REBUFFER_MISSES must allow conceal+fade before forced rebuffer");

// Jitter target must fit in the actual PCM ring buffer capacity
_Static_assert(JITTER_BUFFER_FRAMES <= PCM_BUFFER_FRAMES,
               "JITTER_BUFFER_FRAMES must be <= PCM_BUFFER_FRAMES");

_Static_assert(RX_PCM_HIGH_WATER_FRAMES >= JITTER_PREFILL_FRAMES,
               "RX_PCM_HIGH_WATER_FRAMES must stay above startup prefill");

#ifdef NET_FRAME_HEADER_SIZE
// Network packet buffer sizing must include the largest supported batched Opus payload.
_Static_assert(MAX_PACKET_SIZE >= (NET_FRAME_HEADER_SIZE + MESH_OPUS_BATCH_MAX_BYTES),
               "MAX_PACKET_SIZE must hold header + max batched Opus payload");

// RX task packet buffer must be large enough for the largest encoded TX packet.
_Static_assert(MAX_PACKET_SIZE <= MESH_RX_BUFFER_SIZE,
               "MESH_RX_BUFFER_SIZE must be >= MAX_PACKET_SIZE");
#endif

// DMA buffer size per descriptor must fit in hardware limit (4092 bytes)
// For stereo 16-bit: 4 bytes per frame, so max ~1023 frames
_Static_assert((I2S_DMA_CHUNK_SAMPLES * AUDIO_CHANNELS_STEREO * AUDIO_BYTES_PER_SAMPLE) <= 4092,
               "I2S DMA buffer size exceeds hardware limit");
