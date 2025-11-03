#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

esp_err_t adc_audio_init(void);
esp_err_t adc_audio_deinit(void);
esp_err_t adc_audio_start(void);
esp_err_t adc_audio_stop(void);
esp_err_t adc_audio_read_stereo(int16_t *stereo_buffer, size_t num_samples, size_t *samples_read);
