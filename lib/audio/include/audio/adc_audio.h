#pragma once

#include <esp_err.h>
#include <stdint.h>

esp_err_t adc_audio_init(void);
esp_err_t adc_audio_read(uint8_t *left_value, uint8_t *right_value);
