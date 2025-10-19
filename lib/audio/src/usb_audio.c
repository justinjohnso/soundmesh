#include "audio/usb_audio.h"
#include "config/build.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "usb_audio";

// USB audio is disabled for tone testing - pure stub implementation

esp_err_t usb_audio_init(void) {
    ESP_LOGI(TAG, "USB audio init (stub - disabled for tone testing)");
    return ESP_OK;
}

esp_err_t usb_audio_read_frames(int16_t *buffer, size_t num_samples) {
// Return silence
memset(buffer, 0, num_samples * sizeof(int16_t));
return ESP_ERR_NOT_FOUND;
}

bool usb_audio_is_active(void) {
return false;
}
