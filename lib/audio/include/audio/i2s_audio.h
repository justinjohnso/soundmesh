#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

esp_err_t i2s_audio_init(void);
esp_err_t i2s_audio_write_samples(const int16_t *samples, size_t num_samples);
