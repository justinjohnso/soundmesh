#pragma once

#include <esp_err.h>
#include <stdbool.h>

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS
} button_event_t;

esp_err_t buttons_init(void);
button_event_t buttons_poll(void);
