#include "network/frame_codec.h"

bool network_frame_resolve_header_size(size_t packet_size,
                                       uint16_t payload_len,
                                       size_t current_header_size,
                                       size_t legacy_header_size,
                                       size_t *header_size_out)
{
    if (!header_size_out) {
        return false;
    }

    if (packet_size == current_header_size + payload_len) {
        *header_size_out = current_header_size;
        return true;
    }
    if (packet_size == legacy_header_size + payload_len) {
        *header_size_out = legacy_header_size;
        return true;
    }

    return false;
}

uint8_t network_frame_extract_frame_count(const uint8_t *packet,
                                          size_t packet_size,
                                          size_t current_header_size,
                                          size_t header_size,
                                          uint8_t frame_count_current,
                                          size_t legacy_frame_count_offset)
{
    if (header_size == current_header_size) {
        return frame_count_current;
    }

    if (header_size <= legacy_frame_count_offset || packet_size <= legacy_frame_count_offset) {
        return 1;
    }

    return packet[legacy_frame_count_offset];
}

size_t network_frame_unpack_batch(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t frame_count,
                                  uint16_t base_seq,
                                  network_frame_iter_callback_t callback,
                                  void *ctx)
{
    if (!payload || !callback) {
        return 0;
    }

    uint8_t effective_frame_count = frame_count > 0 ? frame_count : 1;
    if (effective_frame_count <= 1) {
        callback(payload, (uint16_t)payload_len, base_seq, ctx);
        return payload_len;
    }

    size_t offset = 0;
    for (uint8_t idx = 0; idx < effective_frame_count && offset + 2 <= payload_len; idx++) {
        uint16_t frame_len = (uint16_t)(((uint16_t)payload[offset] << 8) | payload[offset + 1]);
        offset += 2;

        if (frame_len == 0 || offset + frame_len > payload_len) {
            continue;
        }

        callback(&payload[offset], frame_len, (uint16_t)(base_seq + idx), ctx);
        offset += frame_len;
    }

    return offset;
}
