#include "audio/usb_audio.h"
#include "config/build.h"
#include <esp_log.h>
#include <string.h>
#include <math.h>

#ifdef CONFIG_TX_BUILD
#include "usb_device_uac.h"
#include "ring_buffer.h"
#endif

static const char *TAG = "usb_audio";

#ifdef CONFIG_TX_BUILD
// USB audio speaker implementation
#define USB_AUDIO_BUFFER_SIZE (AUDIO_FRAME_SAMPLES * 4 * 2)  // Buffer for 4 frames (bytes)
static ring_buffer_t *usb_audio_buffer = NULL;
static bool usb_audio_active = false;

// Audio format: 44.1kHz, 16-bit, mono (we'll convert to stereo later)
#define USB_SAMPLE_RATE 44100
#define USB_CHANNELS 1
#define USB_BITS_PER_SAMPLE 16

// Callback for USB audio output (speaker input from host)
static esp_err_t usb_audio_output_cb(uint8_t *buf, size_t len, void *cb_ctx) {
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
#endif

esp_err_t usb_audio_init(void) {
#ifdef CONFIG_TX_BUILD
ESP_LOGI(TAG, "USB audio speaker init");

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

    ESP_LOGI(TAG, "USB audio initialized");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "USB audio not supported on RX");
    return ESP_OK;
#endif
}

esp_err_t usb_audio_read_frames(int16_t *buffer, size_t num_samples) {
#ifdef CONFIG_TX_BUILD
if (!usb_audio_active || !usb_audio_buffer) {
memset(buffer, 0, num_samples * sizeof(int16_t));
usb_audio_active = false;  // Reset active flag if no buffer
    return ESP_OK;
    }

// Read from USB audio buffer
size_t bytes_to_read = num_samples * sizeof(int16_t);
if (ring_buffer_read(usb_audio_buffer, (uint8_t*)buffer, bytes_to_read) != ESP_OK) {
// Not enough data, fill with silence and mark inactive
    memset(buffer, 0, bytes_to_read);
        usb_audio_active = false;
}

return ESP_OK;
#else
// RX doesn't support USB audio
    memset(buffer, 0, num_samples * sizeof(int16_t));
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
