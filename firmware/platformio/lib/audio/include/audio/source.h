#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Audio source interface
typedef struct {
    esp_err_t (*init)(const void *cfg);
    size_t (*read)(int16_t *dst, size_t frames, uint32_t timeout_ms);
    void (*deinit)(void);
} audio_source_t;

// Tone source
extern const audio_source_t tone_source;

// USB audio source (TinyUSB)
extern const audio_source_t usb_source;

// AUX input source (PCF8591)
extern const audio_source_t aux_source;
