#include "audio/usb_audio.h"

#include <string.h>

#include <esp_log.h>

#include "audio/ring_buffer.h"

#ifdef CONFIG_TX_BUILD
#include "usb_device_uac.h"
#include "esp_private/usb_phy.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static const char *TAG = "usb_audio";

#define USB_AUDIO_BUFFER_SIZE (44100 * 2 * sizeof(int16_t) / 10)  // 100ms buffer

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
#ifdef CONFIG_TX_BUILD
    ESP_LOGI(TAG, "USB audio speaker init");

    // Initialize USB PHY for internal USB
    usb_phy_handle_t phy_hdl;
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_TARGET_INTERNAL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));

    // Create ring buffer for USB audio data
    usb_audio_buffer = ring_buffer_create(USB_AUDIO_BUFFER_SIZE);
    if (!usb_audio_buffer) {
        ESP_LOGE(TAG, "Failed to create USB audio buffer");
        return ESP_FAIL;
    }

    // Initialize USB UAC device
    uac_device_config_t config = {
        .output_cb = usb_audio_output_cb,
        .cb_ctx = NULL,
    };
    esp_err_t ret = uac_device_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB UAC device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB UAC device initialized - device should appear as audio output on host");
    ESP_LOGI(TAG, "If not visible, try disconnecting and reconnecting USB cable");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "USB audio not supported on RX");
    return ESP_OK;
#endif
}

esp_err_t usb_audio_read_frames(int16_t *frames, size_t frame_count, size_t *frames_read) {
#ifdef CONFIG_TX_BUILD
    if (!usb_audio_buffer) {
        *frames_read = 0;
        return ESP_FAIL;
    }

    size_t bytes_to_read = frame_count * 2 * sizeof(int16_t);
    size_t bytes_read = ring_buffer_read(usb_audio_buffer, frames, bytes_to_read);
    *frames_read = bytes_read / (2 * sizeof(int16_t));

    // If we don't have enough data, fill with silence
    if (bytes_read < bytes_to_read) {
        memset((uint8_t *)frames + bytes_read, 0, bytes_to_read - bytes_read);
        *frames_read = frame_count;
    }

    return ESP_OK;
#else
    *frames_read = 0;
    return ESP_OK;
#endif
}

bool usb_audio_is_active(void) {
#ifdef CONFIG_TX_BUILD
    return usb_audio_active;
#else
    return false;
#endif
}
