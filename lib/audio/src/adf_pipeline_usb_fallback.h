#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include "config/build.h"

static inline bool adf_pipeline_usb_should_fallback(bool usb_ready,
                                                    uint32_t inactive_ms,
                                                    TickType_t now_ticks,
                                                    TickType_t *confirm_start_ticks)
{
    if (!confirm_start_ticks) {
        return false;
    }

    bool usb_timed_out = (!usb_ready) || (inactive_ms >= USB_AUDIO_INACTIVITY_TIMEOUT_MS);
    if (!usb_timed_out) {
        *confirm_start_ticks = 0;
        return false;
    }

    if (*confirm_start_ticks == 0) {
        *confirm_start_ticks = now_ticks;
        return false;
    }

    if ((now_ticks - *confirm_start_ticks) >= pdMS_TO_TICKS(USB_AUDIO_INACTIVITY_CONFIRM_MS)) {
        *confirm_start_ticks = 0;
        return true;
    }
    return false;
}
