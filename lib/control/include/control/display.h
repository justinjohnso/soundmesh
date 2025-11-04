#pragma once

#include "status.h"
#include <esp_err.h>

esp_err_t display_init(void);
void display_clear(void);
void display_render_tx(display_view_t view, const tx_status_t *status);
void display_render_rx(display_view_t view, const rx_status_t *status);
void display_render_combo(display_view_t view, const combo_status_t *status);
