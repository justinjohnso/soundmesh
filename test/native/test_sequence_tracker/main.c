#include <unity.h>

#define ADC_CHANNEL_3 3

#include "config/build.h"
#include "audio/sequence_tracker.h"
#include "../../../lib/audio/src/sequence_tracker.c"

#define TEST_MAX_STALE 24
#define TEST_PLC_CAP RX_PLC_MAX_FRAMES_PER_GAP

/*
 * Host-test boundary note:
 * adf_pipeline_feed_opus_impl and RX decode/playback behavior depend on FreeRTOS
 * task notifications, ring buffers, and Opus decoder side effects, which are not
 * deterministic in native host tests without heavy integration scaffolding.
 * These tests intentionally lock regression coverage to sequence_tracker_update(),
 * the extracted pure helper that defines loss/FEC/PLC sequence invariants.
 */

void test_first_packet_sets_baseline_without_drops(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(true, 0, 100, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_FALSE(result.first_packet);
    TEST_ASSERT_EQUAL_UINT16(100, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_in_order_packet_reports_no_gap(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 101, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(101, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
}

void test_single_gap_requests_fec(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 102, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(102, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(1, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_multi_gap_requests_plc_capped(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 106, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(106, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(5, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(TEST_PLC_CAP, result.plc_frames_to_inject);
}

void test_large_gap_is_ignored_as_nonrecoverable(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 250, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(250, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_out_of_order_packet_does_not_increment_drop_counter(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 99, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(100, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_TRUE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
}

void test_duplicate_packet_preserves_baseline(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 100, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(100, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_TRUE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_stale_packet_does_not_poison_next_in_order_step(void)
{
    sequence_tracker_result_t stale = sequence_tracker_update(false, 100, 99, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(100, stale.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, stale.dropped_frames);
    TEST_ASSERT_TRUE(stale.late_or_duplicate);
    TEST_ASSERT_FALSE(stale.hard_reset);

    sequence_tracker_result_t next = sequence_tracker_update(false, stale.last_seq, 101, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(101, next.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, next.dropped_frames);
    TEST_ASSERT_FALSE(next.late_or_duplicate);
    TEST_ASSERT_FALSE(next.hard_reset);
    TEST_ASSERT_FALSE(next.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, next.plc_frames_to_inject);
}

void test_wraparound_sequence_is_handled(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 0xFFFF, 0x0000, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(0x0000, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
}

void test_wraparound_single_missing_frame_requests_fec(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 0xFFFE, 0x0000, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(0x0000, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(1, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_wraparound_multi_missing_frames_injects_plc(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 0xFFFE, 0x0002, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(0x0002, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(3, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    uint8_t expected_plc = (2u > TEST_PLC_CAP) ? TEST_PLC_CAP : 2u;
    TEST_ASSERT_EQUAL_UINT8(expected_plc, result.plc_frames_to_inject);
}

void test_gap_of_99_is_recoverable_but_plc_is_capped(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 200, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(200, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(99, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(TEST_PLC_CAP, result.plc_frames_to_inject);
}

void test_gap_of_100_is_treated_as_nonrecoverable(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 201, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(201, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_plc_injection_respects_zero_cap(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 105, 0, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(105, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(4, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_first_packet_ignores_any_apparent_gap(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(true, 100, 60000, TEST_PLC_CAP, TEST_MAX_STALE);
    TEST_ASSERT_EQUAL_UINT16(60000, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_FALSE(result.hard_reset);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_far_stale_packet_triggers_hard_reset_instead_of_duplicate_drop(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 500, 430, TEST_PLC_CAP, 24);
    TEST_ASSERT_FALSE(result.late_or_duplicate);
    TEST_ASSERT_TRUE(result.hard_reset);
    TEST_ASSERT_EQUAL_UINT16(430, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_packet_sets_baseline_without_drops);
    RUN_TEST(test_in_order_packet_reports_no_gap);
    RUN_TEST(test_single_gap_requests_fec);
    RUN_TEST(test_multi_gap_requests_plc_capped);
    RUN_TEST(test_large_gap_is_ignored_as_nonrecoverable);
    RUN_TEST(test_out_of_order_packet_does_not_increment_drop_counter);
    RUN_TEST(test_duplicate_packet_preserves_baseline);
    RUN_TEST(test_stale_packet_does_not_poison_next_in_order_step);
    RUN_TEST(test_wraparound_sequence_is_handled);
    RUN_TEST(test_wraparound_single_missing_frame_requests_fec);
    RUN_TEST(test_wraparound_multi_missing_frames_injects_plc);
    RUN_TEST(test_gap_of_99_is_recoverable_but_plc_is_capped);
    RUN_TEST(test_gap_of_100_is_treated_as_nonrecoverable);
    RUN_TEST(test_plc_injection_respects_zero_cap);
    RUN_TEST(test_first_packet_ignores_any_apparent_gap);
    RUN_TEST(test_far_stale_packet_triggers_hard_reset_instead_of_duplicate_drop);
    return UNITY_END();
}
