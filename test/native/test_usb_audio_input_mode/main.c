#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef TickType_t
typedef uint32_t TickType_t;
#endif

#include "audio/ring_buffer.h"
#include "audio/usb_device_uac.h"
#include "config/build.h"

#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif

struct ring_buffer_t {
    uint8_t *data;
    size_t capacity;
    size_t read_idx;
    size_t write_idx;
    size_t used;
};

static int64_t s_now_us = 0;
static uac_output_cb_t s_uac_output_cb = NULL;
static esp_err_t s_uac_init_result = ESP_OK;

ring_buffer_t *ring_buffer_create(size_t size)
{
    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) {
        return NULL;
    }
    rb->data = calloc(1, size);
    if (!rb->data) {
        free(rb);
        return NULL;
    }
    rb->capacity = size;
    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) {
        return;
    }
    free(rb->data);
    free(rb);
}

esp_err_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > (rb->capacity - rb->used)) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < len; i++) {
        rb->data[rb->write_idx] = data[i];
        rb->write_idx = (rb->write_idx + 1) % rb->capacity;
    }
    rb->used += len;
    return ESP_OK;
}

esp_err_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > rb->used) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = rb->data[rb->read_idx];
        rb->read_idx = (rb->read_idx + 1) % rb->capacity;
    }
    rb->used -= len;
    return ESP_OK;
}

size_t ring_buffer_available(ring_buffer_t *rb)
{
    return rb ? rb->used : 0;
}

esp_err_t uac_device_init(uac_device_config_t *config)
{
    if (config) {
        s_uac_output_cb = config->output_cb;
    }
    return s_uac_init_result;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

#define CONFIG_SRC_BUILD 1
#include "../../../lib/audio/src/usb_audio.c"
#include "../../../lib/audio/src/adf_pipeline_usb_fallback.h"

static void push_usb_frames(const int16_t *samples, size_t frame_count)
{
    TEST_ASSERT_NOT_NULL(s_uac_output_cb);
    size_t bytes = frame_count * USB_AUDIO_FRAME_BYTES_STEREO;
    TEST_ASSERT_EQUAL_INT(ESP_OK, s_uac_output_cb((uint8_t *)samples, bytes, NULL));
}

void setUp(void)
{
    s_now_us = 0;
    s_uac_init_result = ESP_OK;
    usb_audio_deinit();
}

void tearDown(void)
{
    usb_audio_deinit();
}

void test_usb_init_registers_callback_and_marks_ready(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_init());
    TEST_ASSERT_TRUE(usb_audio_is_ready());
    TEST_ASSERT_NOT_NULL(s_uac_output_cb);
}

void test_usb_init_failure_leaves_runtime_not_ready(void)
{
    s_uac_init_result = ESP_FAIL;
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, usb_audio_init());
    TEST_ASSERT_FALSE(usb_audio_is_ready());
}

void test_usb_read_stereo_requires_buffered_data(void)
{
    int16_t stereo[8] = {0};
    size_t frames_read = 123;

    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_init());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, usb_audio_read_stereo(stereo, 4, &frames_read));
    TEST_ASSERT_EQUAL_UINT32(0, frames_read);
}

void test_usb_activity_timeout_drives_inactive_state_for_fallback(void)
{
    int16_t stereo[8] = {1, -1, 2, -2, 3, -3, 4, -4};
    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_init());

    s_now_us = 1000 * 1000;
    push_usb_frames(stereo, 4);
    TEST_ASSERT_TRUE(usb_audio_is_active());

    s_now_us += (int64_t)(USB_AUDIO_INACTIVITY_TIMEOUT_MS + 25) * 1000;
    TEST_ASSERT_FALSE(usb_audio_is_active());
    TEST_ASSERT_TRUE(usb_audio_get_inactive_ms() >= USB_AUDIO_INACTIVITY_TIMEOUT_MS);
}

