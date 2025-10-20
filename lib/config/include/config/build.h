#pragma once

// Audio configuration
#define AUDIO_SAMPLE_RATE      44100
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_CHANNELS         2  // Stereo for ADC input
#define AUDIO_FRAME_MS         5
#define AUDIO_FRAME_SAMPLES    (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)
#define AUDIO_FRAME_BYTES      (AUDIO_FRAME_SAMPLES * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS)

// Opus compression (64kbps for 10ms stereo frame ≈ 80 bytes)
#define OPUS_MAX_FRAME_BYTES   256  // Maximum Opus frame size
#define NETWORK_FRAME_BYTES    OPUS_MAX_FRAME_BYTES  // Network packet size

// Network configuration
#define MESH_SSID              "MeshNet-Audio"
#define MESH_PASSWORD          "meshnet123"
#define UDP_PORT               3333
#define MAX_PACKET_SIZE        (NET_FRAME_HEADER_SIZE + AUDIO_FRAME_BYTES)

// Buffer configuration
#define RING_BUFFER_SIZE       (AUDIO_FRAME_BYTES * 10)
