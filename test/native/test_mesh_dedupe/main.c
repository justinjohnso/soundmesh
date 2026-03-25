#include <string.h>

#include <unity.h>

#include "mesh/mesh_dedupe.h"

static int64_t stub_time_us = 0;

int64_t esp_timer_get_time(void)
{
    return stub_time_us;
}

#include "../../../lib/network/src/mesh/mesh_dedupe.c"

static void reset_dedupe_state(void)
{
    mesh_dedupe_reset();
    stub_time_us = 0;
}

void setUp(void)
{
    reset_dedupe_state();
}

void tearDown(void)
{
}

void test_mark_seen_turns_entry_into_duplicate(void)
{
    TEST_ASSERT_FALSE(mesh_dedupe_is_duplicate(7, 1234));

    mesh_dedupe_mark_seen(7, 1234);

    TEST_ASSERT_TRUE(mesh_dedupe_is_duplicate(7, 1234));
}

void test_stream_id_scopes_deduplication(void)
{
    mesh_dedupe_mark_seen(1, 42);

    TEST_ASSERT_TRUE(mesh_dedupe_is_duplicate(1, 42));
    TEST_ASSERT_FALSE(mesh_dedupe_is_duplicate(2, 42));
}

void test_mark_seen_stores_millisecond_timestamp(void)
{
    stub_time_us = 1234567;

    mesh_dedupe_mark_seen(3, 99);

    TEST_ASSERT_EQUAL_UINT32(1234, dedupe_cache[0].timestamp_ms);
}

void test_cache_eviction_overwrites_oldest_entry(void)
{
    for (int i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        mesh_dedupe_mark_seen(9, (uint16_t)i);
    }
    TEST_ASSERT_TRUE(mesh_dedupe_is_duplicate(9, 0));

    mesh_dedupe_mark_seen(9, 1000);

    TEST_ASSERT_FALSE(mesh_dedupe_is_duplicate(9, 0));
    TEST_ASSERT_TRUE(mesh_dedupe_is_duplicate(9, 1000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mark_seen_turns_entry_into_duplicate);
    RUN_TEST(test_stream_id_scopes_deduplication);
    RUN_TEST(test_mark_seen_stores_millisecond_timestamp);
    RUN_TEST(test_cache_eviction_overwrites_oldest_entry);
    return UNITY_END();
}
