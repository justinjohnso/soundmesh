#include "audio/source.h"
#include "esp_log.h"

static const char *TAG = "usb_source";

static esp_err_t usb_init(const void *cfg) {
    (void)cfg;
    ESP_LOGW(TAG, "USB audio source not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

static size_t usb_read(int16_t *dst, size_t frames, uint32_t timeout_ms) {
    (void)dst;
    (void)frames;
    (void)timeout_ms;
    return 0;
}

static void usb_deinit(void) {
    ESP_LOGI(TAG, "USB source deinitialized");
}

const audio_source_t usb_source = {
    .init = usb_init,
    .read = usb_read,
    .deinit = usb_deinit,
};
