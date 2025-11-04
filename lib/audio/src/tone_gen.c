#include "audio/tone_gen.h"
#include "config/build.h"
#include <math.h>
#include <esp_log.h>

static const char *TAG = "tone_gen";
static double phase = 0.0;
static float phase_increment = 0.0f;

esp_err_t tone_gen_init(uint32_t freq_hz) {
    phase_increment = 2.0f * M_PI * freq_hz / AUDIO_SAMPLE_RATE;
    ESP_LOGI(TAG, "Tone generator initialized: %luHz", freq_hz);
    return ESP_OK;
}

void tone_gen_set_frequency(uint32_t freq_hz) {
phase_increment = 2.0f * M_PI * freq_hz / AUDIO_SAMPLE_RATE;
phase = 0.0;  // Reset phase to avoid discontinuity
}

void tone_gen_fill_buffer(int16_t *buffer, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        buffer[i] = (int16_t)(sin(phase) * 16000.0);
        phase += phase_increment;
        if (phase >= 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    }
}
