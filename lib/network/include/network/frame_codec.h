#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*network_frame_iter_callback_t)(const uint8_t *frame,
                                              uint16_t frame_len,
                                              uint16_t seq,
                                              void *ctx);

bool network_frame_resolve_header_size(size_t packet_size,
                                       uint16_t payload_len,
                                       size_t current_header_size,
                                       size_t legacy_header_size,
                                       size_t *header_size_out);

uint8_t network_frame_extract_frame_count(const uint8_t *packet,
                                          size_t packet_size,
                                          size_t current_header_size,
                                          size_t header_size,
                                          uint8_t frame_count_current,
                                          size_t legacy_frame_count_offset);

size_t network_frame_unpack_batch(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t frame_count,
                                  uint16_t base_seq,
                                  network_frame_iter_callback_t callback,
                                  void *ctx);

#ifdef __cplusplus
}
#endif
