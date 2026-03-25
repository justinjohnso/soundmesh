#pragma once

#include "status.h"
#include <esp_err.h>
#include <stdbool.h>

#define DASH_WIDTH 48
#define DASH_MONITOR_LINE_MAX 128
#define DASH_MONITOR_HISTORY_MAX 48

void dashboard_init(void);

void dashboard_render_src(const src_status_t *status);
void dashboard_render_out(const out_status_t *status);

void dashboard_log(const char *fmt, ...);
int dashboard_serialize_recent_json(char *buf, size_t buf_size);
