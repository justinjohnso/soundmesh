#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "common/packet.h"

// Packetizer: PCM frames → audio packets
typedef struct packetizer_s *packetizer_handle_t;

typedef struct {
    uint16_t samples_per_packet;
} packetizer_config_t;

esp_err_t packetizer_init(const packetizer_config_t *config, packetizer_handle_t *out_handle);
esp_err_t packetizer_process(packetizer_handle_t handle, const int16_t *pcm_data,
                            uint16_t num_samples, audio_packet_t *out_packet);
void packetizer_deinit(packetizer_handle_t handle);

// Depacketizer: audio packets → PCM frames
typedef struct depacketizer_s *depacketizer_handle_t;

esp_err_t depacketizer_init(depacketizer_handle_t *out_handle);
esp_err_t depacketizer_process(depacketizer_handle_t handle, const audio_packet_t *packet,
                              int16_t *out_pcm, uint16_t *out_num_samples);
void depacketizer_deinit(depacketizer_handle_t handle);

// Jitter buffer
typedef struct jitter_buffer_s *jitter_buffer_handle_t;

typedef struct {
    uint16_t buffer_packets;
    uint16_t target_latency_ms;
} jitter_buffer_config_t;

esp_err_t jitter_buffer_init(const jitter_buffer_config_t *config, jitter_buffer_handle_t *out_handle);
esp_err_t jitter_buffer_push(jitter_buffer_handle_t handle, const int16_t *pcm, uint16_t num_samples);
esp_err_t jitter_buffer_pop(jitter_buffer_handle_t handle, int16_t *out_pcm, uint16_t num_samples);
uint32_t jitter_buffer_get_fill_level(jitter_buffer_handle_t handle);
void jitter_buffer_deinit(jitter_buffer_handle_t handle);
