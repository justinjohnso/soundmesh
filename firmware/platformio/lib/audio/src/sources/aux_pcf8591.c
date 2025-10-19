#include "audio/source.h"
#include "esp_log.h"

static const char *TAG = "aux_source";

static esp_err_t aux_init(const void *cfg) {
    (void)cfg;
    ESP_LOGW(TAG, "AUX audio source not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

static size_t aux_read(int16_t *dst, size_t frames, uint32_t timeout_ms) {
    (void)dst;
    (void)frames;
    (void)timeout_ms;
    return 0;
}

static void aux_deinit(void) {
    ESP_LOGI(TAG, "AUX source deinitialized");
}

const audio_source_t aux_source = {
    .init = aux_init,
    .read = aux_read,
    .deinit = aux_deinit,
};
