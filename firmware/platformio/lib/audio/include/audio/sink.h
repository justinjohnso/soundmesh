#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Audio sink interface
typedef struct {
    esp_err_t (*init)(const void *cfg);
    size_t (*write)(const int16_t *src, size_t frames, uint32_t timeout_ms);
    void (*deinit)(void);
} audio_sink_t;

// I2S DAC sink (UDA1334)
extern const audio_sink_t i2s_dac_sink;

// Null sink (for testing)
extern const audio_sink_t null_sink;
