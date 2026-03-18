#pragma once

#include <esp_err.h>
#include <stdbool.h>

// Initialize the portal subsystem (USB networking + HTTP + WebSocket + DNS)
// Only call on TX/COMBO builds after mesh is ready.
esp_err_t portal_init(void);

// Check if portal is running
bool portal_is_running(void);
