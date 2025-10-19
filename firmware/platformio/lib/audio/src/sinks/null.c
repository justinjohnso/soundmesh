#include "audio/sink.h"
#include "esp_log.h"

static const char *TAG = "null_sink";

static esp_err_t null_init(const void *cfg) {
    (void)cfg;
    ESP_LOGI(TAG, "Null sink initialized");
    return ESP_OK;
}

static size_t null_write(const int16_t *src, size_t frames, uint32_t timeout_ms) {
    (void)src;
    (void)timeout_ms;
    return frames;
}

static void null_deinit(void) {
    ESP_LOGI(TAG, "Null sink deinitialized");
}

const audio_sink_t null_sink = {
    .init = null_init,
    .write = null_write,
    .deinit = null_deinit,
};
