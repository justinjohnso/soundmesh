#pragma once

#include "audio_element.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple frame header for mesh audio
typedef struct __attribute__((packed)) {
    uint32_t magic;         // 'MSH1'
    uint8_t version;        // 1
    uint8_t codec_id;       // 1=OPUS
    uint8_t channels;       // 1 or 2
    uint8_t reserved;       // alignment
    uint32_t sample_rate;   // Hz
    uint32_t timestamp_ms;  // sender ms clock
    uint32_t seq;           // incrementing sequence
    uint16_t payload_len;   // bytes
    uint16_t hdr_crc;       // simple CRC16 of header
} mesh_audio_hdr_t;

typedef struct {
    int is_writer;              // 1: writer (TX) element, 0: reader (RX)
    int jitter_ms;              // target buffer on RX
    int group_broadcast;        // 1: broadcast to group via root
    int rx_queue_len;           // number of packets queued
} mesh_stream_cfg_t;

// Create mesh stream element (writer or reader depending on cfg.is_writer)
audio_element_handle_t mesh_stream_init(const mesh_stream_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
