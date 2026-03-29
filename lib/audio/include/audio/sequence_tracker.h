#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool first_packet;
    bool late_or_duplicate;
    bool hard_reset;
    bool request_fec;
    uint16_t last_seq;
    uint32_t dropped_frames;
    uint8_t plc_frames_to_inject;
} sequence_tracker_result_t;

sequence_tracker_result_t sequence_tracker_update(bool first_packet,
                                                  uint16_t last_seq,
                                                  uint16_t incoming_seq,
                                                  uint8_t max_plc_frames_per_gap,
                                                  uint8_t max_stale_frames_to_drop);

#ifdef __cplusplus
}
#endif
