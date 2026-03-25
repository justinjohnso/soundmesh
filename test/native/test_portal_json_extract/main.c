#include <unity.h>

#include "control/json_extract.h"
#include "../../../lib/control/src/json_extract.c"

void test_string_extract_succeeds_with_mixed_order(void)
{
    const char *body = "{\"enabled\":true,\"password\":\"secret\",\"ssid\":\"MeshNet\"}";
    char ssid[16] = {0};
    bool ok = json_extract_string_field(body, "ssid", ssid, sizeof(ssid));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("MeshNet", ssid);
}

void test_string_extract_fails_on_missing_key(void)
{
    const char *body = "{\"enabled\":true}";
    char out[16] = {0};
    bool ok = json_extract_string_field(body, "ssid", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
}

void test_string_extract_fails_on_malformed_json_snippet(void)
{
    const char *body = "{\"ssid\":\"MeshNet}";
    char out[16] = {0};
    bool ok = json_extract_string_field(body, "ssid", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
}

void test_string_extract_allows_empty_string(void)
{
    const char *body = "{\"password\":\"\"}";
    char out[8] = {0};
    bool ok = json_extract_string_field(body, "password", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_string_extract_rejects_truncation_overflow(void)
{
    const char *body = "{\"url\":\"https://example.com/firmware.bin\"}";
    char out[8] = {0};
    bool ok = json_extract_string_field(body, "url", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
}

void test_bool_extract_accepts_whitespace_before_true(void)
{
    const char *body = "{\"enabled\": \t true}";
    bool value = false;
    bool ok = json_extract_bool_field(body, "enabled", &value);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(value);
}

void test_bool_extract_accepts_whitespace_before_false(void)
{
    const char *body = "{\"enabled\":\t false}";
    bool value = true;
    bool ok = json_extract_bool_field(body, "enabled", &value);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(value);
}

void test_bool_extract_fails_on_missing_key(void)
{
    const char *body = "{\"ssid\":\"MeshNet\"}";
    bool value = false;
    bool ok = json_extract_bool_field(body, "enabled", &value);
    TEST_ASSERT_FALSE(ok);
}

void test_bool_extract_fails_on_malformed_value(void)
{
    const char *body = "{\"enabled\":tru}";
    bool value = false;
    bool ok = json_extract_bool_field(body, "enabled", &value);
    TEST_ASSERT_FALSE(ok);
}

void test_uint16_extract_succeeds(void)
{
    const char *body = "{\"outGainPct\":250}";
    uint16_t value = 0;
    bool ok = json_extract_uint16_field(body, "outGainPct", &value);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT16(250, value);
}

void test_uint16_extract_rejects_overflow(void)
{
    const char *body = "{\"outGainPct\":70000}";
    uint16_t value = 0;
    bool ok = json_extract_uint16_field(body, "outGainPct", &value);
    TEST_ASSERT_FALSE(ok);
}

void test_uint16_extract_rejects_non_numeric(void)
{
    const char *body = "{\"outGainPct\":\"250\"}";
    uint16_t value = 0;
    bool ok = json_extract_uint16_field(body, "outGainPct", &value);
    TEST_ASSERT_FALSE(ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_string_extract_succeeds_with_mixed_order);
    RUN_TEST(test_string_extract_fails_on_missing_key);
    RUN_TEST(test_string_extract_fails_on_malformed_json_snippet);
    RUN_TEST(test_string_extract_allows_empty_string);
    RUN_TEST(test_string_extract_rejects_truncation_overflow);
    RUN_TEST(test_bool_extract_accepts_whitespace_before_true);
    RUN_TEST(test_bool_extract_accepts_whitespace_before_false);
    RUN_TEST(test_bool_extract_fails_on_missing_key);
    RUN_TEST(test_bool_extract_fails_on_malformed_value);
    RUN_TEST(test_uint16_extract_succeeds);
    RUN_TEST(test_uint16_extract_rejects_overflow);
    RUN_TEST(test_uint16_extract_rejects_non_numeric);
    return UNITY_END();
}
