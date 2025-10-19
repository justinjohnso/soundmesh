#pragma once

#include "esp_err.h"
#include "common/types.h"

// Button handle (opaque)
typedef struct button_handle_s *button_handle_t;

// Button configuration
typedef struct {
    int gpio_num;
    uint32_t debounce_ms;
    uint32_t long_press_ms;
} button_config_t;

// Button API
esp_err_t button_init(const button_config_t *config, button_handle_t *out_handle);
button_event_t button_get_event(button_handle_t handle, uint32_t timeout_ms);
void button_deinit(button_handle_t handle);
