#include <stdbool.h>
#include <stdint.h>

#include <unity.h>

#define ADC_CHANNEL_3 3

#include "config/build.h"
#include "../../../lib/audio/src/rx_underrun_concealment.c"

void test_initial_miss_returns_full_gain_without_rebuffer(void)
{
    rx_underrun_state_t state = {0};
    rx_underrun_action_t action = rx_underrun_on_miss(&state);

    TEST_ASSERT_EQUAL_UINT32(1, action.consecutive_misses);
    TEST_ASSERT_EQUAL_UINT16(RX_UNDERRUN_GAIN_Q15_ONE, action.gain_q15);
    TEST_ASSERT_FALSE(action.force_rebuffer);
}

void test_concealment_window_holds_full_gain(void)
{
    rx_underrun_state_t state = {0};
    rx_underrun_action_t action = {0};
    for (uint32_t i = 0; i < RX_UNDERRUN_CONCEAL_FRAMES; i++) {
        action = rx_underrun_on_miss(&state);
    }

    TEST_ASSERT_EQUAL_UINT32(RX_UNDERRUN_CONCEAL_FRAMES, action.consecutive_misses);
    TEST_ASSERT_EQUAL_UINT16(RX_UNDERRUN_GAIN_Q15_ONE, action.gain_q15);
    TEST_ASSERT_FALSE(action.force_rebuffer);
}

void test_fade_window_decreases_gain_monotonically(void)
{
    rx_underrun_state_t state = {0};
    uint16_t previous = RX_UNDERRUN_GAIN_Q15_ONE;

    for (uint32_t miss = 1; miss <= (RX_UNDERRUN_CONCEAL_FRAMES + RX_UNDERRUN_FADE_FRAMES); miss++) {
        rx_underrun_action_t action = rx_underrun_on_miss(&state);
        if (miss <= RX_UNDERRUN_CONCEAL_FRAMES) {
            TEST_ASSERT_EQUAL_UINT16(RX_UNDERRUN_GAIN_Q15_ONE, action.gain_q15);
            continue;
        }

        TEST_ASSERT_TRUE(action.gain_q15 <= previous);
        previous = action.gain_q15;
    }
}

void test_gain_reaches_silence_after_fade_budget(void)
{
    rx_underrun_state_t state = {0};
    rx_underrun_action_t action = {0};
    const uint32_t misses_to_silence = RX_UNDERRUN_CONCEAL_FRAMES + RX_UNDERRUN_FADE_FRAMES;

    for (uint32_t i = 0; i < misses_to_silence; i++) {
        action = rx_underrun_on_miss(&state);
    }

    TEST_ASSERT_EQUAL_UINT16(0, action.gain_q15);
}

void test_force_rebuffer_trips_at_configured_threshold(void)
{
    rx_underrun_state_t state = {0};
    rx_underrun_action_t action = {0};
    for (uint32_t i = 0; i < RX_UNDERRUN_REBUFFER_MISSES; i++) {
        action = rx_underrun_on_miss(&state);
    }

    TEST_ASSERT_TRUE(action.force_rebuffer);
    TEST_ASSERT_EQUAL_UINT16(0, action.gain_q15);
}

void test_reset_clears_miss_streak(void)
{
    rx_underrun_state_t state = {0};
    for (uint32_t i = 0; i < RX_UNDERRUN_REBUFFER_MISSES; i++) {
        (void)rx_underrun_on_miss(&state);
    }

    rx_underrun_reset(&state);
    rx_underrun_action_t action = rx_underrun_on_miss(&state);

    TEST_ASSERT_EQUAL_UINT32(1, action.consecutive_misses);
    TEST_ASSERT_EQUAL_UINT16(RX_UNDERRUN_GAIN_Q15_ONE, action.gain_q15);
    TEST_ASSERT_FALSE(action.force_rebuffer);
}

void test_null_state_is_safe_and_does_not_rebuffer(void)
{
    rx_underrun_action_t action = rx_underrun_on_miss(NULL);
    TEST_ASSERT_EQUAL_UINT32(1, action.consecutive_misses);
    TEST_ASSERT_EQUAL_UINT16(RX_UNDERRUN_GAIN_Q15_ONE, action.gain_q15);
    TEST_ASSERT_FALSE(action.force_rebuffer);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_initial_miss_returns_full_gain_without_rebuffer);
    RUN_TEST(test_concealment_window_holds_full_gain);
    RUN_TEST(test_fade_window_decreases_gain_monotonically);
    RUN_TEST(test_gain_reaches_silence_after_fade_budget);
    RUN_TEST(test_force_rebuffer_trips_at_configured_threshold);
    RUN_TEST(test_reset_clears_miss_streak);
    RUN_TEST(test_null_state_is_safe_and_does_not_rebuffer);
    return UNITY_END();
}
