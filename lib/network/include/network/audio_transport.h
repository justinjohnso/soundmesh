#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build and send an audio packet from an Opus batch payload.
// Payload format: repeated [uint16_be frame_len][frame_bytes...]
// where frame_count specifies how many frames are encoded in the payload.
esp_err_t network_send_audio_batch(const uint8_t *opus_batch_payload,
                                   size_t payload_len,
                                   uint16_t seq,
                                   uint32_t timestamp_ms,
                                   uint8_t frame_count,
                                   uint8_t stream_id);

#ifdef __cplusplus
}
#endif
