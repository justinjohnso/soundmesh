#include <string.h>

#include <unity.h>

#include "network/uplink_control.h"

#include "../../../lib/network/src/uplink_control.c"

static void fill_chars(char *dest, size_t count, char ch)
{
    for (size_t i = 0; i < count; i++) {
        dest[i] = ch;
    }
}

void test_encode_rejects_null_args(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet = {0};

    TEST_ASSERT_FALSE(uplink_ctrl_encode(NULL, &packet));
    TEST_ASSERT_FALSE(uplink_ctrl_encode(&msg, NULL));
}

void test_encode_happy_path_sets_wire_fields(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet;

    memset(&packet, 0xA5, sizeof(packet));
    msg.subtype = UPLINK_CTRL_SYNC;
    msg.enabled = true;
    strcpy(msg.ssid, "meshnet");
    strcpy(msg.password, "secret123");

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(0x10, packet.type);
    TEST_ASSERT_EQUAL_UINT8(UPLINK_CTRL_VERSION, packet.version);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UPLINK_CTRL_SYNC, packet.subtype);
    TEST_ASSERT_EQUAL_UINT8(UPLINK_CTRL_FLAG_ENABLED, packet.flags);
    TEST_ASSERT_EQUAL_UINT8(7, packet.ssid_len);
    TEST_ASSERT_EQUAL_UINT8(9, packet.password_len);
    TEST_ASSERT_EQUAL_STRING("meshnet", packet.ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", packet.password);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.ssid[packet.ssid_len]);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.password[packet.password_len]);
}

void test_encode_disabled_maps_to_zero_flags(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet = {0};

    msg.subtype = UPLINK_CTRL_CLEAR;
    msg.enabled = false;
    strcpy(msg.ssid, "ssid");
    strcpy(msg.password, "pw");

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UPLINK_CTRL_CLEAR, packet.subtype);
    TEST_ASSERT_EQUAL_UINT8(0, packet.flags);
}

void test_encode_supports_empty_credentials(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet = {0};

    msg.subtype = UPLINK_CTRL_REQUEST_SYNC;
    msg.enabled = false;

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(0, packet.ssid_len);
    TEST_ASSERT_EQUAL_UINT8(0, packet.password_len);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.ssid[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.password[0]);
}

void test_encode_copies_max_lengths_and_terminates(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet = {0};

    fill_chars(msg.ssid, UPLINK_SSID_MAX_LEN, 'S');
    fill_chars(msg.password, UPLINK_PASSWORD_MAX_LEN, 'P');
    msg.ssid[UPLINK_SSID_MAX_LEN] = '\0';
    msg.password[UPLINK_PASSWORD_MAX_LEN] = '\0';

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(UPLINK_SSID_MAX_LEN, packet.ssid_len);
    TEST_ASSERT_EQUAL_UINT8(UPLINK_PASSWORD_MAX_LEN, packet.password_len);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.ssid[UPLINK_SSID_MAX_LEN]);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.password[UPLINK_PASSWORD_MAX_LEN]);
}

void test_encode_non_terminated_input_is_capped(void)
{
    uplink_ctrl_message_t msg = {0};
    uplink_ctrl_packet_t packet = {0};

    fill_chars(msg.ssid, sizeof(msg.ssid), 'X');
    fill_chars(msg.password, sizeof(msg.password), 'Y');

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&msg, &packet));
    TEST_ASSERT_EQUAL_UINT8(UPLINK_SSID_MAX_LEN, packet.ssid_len);
    TEST_ASSERT_EQUAL_UINT8(UPLINK_PASSWORD_MAX_LEN, packet.password_len);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.ssid[UPLINK_SSID_MAX_LEN]);
    TEST_ASSERT_EQUAL_CHAR('\0', packet.password[UPLINK_PASSWORD_MAX_LEN]);
}

void test_decode_rejects_null_args_and_short_packets(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg = {0};

    TEST_ASSERT_FALSE(uplink_ctrl_decode(NULL, sizeof(packet), &msg));
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet), NULL));
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet) - 1, &msg));
}

void test_decode_rejects_wrong_type_and_version(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg = {0};

    packet.type = 0x11;
    packet.version = UPLINK_CTRL_VERSION;
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.type = 0x10;
    packet.version = UPLINK_CTRL_VERSION + 1;
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));
}

