// Placeholder - Opus codec removed for now to focus on improving raw PCM quality
#include "audio/opus_codec.h"
#include <esp_log.h>

static const char *TAG = "opus_codec";

esp_err_t opus_codec_init(void) {
    ESP_LOGI(TAG, "Opus codec placeholder - using raw PCM for now");
    return ESP_OK;
}

esp_err_t opus_codec_encode(const int16_t *pcm_data, int pcm_samples, uint8_t *opus_data, int *opus_len) {
    // Placeholder - just return error to use raw PCM
    return ESP_FAIL;
}

esp_err_t opus_codec_decode(const uint8_t *opus_data, int opus_len, int16_t *pcm_data, int *pcm_samples) {
    // Placeholder - just return error to use raw PCM
    return ESP_FAIL;
}
