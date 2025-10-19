#pragma once

// Audio configuration
#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_CHANNELS         1
#define AUDIO_FRAME_MS         10
#define AUDIO_FRAME_SAMPLES    (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)
#define AUDIO_FRAME_BYTES      (AUDIO_FRAME_SAMPLES * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS)

// Network configuration
#define MESH_SSID              "MeshNet-Audio"
#define MESH_PASSWORD          "meshnet123"
#define UDP_PORT               3333
#define MAX_PACKET_SIZE        512

// Buffer configuration
#define RING_BUFFER_SIZE       (AUDIO_FRAME_BYTES * 10)
