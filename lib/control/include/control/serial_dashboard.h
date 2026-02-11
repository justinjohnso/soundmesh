#pragma once

#include "status.h"
#include <esp_err.h>
#include <stdbool.h>

#define DASH_WIDTH 48

void dashboard_init(void);

void dashboard_render_tx(const tx_status_t *status);
void dashboard_render_rx(const rx_status_t *status);
void dashboard_render_combo(const combo_status_t *status);

void dashboard_log(const char *fmt, ...);