void test_usb_inactive_window_starts_at_runtime_init_before_first_packet(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_init());
    TEST_ASSERT_FALSE(usb_audio_is_active());
    TEST_ASSERT_EQUAL_UINT32(0, usb_audio_get_inactive_ms());

    s_now_us = (int64_t)(USB_AUDIO_INACTIVITY_TIMEOUT_MS - 1) * 1000;
    TEST_ASSERT_TRUE(usb_audio_get_inactive_ms() < USB_AUDIO_INACTIVITY_TIMEOUT_MS);

    s_now_us = (int64_t)(USB_AUDIO_INACTIVITY_TIMEOUT_MS + 1) * 1000;
    TEST_ASSERT_TRUE(usb_audio_get_inactive_ms() >= USB_AUDIO_INACTIVITY_TIMEOUT_MS);
}

void test_usb_read_stereo_returns_pushed_samples(void)
{
    int16_t in[8] = {100, -100, 200, -200, 300, -300, 400, -400};
    int16_t out[8] = {0};
    size_t frames_read = 0;

    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_init());
    s_now_us = 2000 * 1000;
    push_usb_frames(in, 4);

    TEST_ASSERT_EQUAL_INT(ESP_OK, usb_audio_read_stereo(out, 4, &frames_read));
    TEST_ASSERT_EQUAL_UINT32(4, frames_read);
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 8);
}

void test_pipeline_usb_fallback_waits_for_timeout_and_confirm_windows(void)
{
    TickType_t confirm_start = 0;
    TickType_t timeout_tick = 50;
    TickType_t confirm_ticks = pdMS_TO_TICKS(USB_AUDIO_INACTIVITY_CONFIRM_MS);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS - 1,
                                                       timeout_tick - 1,
                                                       &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(0, confirm_start);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS,
                                                       timeout_tick,
                                                       &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(timeout_tick, confirm_start);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS + 1,
                                                       timeout_tick + confirm_ticks - 1,
                                                       &confirm_start));

    TEST_ASSERT_TRUE(adf_pipeline_usb_should_fallback(true,
                                                      USB_AUDIO_INACTIVITY_TIMEOUT_MS + 1,
                                                      timeout_tick + confirm_ticks,
                                                      &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(0, confirm_start);
}

void test_pipeline_usb_fallback_confirm_resets_when_input_recovers(void)
{
    TickType_t confirm_start = 0;
    TickType_t timeout_tick = 100;
    TickType_t confirm_ticks = pdMS_TO_TICKS(USB_AUDIO_INACTIVITY_CONFIRM_MS);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS + 5,
                                                       timeout_tick,
                                                       &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(timeout_tick, confirm_start);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS - 10,
                                                       timeout_tick + 1,
                                                       &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(0, confirm_start);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS + 5,
                                                       timeout_tick + 2,
                                                       &confirm_start));
    TEST_ASSERT_EQUAL_UINT32(timeout_tick + 2, confirm_start);

    TEST_ASSERT_FALSE(adf_pipeline_usb_should_fallback(true,
                                                       USB_AUDIO_INACTIVITY_TIMEOUT_MS + 5,
                                                       timeout_tick + 2 + confirm_ticks - 1,
                                                       &confirm_start));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_usb_init_failure_leaves_runtime_not_ready);
    RUN_TEST(test_usb_init_registers_callback_and_marks_ready);
    RUN_TEST(test_usb_read_stereo_requires_buffered_data);
    RUN_TEST(test_usb_activity_timeout_drives_inactive_state_for_fallback);
    RUN_TEST(test_usb_inactive_window_starts_at_runtime_init_before_first_packet);
    RUN_TEST(test_usb_read_stereo_returns_pushed_samples);
    RUN_TEST(test_pipeline_usb_fallback_waits_for_timeout_and_confirm_windows);
    RUN_TEST(test_pipeline_usb_fallback_confirm_resets_when_input_recovers);
    return UNITY_END();
}
