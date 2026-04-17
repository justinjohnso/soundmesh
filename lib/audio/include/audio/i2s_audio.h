#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t i2s_audio_init(void);
esp_err_t i2s_audio_deinit(void);
esp_err_t i2s_audio_read_stereo(int16_t *buffer, size_t max_frames, size_t *read);
esp_err_t i2s_audio_write_stereo(const int16_t *buffer, size_t frames);
esp_err_t i2s_audio_write_mono_as_stereo(const int16_t *mono_samples, size_t num_mono_samples);
bool i2s_audio_is_ready(void);
