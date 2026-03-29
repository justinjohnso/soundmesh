#include "audio/sequence_tracker.h"

sequence_tracker_result_t sequence_tracker_update(bool first_packet,
                                                  uint16_t last_seq,
                                                  uint16_t incoming_seq,
                                                  uint8_t max_plc_frames_per_gap,
                                                  uint8_t max_stale_frames_to_drop)
{
    sequence_tracker_result_t result = {
        .first_packet = false,
        .late_or_duplicate = false,
        .hard_reset = false,
        .request_fec = false,
        .last_seq = incoming_seq,
        .dropped_frames = 0,
        .plc_frames_to_inject = 0
    };

    if (first_packet) {
        return result;
    }

    uint16_t expected = (uint16_t)(last_seq + 1U);
    if (incoming_seq == expected) {
        return result;
    }

    // Ignore bounded stale/duplicate packets without moving baseline backwards.
    // In multi-hop meshes, occasional short reordering is expected.
    int16_t delta_from_last = (int16_t)(incoming_seq - last_seq);
    if (delta_from_last <= 0) {
        uint16_t reverse_distance = (uint16_t)(last_seq - incoming_seq);
        if (reverse_distance <= max_stale_frames_to_drop) {
            result.late_or_duplicate = true;
            result.last_seq = last_seq;
        } else {
            // Treat very old sequence numbers as stream discontinuity / reset.
            result.hard_reset = true;
        }
        return result;
    }

    int gap = (int16_t)(incoming_seq - expected);
    if (gap >= 100) {
        return result;
    }

    result.dropped_frames = (uint32_t)gap;
    result.request_fec = true;

    if (gap > 1) {
        uint8_t plc_needed = (uint8_t)(gap - 1);
        uint8_t plc = (plc_needed > max_plc_frames_per_gap) ? max_plc_frames_per_gap : plc_needed;
        result.plc_frames_to_inject = plc;
    }
    return result;
}