void test_decode_rejects_invalid_length_bounds(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg = {0};

    packet.type = 0x10;
    packet.version = UPLINK_CTRL_VERSION;

    packet.ssid_len = UPLINK_SSID_MAX_LEN + 1;
    packet.password_len = 0;
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));

    packet.ssid_len = 0;
    packet.password_len = UPLINK_PASSWORD_MAX_LEN + 1;
    TEST_ASSERT_FALSE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));
}

void test_decode_happy_path_copies_fields_and_maps_flag(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg;

    memset(&msg, 0xA5, sizeof(msg));

    packet.type = 0x10;
    packet.version = UPLINK_CTRL_VERSION;
    packet.subtype = (uint8_t)UPLINK_CTRL_REQUEST_SYNC;
    packet.flags = UPLINK_CTRL_FLAG_ENABLED | 0x80;
    packet.ssid_len = 4;
    packet.password_len = 8;
    memcpy(packet.ssid, "mesh", 4);
    memcpy(packet.password, "password", 8);

    TEST_ASSERT_TRUE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)UPLINK_CTRL_REQUEST_SYNC, (uint8_t)msg.subtype);
    TEST_ASSERT_TRUE(msg.enabled);
    TEST_ASSERT_EQUAL_STRING("mesh", msg.ssid);
    TEST_ASSERT_EQUAL_STRING("password", msg.password);
}

void test_decode_ignores_bytes_beyond_declared_lengths(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg = {0};

    packet.type = 0x10;
    packet.version = UPLINK_CTRL_VERSION;
    packet.subtype = (uint8_t)UPLINK_CTRL_SET;
    packet.flags = UPLINK_CTRL_FLAG_ENABLED;
    packet.ssid_len = 2;
    packet.password_len = 3;
    memcpy(packet.ssid, "abZ", 3);
    memcpy(packet.password, "xyzq", 4);

    TEST_ASSERT_TRUE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_EQUAL_STRING("ab", msg.ssid);
    TEST_ASSERT_EQUAL_STRING("xyz", msg.password);
}

void test_decode_enabled_flag_unset_maps_to_false(void)
{
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t msg = {0};

    packet.type = 0x10;
    packet.version = UPLINK_CTRL_VERSION;
    packet.subtype = 0xFE;
    packet.flags = 0x80;
    packet.ssid_len = 0;
    packet.password_len = 0;

    TEST_ASSERT_TRUE(uplink_ctrl_decode(&packet, sizeof(packet), &msg));
    TEST_ASSERT_FALSE(msg.enabled);
    TEST_ASSERT_EQUAL_UINT8(0xFE, (uint8_t)msg.subtype);
}

void test_encode_then_decode_round_trip_preserves_values(void)
{
    uplink_ctrl_message_t input = {0};
    uplink_ctrl_packet_t packet = {0};
    uplink_ctrl_message_t output = {0};

    input.subtype = UPLINK_CTRL_SET;
    input.enabled = true;
    strcpy(input.ssid, "MeshNet-A");
    strcpy(input.password, "P4ssword!");

    TEST_ASSERT_TRUE(uplink_ctrl_encode(&input, &packet));
    TEST_ASSERT_TRUE(uplink_ctrl_decode(&packet, sizeof(packet), &output));

    TEST_ASSERT_EQUAL_UINT8((uint8_t)input.subtype, (uint8_t)output.subtype);
    TEST_ASSERT_EQUAL(input.enabled, output.enabled);
    TEST_ASSERT_EQUAL_STRING(input.ssid, output.ssid);
    TEST_ASSERT_EQUAL_STRING(input.password, output.password);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_rejects_null_args);
    RUN_TEST(test_encode_happy_path_sets_wire_fields);
    RUN_TEST(test_encode_disabled_maps_to_zero_flags);
    RUN_TEST(test_encode_supports_empty_credentials);
    RUN_TEST(test_encode_copies_max_lengths_and_terminates);
    RUN_TEST(test_encode_non_terminated_input_is_capped);
    RUN_TEST(test_decode_rejects_null_args_and_short_packets);
    RUN_TEST(test_decode_rejects_wrong_type_and_version);
    RUN_TEST(test_decode_rejects_invalid_length_bounds);
    RUN_TEST(test_decode_happy_path_copies_fields_and_maps_flag);
    RUN_TEST(test_decode_ignores_bytes_beyond_declared_lengths);
    RUN_TEST(test_decode_enabled_flag_unset_maps_to_false);
    RUN_TEST(test_encode_then_decode_round_trip_preserves_values);
    return UNITY_END();
}
