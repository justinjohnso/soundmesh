#include "audio/usb_audio.h"

#include <string.h>

#include <esp_log.h>

#include "audio/ring_buffer.h"

#ifdef CONFIG_TX_BUILD
// TODO: USB audio support (v0.2) - requires usb_device_uac.h from ESP-IDF
// #include "usb_device_uac.h"
// #include "esp_private/usb_phy.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static const char *TAG = "usb_audio";

#define USB_AUDIO_BUFFER_SIZE (48000 * 2 * 3 / 10)  // 100ms buffer @ 48kHz 24-bit stereo

static ring_buffer_t *usb_audio_buffer = NULL;

static bool usb_audio_active = false;

// Callback for USB audio output (speaker input from host)
static esp_err_t usb_audio_output_cb(uint8_t *buf, size_t len, void *cb_ctx) {
    (void)cb_ctx;

    if (!usb_audio_buffer) {
        return ESP_FAIL;
    }

    // Write audio data to ring buffer
    if (ring_buffer_write(usb_audio_buffer, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "USB audio buffer full, dropping %d bytes", len);
    }

    usb_audio_active = true;
    return ESP_OK;
}

esp_err_t usb_audio_init(void) {
    // TODO: USB audio support deferred to v0.2
    // Requires ESP-IDF TinyUSB UAC component configuration
    ESP_LOGI(TAG, "USB audio init (stub - v0.2 feature)");
    return ESP_OK;
}

esp_err_t usb_audio_read_frames(int16_t *frames, size_t frame_count, size_t *frames_read) {
#ifdef CONFIG_TX_BUILD
    if (!usb_audio_buffer) {
        *frames_read = 0;
        return ESP_FAIL;
    }

    // Read 16-bit stereo directly
    size_t bytes_to_read = frame_count * 2 * sizeof(int16_t);
    size_t bytes_read = ring_buffer_read(usb_audio_buffer, frames, bytes_to_read);
    *frames_read = bytes_read / (2 * sizeof(int16_t));

    // If we don't have enough data, fill with silence
    if (*frames_read < frame_count) {
        memset(&frames[*frames_read * 2], 0, (frame_count - *frames_read) * 2 * sizeof(int16_t));
        *frames_read = frame_count;
    }

    return ESP_OK;
#else
    *frames_read = 0;
    return ESP_OK;
#endif
}

bool usb_audio_is_active(void) {
    // TODO: Stub for v0.1 - implement in v0.2
    return false;
}
