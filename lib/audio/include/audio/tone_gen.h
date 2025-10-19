#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

esp_err_t tone_gen_init(uint32_t freq_hz);
void tone_gen_fill_buffer(int16_t *buffer, size_t num_samples);
