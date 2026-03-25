#include <string.h>

#include <unity.h>

#include "network/mixer_control.h"

#include "../../../lib/network/src/mixer_control.c"

void test_mixer_encode_rejects_null_args(void)
{
    mixer_ctrl_message_t msg = {0};
    mixer_ctrl_packet_t packet = {0};
    TEST_ASSERT_FALSE(mixer_ctrl_encode(NULL, &packet));
    TEST_ASSERT_FALSE(mixer_ctrl_encode(&msg, NULL));
}

void test_mixer_encode_happy_path_sets_wire_fields(void)
{
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SET,
        .out_gain_pct = 250,
    };
    mixer_ctrl_packet_t packet = {0};
    TEST_ASSERT_TRUE(mixer_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(NET_PKT_TYPE_CONTROL, packet.type);
    TEST_ASSERT_EQUAL_UINT8(MIXER_CTRL_VERSION, packet.version);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MIXER_CTRL_SET, packet.subtype);
    TEST_ASSERT_EQUAL_UINT16(250, ntohs(packet.out_gain_pct));
}

void test_mixer_encode_rejects_out_of_range(void)
{
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SET,
        .out_gain_pct = MIXER_OUT_GAIN_MAX_PCT + 1,
    };
    mixer_ctrl_packet_t packet = {0};
    TEST_ASSERT_FALSE(mixer_ctrl_encode(&msg, &packet));
}

void test_mixer_decode_rejects_invalid_packets(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    TEST_ASSERT_FALSE(mixer_ctrl_decode(NULL, sizeof(packet), &msg));
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), NULL));
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet) - 1, &msg));

    packet.type = NET_PKT_TYPE_AUDIO_OPUS;
    packet.version = MIXER_CTRL_VERSION;
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION + 1;
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = 0;
    packet.out_gain_pct = htons(200);
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.subtype = (uint8_t)MIXER_CTRL_REQUEST_SYNC;
    TEST_ASSERT_TRUE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
}

void test_mixer_decode_happy_path(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = (uint8_t)MIXER_CTRL_SYNC;
    packet.out_gain_pct = htons(180);

    TEST_ASSERT_TRUE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MIXER_CTRL_SYNC, (uint8_t)msg.subtype);
    TEST_ASSERT_EQUAL_UINT16(180, msg.out_gain_pct);
}

void test_mixer_decode_rejects_out_of_range(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = (uint8_t)MIXER_CTRL_SET;
    packet.out_gain_pct = htons(MIXER_OUT_GAIN_MAX_PCT + 1);

    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mixer_encode_rejects_null_args);
    RUN_TEST(test_mixer_encode_happy_path_sets_wire_fields);
    RUN_TEST(test_mixer_encode_rejects_out_of_range);
    RUN_TEST(test_mixer_decode_rejects_invalid_packets);
    RUN_TEST(test_mixer_decode_happy_path);
    RUN_TEST(test_mixer_decode_rejects_out_of_range);
    return UNITY_END();
}
