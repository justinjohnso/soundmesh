#include "audio/source.h"
#include "common/config.h"
#include "esp_log.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "tone_source";

#define TONE_FREQ 440.0f

static bool initialized = false;
static float phase = 0.0f;

static esp_err_t tone_init(const void *cfg) {
    (void)cfg;
    phase = 0.0f;
    initialized = true;
    ESP_LOGI(TAG, "Tone source initialized (%.0f Hz)", TONE_FREQ);
    return ESP_OK;
}

static size_t tone_read(int16_t *dst, size_t frames, uint32_t timeout_ms) {
    (void)timeout_ms;
    
    if (!initialized || !dst) {
        return 0;
    }

    float phase_increment = (2.0f * M_PI * TONE_FREQ) / AUDIO_SAMPLE_RATE;

    for (size_t i = 0; i < frames; i++) {
        float sample = sinf(phase) * 16000.0f;  // -16000 to +16000 range
        dst[i] = (int16_t)sample;
        phase += phase_increment;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
    }

    return frames;
}

static void tone_deinit(void) {
    initialized = false;
    ESP_LOGI(TAG, "Tone source deinitialized");
}

const audio_source_t tone_source = {
    .init = tone_init,
    .read = tone_read,
    .deinit = tone_deinit,
};
