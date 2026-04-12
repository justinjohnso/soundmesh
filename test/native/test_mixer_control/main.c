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
        .version = MIXER_CTRL_VERSION,
        .out_gain_pct = 250,
        .stream_count = 2,
        .streams = {
            {
                .stream_id = 1,
                .gain_pct = 125,
                .enabled = true,
                .muted = false,
                .solo = false,
                .active = true,
            },
            {
                .stream_id = 2,
                .gain_pct = 75,
                .enabled = true,
                .muted = true,
                .solo = true,
                .active = false,
            },
        },
    };
    mixer_ctrl_packet_t packet = {0};
    TEST_ASSERT_TRUE(mixer_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(NET_PKT_TYPE_CONTROL, packet.type);
    TEST_ASSERT_EQUAL_UINT8(MIXER_CTRL_VERSION, packet.version);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MIXER_CTRL_SET, packet.subtype);
    TEST_ASSERT_EQUAL_UINT8(2, packet.stream_count);
    TEST_ASSERT_EQUAL_UINT16(250, ntohs(packet.out_gain_pct));
    TEST_ASSERT_EQUAL_UINT8(1, packet.streams[0].stream_id);
    TEST_ASSERT_EQUAL_UINT16(125, ntohs(packet.streams[0].gain_pct));
    TEST_ASSERT_BITS_HIGH(
        MIXER_CTRL_STREAM_FLAG_ENABLED | MIXER_CTRL_STREAM_FLAG_ACTIVE, packet.streams[0].flags);
    TEST_ASSERT_BITS_LOW(
        MIXER_CTRL_STREAM_FLAG_MUTED | MIXER_CTRL_STREAM_FLAG_SOLO, packet.streams[0].flags);
    TEST_ASSERT_EQUAL_UINT8(2, packet.streams[1].stream_id);
    TEST_ASSERT_EQUAL_UINT16(75, ntohs(packet.streams[1].gain_pct));
    TEST_ASSERT_BITS_HIGH(
        MIXER_CTRL_STREAM_FLAG_ENABLED | MIXER_CTRL_STREAM_FLAG_MUTED | MIXER_CTRL_STREAM_FLAG_SOLO,
        packet.streams[1].flags);
    TEST_ASSERT_BITS_LOW(MIXER_CTRL_STREAM_FLAG_ACTIVE, packet.streams[1].flags);
}

void test_mixer_encode_defaults_zero_version_to_current(void)
{
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SYNC,
        .version = 0,
        .out_gain_pct = 100,
    };
    mixer_ctrl_packet_t packet = {0};
    TEST_ASSERT_TRUE(mixer_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(MIXER_CTRL_VERSION, packet.version);
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

void test_mixer_encode_rejects_invalid_stream_payload(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {
        .subtype = MIXER_CTRL_SET,
        .version = MIXER_CTRL_VERSION,
        .out_gain_pct = 100,
        .stream_count = MIXER_MAX_STREAMS + 1,
    };
    TEST_ASSERT_FALSE(mixer_ctrl_encode(&msg, &packet));

    msg.stream_count = 1;
    msg.streams[0].stream_id = 0;
    msg.streams[0].gain_pct = 100;
    TEST_ASSERT_FALSE(mixer_ctrl_encode(&msg, &packet));

    msg.streams[0].stream_id = 1;
    msg.streams[0].gain_pct = MIXER_STREAM_GAIN_MAX_PCT + 1;
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

    packet.subtype = (uint8_t)MIXER_CTRL_SET;
    packet.stream_count = MIXER_MAX_STREAMS + 1;
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
}

void test_mixer_decode_happy_path_with_streams(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = (uint8_t)MIXER_CTRL_SYNC;
    packet.out_gain_pct = htons(180);
    packet.stream_count = 2;
    packet.streams[0].stream_id = 1;
    packet.streams[0].flags = MIXER_CTRL_STREAM_FLAG_ENABLED | MIXER_CTRL_STREAM_FLAG_ACTIVE;
    packet.streams[0].gain_pct = htons(140);
    packet.streams[1].stream_id = 2;
    packet.streams[1].flags = MIXER_CTRL_STREAM_FLAG_ENABLED | MIXER_CTRL_STREAM_FLAG_MUTED;
    packet.streams[1].gain_pct = htons(60);

    TEST_ASSERT_TRUE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MIXER_CTRL_SYNC, (uint8_t)msg.subtype);
    TEST_ASSERT_EQUAL_UINT16(180, msg.out_gain_pct);
    TEST_ASSERT_EQUAL_UINT8(2, msg.stream_count);
    TEST_ASSERT_EQUAL_UINT8(1, msg.streams[0].stream_id);
    TEST_ASSERT_EQUAL_UINT16(140, msg.streams[0].gain_pct);
    TEST_ASSERT_TRUE(msg.streams[0].enabled);
    TEST_ASSERT_FALSE(msg.streams[0].muted);
    TEST_ASSERT_FALSE(msg.streams[0].solo);
    TEST_ASSERT_TRUE(msg.streams[0].active);
    TEST_ASSERT_EQUAL_UINT8(2, msg.streams[1].stream_id);
    TEST_ASSERT_EQUAL_UINT16(60, msg.streams[1].gain_pct);
    TEST_ASSERT_TRUE(msg.streams[1].enabled);
    TEST_ASSERT_TRUE(msg.streams[1].muted);
    TEST_ASSERT_FALSE(msg.streams[1].solo);
    TEST_ASSERT_FALSE(msg.streams[1].active);
}

void test_mixer_decode_happy_path_legacy_zero_streams(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = (uint8_t)MIXER_CTRL_SET;
    packet.out_gain_pct = htons(200);
    packet.stream_count = 0;

    TEST_ASSERT_TRUE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_EQUAL_UINT16(200, msg.out_gain_pct);
    TEST_ASSERT_EQUAL_UINT8(0, msg.stream_count);
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

void test_mixer_decode_rejects_invalid_stream_entries(void)
{
    mixer_ctrl_packet_t packet = {0};
    mixer_ctrl_message_t msg = {0};

    packet.type = NET_PKT_TYPE_CONTROL;
    packet.version = MIXER_CTRL_VERSION;
    packet.subtype = (uint8_t)MIXER_CTRL_SET;
    packet.out_gain_pct = htons(200);
    packet.stream_count = 1;
    packet.streams[0].stream_id = 0;
    packet.streams[0].gain_pct = htons(100);
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.streams[0].stream_id = 1;
    packet.streams[0].gain_pct = htons(MIXER_STREAM_GAIN_MAX_PCT + 1);
    TEST_ASSERT_FALSE(mixer_ctrl_decode(&packet, sizeof(packet), &msg));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mixer_encode_rejects_null_args);
    RUN_TEST(test_mixer_encode_happy_path_sets_wire_fields);
    RUN_TEST(test_mixer_encode_defaults_zero_version_to_current);
    RUN_TEST(test_mixer_encode_rejects_out_of_range);
    RUN_TEST(test_mixer_encode_rejects_invalid_stream_payload);
    RUN_TEST(test_mixer_decode_rejects_invalid_packets);
    RUN_TEST(test_mixer_decode_happy_path_with_streams);
    RUN_TEST(test_mixer_decode_happy_path_legacy_zero_streams);
    RUN_TEST(test_mixer_decode_rejects_out_of_range);
    RUN_TEST(test_mixer_decode_rejects_invalid_stream_entries);
    return UNITY_END();
}
