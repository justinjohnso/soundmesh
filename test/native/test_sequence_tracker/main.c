#include <unity.h>

#include "audio/sequence_tracker.h"
#include "../../../lib/audio/src/sequence_tracker.c"

void test_first_packet_sets_baseline_without_drops(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(true, 0, 100, 3);
    TEST_ASSERT_FALSE(result.first_packet);
    TEST_ASSERT_EQUAL_UINT16(100, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_in_order_packet_reports_no_gap(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 101, 3);
    TEST_ASSERT_EQUAL_UINT16(101, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
}

void test_single_gap_requests_fec(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 102, 3);
    TEST_ASSERT_EQUAL_UINT16(102, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(1, result.dropped_frames);
    TEST_ASSERT_TRUE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_multi_gap_requests_plc_capped(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 106, 3);
    TEST_ASSERT_EQUAL_UINT16(106, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(5, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(3, result.plc_frames_to_inject);
}

void test_large_gap_is_ignored_as_nonrecoverable(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 250, 3);
    TEST_ASSERT_EQUAL_UINT16(250, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
    TEST_ASSERT_EQUAL_UINT8(0, result.plc_frames_to_inject);
}

void test_out_of_order_packet_does_not_increment_drop_counter(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 100, 99, 3);
    TEST_ASSERT_EQUAL_UINT16(99, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
    TEST_ASSERT_FALSE(result.request_fec);
}

void test_wraparound_sequence_is_handled(void)
{
    sequence_tracker_result_t result = sequence_tracker_update(false, 0xFFFF, 0x0000, 3);
    TEST_ASSERT_EQUAL_UINT16(0x0000, result.last_seq);
    TEST_ASSERT_EQUAL_UINT32(0, result.dropped_frames);
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
    RUN_TEST(test_wraparound_sequence_is_handled);
    return UNITY_END();
}
