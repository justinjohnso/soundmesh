#include "audio/usb_audio.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#include "audio/ring_buffer.h"
#include "audio/usb_device_uac.h"
#include "config/build.h"
#include "config/build_role.h"

static const char *TAG = "usb_audio";

extern esp_err_t uac_device_init(uac_device_config_t *config) __attribute__((weak));

static ring_buffer_t *s_usb_audio_buffer = NULL;
static bool s_usb_runtime_ready = false;
static bool s_usb_uac_registered = false;
static int64_t s_runtime_start_us = 0;
static int64_t s_last_data_us = 0;

static esp_err_t usb_audio_output_cb(uint8_t *buf, size_t len, void *cb_ctx) {
    (void)cb_ctx;

    if (!s_usb_runtime_ready || !s_usb_audio_buffer || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ring_buffer_write(s_usb_audio_buffer, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "USB ring buffer full, dropping %u bytes", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    s_last_data_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t usb_audio_init(void) {
#if BUILD_IS_SOURCE
    if (s_usb_runtime_ready) {
        return ESP_OK;
    }

    if (!s_usb_audio_buffer) {
        s_usb_audio_buffer = ring_buffer_create(USB_AUDIO_BUFFER_BYTES);
        if (!s_usb_audio_buffer) {
            ESP_LOGE(TAG, "Failed to create USB ring buffer (%u bytes)",
                     (unsigned)USB_AUDIO_BUFFER_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_usb_uac_registered) {
        if (uac_device_init == NULL) {
            ESP_LOGW(TAG, "USB UAC runtime unavailable (uac_device_init missing)");
            ring_buffer_destroy(s_usb_audio_buffer);
            s_usb_audio_buffer = NULL;
            return ESP_ERR_NOT_SUPPORTED;
        }

        uac_device_config_t cfg = {0};
        cfg.output_cb = usb_audio_output_cb;

        esp_err_t ret = uac_device_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(ret));
            ring_buffer_destroy(s_usb_audio_buffer);
            s_usb_audio_buffer = NULL;
            return ret;
        }
        s_usb_uac_registered = true;
    }

    s_runtime_start_us = esp_timer_get_time();
    s_last_data_us = 0;
    s_usb_runtime_ready = true;
    ESP_LOGI(TAG, "USB audio runtime ready (buffer=%u bytes)", (unsigned)USB_AUDIO_BUFFER_BYTES);
#endif
    return ESP_OK;
}

esp_err_t usb_audio_deinit(void) {
#if BUILD_IS_SOURCE
    s_usb_runtime_ready = false;
    s_runtime_start_us = 0;
    s_last_data_us = 0;
    if (s_usb_audio_buffer) {
        ring_buffer_destroy(s_usb_audio_buffer);
        s_usb_audio_buffer = NULL;
    }
#endif
    return ESP_OK;
}

esp_err_t usb_audio_read_stereo(int16_t *stereo_buffer, size_t max_frames, size_t *frames_read) {
    if (!stereo_buffer || !frames_read) {
        return ESP_ERR_INVALID_ARG;
    }

    *frames_read = 0;

#if BUILD_IS_SOURCE
    if (!s_usb_runtime_ready || !s_usb_audio_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t bytes_to_read = max_frames * USB_AUDIO_FRAME_BYTES_STEREO;
    if (ring_buffer_available(s_usb_audio_buffer) < bytes_to_read) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ring_buffer_read(s_usb_audio_buffer, (uint8_t *)stereo_buffer, bytes_to_read);
    if (ret != ESP_OK) {
        return ret;
    }

    *frames_read = max_frames;
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

bool usb_audio_is_ready(void) {
    return s_usb_runtime_ready;
}

bool usb_audio_is_active(void) {
    if (!s_usb_runtime_ready || s_last_data_us == 0) {
        return false;
    }
    return usb_audio_get_inactive_ms() < USB_AUDIO_INACTIVITY_TIMEOUT_MS;
}

uint32_t usb_audio_get_inactive_ms(void) {
    if (!s_usb_runtime_ready) {
        return UINT32_MAX;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t reference_us = s_last_data_us;
    if (reference_us == 0) {
        reference_us = s_runtime_start_us;
    }
    if (reference_us < 0 || now_us < reference_us) {
        return 0;
    }

    uint64_t inactive_ms = (uint64_t)(now_us - reference_us) / 1000ULL;
    if (inactive_ms > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)inactive_ms;
}
