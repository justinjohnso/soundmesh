#pragma once

// Audio configuration - 48 kHz, 24-bit, mono with 5ms frames
#define AUDIO_SAMPLE_RATE      48000
#define AUDIO_BITS_PER_SAMPLE  24
#define AUDIO_CHANNELS         1     // Mono (v0.1)
#define AUDIO_FRAME_MS         5     // 5ms frames for low-latency mesh
#define AUDIO_FRAME_SAMPLES    (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 240 samples
#define AUDIO_BYTES_PER_SAMPLE 3     // 24-bit = 3 bytes (packed format)
#define AUDIO_FRAME_BYTES      (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS)  // 720 bytes

// Opus compression (future - v0.2)
#define OPUS_MAX_FRAME_BYTES   256  // Maximum Opus frame size
#define NETWORK_FRAME_BYTES    OPUS_MAX_FRAME_BYTES  // Network packet size (future)

// Network configuration - 10.48.0.x scheme (48kHz reference)
#define MESH_ID                "MeshNet-Audio-48"
#define MESH_PASSWORD          "meshnet123"
#define MESH_CHANNEL           6
#define UDP_PORT               3333  // Legacy (for star topology fallback if needed)
#define MAX_PACKET_SIZE        (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES)

// USB networking (CDC-ECM) - v0.3
#define USB_ECM_IP_ADDR        "10.48.0.1"
#define USB_ECM_NETMASK        "255.255.255.0"
#define USB_ECM_GATEWAY        "10.48.0.1"

// Buffer configuration
#define RING_BUFFER_SIZE       (AUDIO_FRAME_BYTES * 10)  // 10 frames = 50ms @ 5ms/frame
#define JITTER_BUFFER_FRAMES   10   // 10 frames = 50ms
#define JITTER_PREFILL_FRAMES  4    // Prefill 4 frames = 20ms startup latency

// Control layer configuration
#define CONTROL_TELEMETRY_RATE_MS    1000   // 1 Hz
#define CONTROL_HEARTBEAT_RATE_MS    2000   // 0.5 Hz
#define CONTROL_STATE_CACHE_TTL_MS   120000 // 2 minutes
#define CONTROL_STATE_CACHE_MAX_NODES 32
