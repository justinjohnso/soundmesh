#include "audio/sequence_tracker.h"

sequence_tracker_result_t sequence_tracker_update(bool first_packet,
                                                  uint16_t last_seq,
                                                  uint16_t incoming_seq,
                                                  uint8_t max_plc_frames_per_gap)
{
    sequence_tracker_result_t result = {
        .first_packet = false,
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

    int gap = (int16_t)(incoming_seq - expected);
    if (gap <= 0 || gap >= 100) {
        return result;
    }

    result.dropped_frames = (uint32_t)gap;
    if (gap == 1) {
        result.request_fec = true;
        return result;
    }

    uint8_t plc = (gap > max_plc_frames_per_gap) ? max_plc_frames_per_gap : (uint8_t)gap;
    result.plc_frames_to_inject = plc;
    return result;
}

