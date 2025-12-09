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
#define AUDIO_FRAME_MS             20   // 20ms = 50 fps (reduced from 40ms to fix stack overflow)

// Derived: samples and bytes per codec frame
#define AUDIO_FRAME_SAMPLES        ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)  // 1920
#define AUDIO_FRAME_BYTES_MONO     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_MONO)    // 3840
#define AUDIO_FRAME_BYTES_STEREO   (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS_STEREO)  // 7680

// Legacy alias for code expecting mono PCM frame size
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

#define OPUS_BITRATE               64000     // 64 kbps (transparent for mono music)
#define OPUS_COMPLEXITY            2         // Low complexity to reduce stack usage (was 5, overflow)

// Opus frame duration is tied to the pipeline PCM frame duration
#define OPUS_FRAME_DURATION_MS     AUDIO_FRAME_MS

// Target bytes per Opus frame (for documentation/sizing estimates)
#define OPUS_TARGET_FRAME_BYTES    ((OPUS_BITRATE * OPUS_FRAME_DURATION_MS) / (8 * 1000))  // ~320

// Max Opus frame size with headroom (~320 bytes typical at 64kbps/40ms)
#define OPUS_MAX_FRAME_BYTES       512

// ============================================================================
// Network Configuration - ESP-WIFI-MESH
// ============================================================================

#define MESH_ID                "MeshNet-Audio-48"
#define MESH_SSID              MESH_ID
#define MESH_PASSWORD          "meshnet123"
#define MESH_CHANNEL           6
#define MESH_ROUTE_TABLE_SIZE  50        // Max nodes in routing table
#define UDP_PORT               3333      // Legacy fallback
#define MAX_PACKET_SIZE        (NET_FRAME_HEADER_SIZE + OPUS_MAX_FRAME_BYTES)

// USB networking (CDC-ECM) - v0.3
#define USB_ECM_IP_ADDR        "10.48.0.1"
#define USB_ECM_NETMASK        "255.255.255.0"
#define USB_ECM_GATEWAY        "10.48.0.1"

// ============================================================================
// Buffer Configuration
// All sizes derived from base frame parameters
// ============================================================================

// Buffer depths in codec frames
#define PCM_BUFFER_FRAMES          4    // 4 × 20ms = 80ms PCM buffer
#define OPUS_BUFFER_FRAMES         8    // 8 × 20ms = 160ms compressed (cheap in RAM)

// Derived: buffer sizes in bytes
#define PCM_BUFFER_SIZE            (AUDIO_FRAME_BYTES_MONO * PCM_BUFFER_FRAMES)  // 7680 (4×1920)

// Opus buffer includes 2-byte length prefix per frame
#define OPUS_BUFFER_ITEM_MAX       (2 + OPUS_MAX_FRAME_BYTES)  // 514
#define OPUS_BUFFER_SIZE           (OPUS_BUFFER_ITEM_MAX * OPUS_BUFFER_FRAMES)  // 4112

// Jitter buffer (in codec frames)
// With 20ms frames, need more frames for same latency headroom
#define JITTER_BUFFER_FRAMES       3    // 3 × 20ms = 60ms max depth
#define JITTER_PREFILL_FRAMES      2    // 2 × 20ms = 40ms startup latency

// Derived: jitter thresholds in bytes
#define JITTER_BUFFER_BYTES        (AUDIO_FRAME_BYTES_MONO * JITTER_BUFFER_FRAMES)
#define JITTER_PREFILL_BYTES       (AUDIO_FRAME_BYTES_MONO * JITTER_PREFILL_FRAMES)

// Legacy alias
#define RING_BUFFER_SIZE           PCM_BUFFER_SIZE

// ============================================================================
// Task Stack Configuration (CENTRALIZED)
// 
// FreeRTOS xTaskCreate stack size is in WORDS (4 bytes on ESP32)
// Define in BYTES for clarity, then convert with macro
// ============================================================================

#define STACK_BYTES_TO_WORDS(bytes)  ((bytes) / sizeof(StackType_t))

// TX pipeline tasks
#define CAPTURE_TASK_STACK_BYTES     (6 * 1024)   // Uses static buffers
#define ENCODE_TASK_STACK_BYTES      (32 * 1024)  // Opus encoder: 20ms @ 48kHz with headroom for analysis buffers

// RX pipeline tasks
#define DECODE_TASK_STACK_BYTES      (16 * 1024)  // Opus decoder needs more stack (was 6KB, crashed)
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
// Configuration Sanity Checks (compile-time)
// ============================================================================

// Codec frame must be a multiple of DMA chunk for clean accumulation
_Static_assert((AUDIO_FRAME_MS % I2S_DMA_CHUNK_MS) == 0,
               "AUDIO_FRAME_MS must be a multiple of I2S_DMA_CHUNK_MS");

// Jitter prefill can't exceed buffer depth
_Static_assert(JITTER_PREFILL_FRAMES <= JITTER_BUFFER_FRAMES,
               "JITTER_PREFILL_FRAMES must be <= JITTER_BUFFER_FRAMES");

// DMA buffer size per descriptor must fit in hardware limit (4092 bytes)
// For stereo 16-bit: 4 bytes per frame, so max ~1023 frames
_Static_assert((I2S_DMA_CHUNK_SAMPLES * AUDIO_CHANNELS_STEREO * AUDIO_BYTES_PER_SAMPLE) <= 4092,
               "I2S DMA buffer size exceeds hardware limit");
