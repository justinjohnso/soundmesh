#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RX_UNDERRUN_GAIN_Q15_ONE 32767U

typedef struct {
    uint32_t consecutive_misses;
} rx_underrun_state_t;

typedef struct {
    uint32_t consecutive_misses;
    uint16_t gain_q15;
    bool force_rebuffer;
} rx_underrun_action_t;

void rx_underrun_reset(rx_underrun_state_t *state);
rx_underrun_action_t rx_underrun_on_miss(rx_underrun_state_t *state);

#ifdef __cplusplus
}
#endif
