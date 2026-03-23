#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool first_packet;
    bool request_fec;
    uint16_t last_seq;
    uint32_t dropped_frames;
    uint8_t plc_frames_to_inject;
} sequence_tracker_result_t;

sequence_tracker_result_t sequence_tracker_update(bool first_packet,
                                                  uint16_t last_seq,
                                                  uint16_t incoming_seq,
                                                  uint8_t max_plc_frames_per_gap);

#ifdef __cplusplus
}
#endif

