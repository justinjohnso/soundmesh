#include <unity.h>

#include "network/frame_codec.h"

#define CURRENT_HEADER_SIZE 26
#define LEGACY_HEADER_SIZE 14

#include "../../../lib/network/src/frame_codec.c"

typedef struct {
    int calls;
    uint16_t seqs[8];
    uint16_t lens[8];
} frame_capture_t;

static void capture_frame(const uint8_t *frame, uint16_t len, uint16_t seq, void *ctx)
{
    (void)frame;
    frame_capture_t *capture = (frame_capture_t *)ctx;
    capture->seqs[capture->calls] = seq;
    capture->lens[capture->calls] = len;
    capture->calls++;
}

void test_resolve_header_size_accepts_current(void)
{
    size_t hdr_size = 0;
    bool ok = network_frame_resolve_header_size(CURRENT_HEADER_SIZE + 42,
                                                42,
                                                CURRENT_HEADER_SIZE,
                                                LEGACY_HEADER_SIZE,
                                                &hdr_size);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(CURRENT_HEADER_SIZE, hdr_size);
}

void test_resolve_header_size_accepts_legacy(void)
{
    size_t hdr_size = 0;
    bool ok = network_frame_resolve_header_size(LEGACY_HEADER_SIZE + 10,
                                                10,
                                                CURRENT_HEADER_SIZE,
                                                LEGACY_HEADER_SIZE,
                                                &hdr_size);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(LEGACY_HEADER_SIZE, hdr_size);
}

void test_resolve_header_size_rejects_mismatch(void)
{
    size_t hdr_size = 0;
    bool ok = network_frame_resolve_header_size(77,
                                                10,
                                                CURRENT_HEADER_SIZE,
                                                LEGACY_HEADER_SIZE,
                                                &hdr_size);
    TEST_ASSERT_FALSE(ok);
}

void test_extract_frame_count_uses_current_header_field(void)
{
    uint8_t packet[CURRENT_HEADER_SIZE] = {0};
    uint8_t frame_count = network_frame_extract_frame_count(packet,
                                                            sizeof(packet),
                                                            CURRENT_HEADER_SIZE,
                                                            CURRENT_HEADER_SIZE,
                                                            3,
                                                            13);
    TEST_ASSERT_EQUAL_UINT8(3, frame_count);
}

void test_extract_frame_count_uses_legacy_offset(void)
{
    uint8_t packet[LEGACY_HEADER_SIZE + 5] = {0};
    packet[13] = 2;
    uint8_t frame_count = network_frame_extract_frame_count(packet,
                                                            sizeof(packet),
                                                            CURRENT_HEADER_SIZE,
                                                            LEGACY_HEADER_SIZE,
                                                            0,
                                                            13);
    TEST_ASSERT_EQUAL_UINT8(2, frame_count);
}

void test_unpack_batch_yields_all_valid_frames(void)
{
    uint8_t payload[] = {
        0x00, 0x03, 0xAA, 0xBB, 0xCC,
        0x00, 0x02, 0x11, 0x22
    };
    frame_capture_t capture = {0};
    size_t consumed = network_frame_unpack_batch(payload, sizeof(payload), 2, 100, capture_frame, &capture);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), consumed);
    TEST_ASSERT_EQUAL_INT(2, capture.calls);
    TEST_ASSERT_EQUAL_UINT16(100, capture.seqs[0]);
    TEST_ASSERT_EQUAL_UINT16(101, capture.seqs[1]);
    TEST_ASSERT_EQUAL_UINT16(3, capture.lens[0]);
    TEST_ASSERT_EQUAL_UINT16(2, capture.lens[1]);
}

void test_unpack_batch_skips_invalid_truncated_frame(void)
{
    uint8_t payload[] = {
        0x00, 0x02, 0xAA, 0xBB,
        0x00, 0x04, 0x11
    };
    frame_capture_t capture = {0};
    size_t consumed = network_frame_unpack_batch(payload, sizeof(payload), 2, 50, capture_frame, &capture);
    TEST_ASSERT_EQUAL_UINT32(6, consumed);
    TEST_ASSERT_EQUAL_INT(1, capture.calls);
    TEST_ASSERT_EQUAL_UINT16(50, capture.seqs[0]);
    TEST_ASSERT_EQUAL_UINT16(2, capture.lens[0]);
}

void test_unpack_batch_single_frame_passthrough(void)
{
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    frame_capture_t capture = {0};
    size_t consumed = network_frame_unpack_batch(payload, sizeof(payload), 1, 9, capture_frame, &capture);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), consumed);
    TEST_ASSERT_EQUAL_INT(1, capture.calls);
    TEST_ASSERT_EQUAL_UINT16(9, capture.seqs[0]);
    TEST_ASSERT_EQUAL_UINT16(4, capture.lens[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resolve_header_size_accepts_current);
    RUN_TEST(test_resolve_header_size_accepts_legacy);
    RUN_TEST(test_resolve_header_size_rejects_mismatch);
    RUN_TEST(test_extract_frame_count_uses_current_header_field);
    RUN_TEST(test_extract_frame_count_uses_legacy_offset);
    RUN_TEST(test_unpack_batch_yields_all_valid_frames);
    RUN_TEST(test_unpack_batch_skips_invalid_truncated_frame);
    RUN_TEST(test_unpack_batch_single_frame_passthrough);
    return UNITY_END();
}
