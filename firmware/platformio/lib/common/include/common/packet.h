#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Packet magic number for validation
#define AUDIO_PACKET_MAGIC 0xA10D

// Packet version
#define AUDIO_PACKET_VERSION 1

// Payload types
typedef enum {
    PAYLOAD_TYPE_PCM16_MONO = 0,
    PAYLOAD_TYPE_PCM16_STEREO = 1
} payload_type_t;

// Audio packet header (packed)
typedef struct __attribute__((packed)) {
    uint16_t magic;              // 0xA10D
    uint8_t version;             // Packet format version
    uint8_t payload_type;        // payload_type_t
    uint32_t sequence;           // Sequence number
    uint16_t sample_rate;        // Sample rate in Hz
    uint8_t channels;            // Number of channels
    uint16_t frame_samples;      // Number of samples in this packet
    uint32_t timestamp_samples;  // Running timestamp in samples
    uint16_t payload_size;       // Size of payload in bytes
} audio_packet_header_t;

// Full packet structure
typedef struct {
    audio_packet_header_t header;
    int16_t payload[]; // Variable-length PCM16 data
} audio_packet_t;

// Packet API
esp_err_t packet_encode(audio_packet_t *pkt, const int16_t *pcm_data, 
                       uint16_t num_samples, uint32_t sequence, 
                       uint32_t timestamp_samples);

esp_err_t packet_decode(const audio_packet_t *pkt, int16_t *pcm_data, 
                       uint16_t *num_samples);

bool packet_validate(const audio_packet_t *pkt);

size_t packet_total_size(uint16_t num_samples);
