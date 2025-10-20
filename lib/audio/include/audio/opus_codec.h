#pragma once

#include <esp_err.h>
#include <stdint.h>

esp_err_t opus_codec_init(void);
esp_err_t opus_codec_encode(const int16_t *pcm_data, int pcm_samples, uint8_t *opus_data, int *opus_len);
esp_err_t opus_codec_decode(const uint8_t *opus_data, int opus_len, int16_t *pcm_data, int *pcm_samples);
