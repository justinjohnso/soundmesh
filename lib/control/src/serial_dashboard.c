#include "control/serial_dashboard.h"
#include "config/build.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define W DASH_WIDTH
#define DIVIDER "================================================"

static uint32_t uptime_sec(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static char s_monitor_lines[DASH_MONITOR_HISTORY_MAX][DASH_MONITOR_LINE_MAX];
static uint32_t s_monitor_seq[DASH_MONITOR_HISTORY_MAX];
static uint16_t s_monitor_head = 0;
static uint16_t s_monitor_count = 0;
static uint32_t s_monitor_next_seq = 1;
static SemaphoreHandle_t s_monitor_mutex = NULL;

static void dashboard_monitor_push(const char *line) {
    if (!s_monitor_mutex) {
        s_monitor_mutex = xSemaphoreCreateMutex();
    }
    if (!s_monitor_mutex || !line) {
        return;
    }

    if (xSemaphoreTake(s_monitor_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    strlcpy(s_monitor_lines[s_monitor_head], line, DASH_MONITOR_LINE_MAX);
    s_monitor_seq[s_monitor_head] = s_monitor_next_seq++;
    s_monitor_head = (uint16_t)((s_monitor_head + 1U) % DASH_MONITOR_HISTORY_MAX);
    if (s_monitor_count < DASH_MONITOR_HISTORY_MAX) {
        s_monitor_count++;
    }

    xSemaphoreGive(s_monitor_mutex);
}

static void print_uptime(char *buf, size_t len) {
    uint32_t s = uptime_sec();
    uint32_t h = s / 3600;
    uint32_t m = (s % 3600) / 60;
    s = s % 60;
    snprintf(buf, len, "%luh%02lum%02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

static void print_ram(char *buf, size_t len) {
    uint32_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t free_mem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t used_pct = (total > 0) ? ((total - free_mem) * 100 / total) : 0;
    snprintf(buf, len, "%lu%% (%luK free)", (unsigned long)used_pct,
             (unsigned long)(free_mem / 1024));
}

void dashboard_init(void) {
    if (!s_monitor_mutex) {
        s_monitor_mutex = xSemaphoreCreateMutex();
    }
    printf("\n%s\n", DIVIDER);
    printf("  SoundMesh Serial Dashboard\n");
    printf("%s\n\n", DIVIDER);
    dashboard_monitor_push("SoundMesh Serial Dashboard started");
    fflush(stdout);
}

void dashboard_log(const char *fmt, ...) {
    char buf[128];
    uint32_t s = uptime_sec();
    int off = snprintf(buf, sizeof(buf), "[%4lus] ", (unsigned long)s);

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, args);
    va_end(args);

    printf("%s\n", buf);
    dashboard_monitor_push(buf);
    fflush(stdout);
}

void dashboard_render_src(const src_status_t *status) {
    char up[16], ram[24];
    print_uptime(up, sizeof(up));
    print_ram(ram, sizeof(ram));

    const char *mode = "???";
    if (status->input_mode == INPUT_MODE_AUX) mode = "AUX";
    else if (status->input_mode == INPUT_MODE_TONE) mode = "Tone";
    else if (status->input_mode == INPUT_MODE_USB) mode = "USB";

    const char *state = status->audio_active ? "ON" : "--";
    
    char rssi_str[16];
    if (status->nearest_rssi == -100 || status->nearest_rssi == 0)
        snprintf(rssi_str, sizeof(rssi_str), "--");
    else
        snprintf(rssi_str, sizeof(rssi_str), "%ddBm", status->nearest_rssi);

    printf("--- SRC | %s ------------------------------------\n", up);
    printf("  In:%-4s  Audio:%-3s  Nodes:%-3lu  RSSI:%-7s\n",
           mode, state, (unsigned long)status->connected_nodes, rssi_str);
    printf("  BW:%-4lukbps  Vol:%-.1f  Bat:%u%%  RAM:%s\n",
           (unsigned long)status->bandwidth_kbps, status->output_volume, status->battery_pct, ram);

    if (status->input_mode == INPUT_MODE_USB || status->usb_fallback_to_aux) {
        printf("  USB:Ready:%s Active:%s Fallback:%s\n",
               status->usb_ready ? "Y" : "N",
               status->usb_active ? "Y" : "N",
               status->usb_fallback_to_aux ? "Y" : "N");
    }
    
    if (status->input_mode == INPUT_MODE_TONE) {
        printf("  Tone: %lu Hz\n", (unsigned long)status->tone_freq_hz);
    }
    printf("\n");

    // CSV for plotting
    printf(">rssi:%d,nodes:%lu,tx_kbps:%lu,audio:%d,vol:%.1f,bat:%u,heap_kb:%lu,usb_ready:%d,usb_active:%d,usb_fallback:%d\n",
           status->nearest_rssi,
           (unsigned long)status->connected_nodes,
           (unsigned long)status->bandwidth_kbps,
           status->audio_active ? 1 : 0,
           status->output_volume,
           status->battery_pct,
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
           status->usb_ready ? 1 : 0,
           status->usb_active ? 1 : 0,
           status->usb_fallback_to_aux ? 1 : 0);

    fflush(stdout);
}

void dashboard_render_out(const out_status_t *status) {
    char up[16], ram[24];
    print_uptime(up, sizeof(up));
    print_ram(ram, sizeof(ram));

    const char *state = status->receiving_audio ? "RX" : "--";
    
    char rssi_str[16];
    if (status->rssi == -100)
        snprintf(rssi_str, sizeof(rssi_str), "--");
    else
        snprintf(rssi_str, sizeof(rssi_str), "%ddBm", status->rssi);
        
    char ping_str[16];
    if (status->latency_ms > 0)
        snprintf(ping_str, sizeof(ping_str), "%lums", (unsigned long)status->latency_ms);
    else
        snprintf(ping_str, sizeof(ping_str), "--");

    printf("--- OUT | %s ------------------------------------\n", up);
    printf("  Audio:%-3s  RSSI:%-7s  Ping:%-6s  Loss:%.1f%%\n",
           state, rssi_str, ping_str, status->loss_pct);
    printf("  Buf:%-3u%%  BW:%-4lukbps  Bat:%u%%  RAM:%s\n",
           status->buffer_pct,
           (unsigned long)status->bandwidth_kbps,
           status->battery_pct, ram);
    printf("  Src:%s  %s\n", status->source_src_id, status->connection_state);
    printf("\n");

    // CSV for plotting
    printf(">rssi:%d,ping:%lu,loss:%.1f,buf:%u,rx_kbps:%lu,bat:%u,heap_kb:%lu\n",
           status->rssi, (unsigned long)status->latency_ms,
           status->loss_pct, status->buffer_pct,
           (unsigned long)status->bandwidth_kbps,
           status->battery_pct,
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));

    fflush(stdout);
}

int dashboard_serialize_recent_json(char *buf, size_t buf_size) {
    if (!buf || buf_size < 32) {
        return 0;
    }
    if (!s_monitor_mutex) {
        s_monitor_mutex = xSemaphoreCreateMutex();
    }

    int off = snprintf(buf, buf_size, "{\"monitor\":[");
    if (off < 0 || off >= (int)buf_size) {
        return 0;
    }

    if (!s_monitor_mutex || xSemaphoreTake(s_monitor_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        off += snprintf(buf + off, buf_size - off, "]}");
        return off;
    }

    uint16_t count = s_monitor_count;
    uint16_t start = (uint16_t)((s_monitor_head + DASH_MONITOR_HISTORY_MAX - count) % DASH_MONITOR_HISTORY_MAX);

    for (uint16_t i = 0; i < count && off < (int)buf_size - 32; i++) {
        uint16_t idx = (uint16_t)((start + i) % DASH_MONITOR_HISTORY_MAX);
        const char *line = s_monitor_lines[idx];
        if (i > 0) {
            off += snprintf(buf + off, buf_size - off, ",");
        }
        off += snprintf(buf + off, buf_size - off, "{\"seq\":%lu,\"line\":\"",
                        (unsigned long)s_monitor_seq[idx]);

        for (const char *p = line; *p && off < (int)buf_size - 8; p++) {
            if (*p == '"' || *p == '\\') {
                off += snprintf(buf + off, buf_size - off, "\\%c", *p);
            } else if ((unsigned char)*p < 0x20) {
                off += snprintf(buf + off, buf_size - off, " ");
            } else {
                buf[off++] = *p;
                buf[off] = '\0';
            }
        }
        off += snprintf(buf + off, buf_size - off, "\"}");
    }

    xSemaphoreGive(s_monitor_mutex);
    off += snprintf(buf + off, buf_size - off, "]}");
    return off;
}
