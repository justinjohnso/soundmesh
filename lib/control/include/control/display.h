#pragma once

#include "status.h"
#include <esp_err.h>

esp_err_t display_init(void);
void display_clear(void);
void display_render_src(display_view_t view, const src_status_t *status);
void display_render_out(display_view_t view, const out_status_t *status);
void display_show_message(const char *line1, const char *line2);
