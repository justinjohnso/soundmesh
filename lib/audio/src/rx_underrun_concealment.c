#include "audio/rx_underrun_concealment.h"

#include "config/build.h"

void rx_underrun_reset(rx_underrun_state_t *state)
{
    if (!state) {
        return;
    }
    state->consecutive_misses = 0;
}

rx_underrun_action_t rx_underrun_on_miss(rx_underrun_state_t *state)
{
    rx_underrun_action_t action = {
        .consecutive_misses = 1,
        .gain_q15 = RX_UNDERRUN_GAIN_Q15_ONE,
        .force_rebuffer = false
    };

    if (!state) {
        return action;
    }

    if (state->consecutive_misses < UINT32_MAX) {
        state->consecutive_misses++;
    }

    action.consecutive_misses = state->consecutive_misses;

    if (state->consecutive_misses <= RX_UNDERRUN_CONCEAL_FRAMES) {
        action.gain_q15 = RX_UNDERRUN_GAIN_Q15_ONE;
        return action;
    }

    uint32_t fade_index = state->consecutive_misses - RX_UNDERRUN_CONCEAL_FRAMES;
    if (fade_index >= RX_UNDERRUN_FADE_FRAMES) {
        action.gain_q15 = 0;
    } else {
        uint32_t remaining = RX_UNDERRUN_FADE_FRAMES - fade_index;
        action.gain_q15 = (uint16_t)((remaining * RX_UNDERRUN_GAIN_Q15_ONE) / RX_UNDERRUN_FADE_FRAMES);
    }

    if (state->consecutive_misses >= RX_UNDERRUN_REBUFFER_MISSES) {
        action.force_rebuffer = true;
    }

    return action;
}
