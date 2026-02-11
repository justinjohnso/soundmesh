#include "control/serial_dashboard.h"
#include "config/build.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#define W DASH_WIDTH
#define DIVIDER "================================================"

static uint32_t uptime_sec(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void print_uptime(char *buf, size_t len) {
    uint32_t s = uptime_sec();
    uint32_t h = s / 3600;
    uint32_t m = (s % 3600) / 60;
    s = s % 60;
    snprintf(buf, len, "%luh%02lum%02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

static uint32_t get_free_heap_kb(void) {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
}

static void print_ram(char *buf, size_t len) {
    uint32_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t free_mem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t used_pct = (total > 0) ? ((total - free_mem) * 100 / total) : 0;
    snprintf(buf, len, "%lu%% (%luK free)", (unsigned long)used_pct,
             (unsigned long)(free_mem / 1024));
}

void dashboard_init(void) {
    printf("\n%s\n", DIVIDER);
    printf("  SoundMesh Serial Dashboard\n");
    printf("%s\n\n", DIVIDER);
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
    fflush(stdout);
}

void dashboard_render_tx(const tx_status_t *status) {
    char up[16], ram[24];
    print_uptime(up, sizeof(up));
    print_ram(ram, sizeof(ram));

    const char *mode = "???";
    if (status->input_mode == INPUT_MODE_AUX) mode = "AUX";
    else if (status->input_mode == INPUT_MODE_TONE) mode = "Tone";
    else if (status->input_mode == INPUT_MODE_USB) mode = "USB";

    const char *state = status->audio_active ? "ON" : "--";

    char rssi_str[16];
    if (status->rssi == -100 || status->rssi == 0)
        snprintf(rssi_str, sizeof(rssi_str), "--");
    else
        snprintf(rssi_str, sizeof(rssi_str), "%ddBm", status->rssi);

    printf("--- TX | %s ------------------------------------\n", up);
    printf("  In:%-4s  Audio:%-3s  Nodes:%-3lu  RSSI:%-7s\n",
           mode, state, (unsigned long)status->connected_nodes, rssi_str);
    printf("  Ping:%-4lums  TX:%-4lukbps  RAM:%s\n",
           (unsigned long)status->latency_ms,
           (unsigned long)status->bandwidth_kbps, ram);
    if (status->input_mode == INPUT_MODE_TONE) {
        printf("  Tone: %lu Hz\n", (unsigned long)status->tone_freq_hz);
    }
    printf("\n");

    printf(">rssi:%d,ping:%lu,tx_kbps:%lu,nodes:%lu,audio:%d,input:%d,heap_kb:%lu\n",
           status->rssi, (unsigned long)status->latency_ms,
           (unsigned long)status->bandwidth_kbps,
           (unsigned long)status->connected_nodes,
           status->audio_active ? 1 : 0,
           (int)status->input_mode,
           (unsigned long)get_free_heap_kb());

    fflush(stdout);
}

void dashboard_render_rx(const rx_status_t *status) {
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

    printf("--- RX | %s ------------------------------------\n", up);
    printf("  Audio:%-3s  RSSI:%-7s  Ping:%-6s  Loss:%.1f%%\n",
           state, rssi_str, ping_str, status->loss_pct);
    printf("  Buf:%-3u%%  RX:%-4lukbps  RAM:%s\n",
           status->buffer_pct,
           (unsigned long)status->bandwidth_kbps, ram);
    printf("\n");

    printf(">rssi:%d,ping:%lu,rx_kbps:%lu,buf:%u,loss:%.1f,audio:%d,heap_kb:%lu\n",
           status->rssi, (unsigned long)status->latency_ms,
           (unsigned long)status->bandwidth_kbps,
           status->buffer_pct, status->loss_pct,
           status->receiving_audio ? 1 : 0,
           (unsigned long)get_free_heap_kb());

    fflush(stdout);
}

void dashboard_render_combo(const combo_status_t *status) {
    char up[16], ram[24];
    print_uptime(up, sizeof(up));
    print_ram(ram, sizeof(ram));

    const char *mode = "???";
    if (status->input_mode == INPUT_MODE_AUX) mode = "AUX";
    else if (status->input_mode == INPUT_MODE_TONE) mode = "Tone";
    else if (status->input_mode == INPUT_MODE_USB) mode = "USB";

    const char *state = status->audio_active ? "ON" : "--";

    char rssi_str[16];
    if (status->connected_nodes == 0 || status->nearest_rssi == -100)
        snprintf(rssi_str, sizeof(rssi_str), "--");
    else
        snprintf(rssi_str, sizeof(rssi_str), "%ddBm", status->nearest_rssi);

    printf("--- COMBO | %s ----------------------------------\n", up);
    printf("  In:%-4s  Audio:%-3s  Nodes:%-3lu  TX:%-4lukbps\n",
           mode, state, (unsigned long)status->connected_nodes,
           (unsigned long)status->bandwidth_kbps);
    printf("  RX_RSSI:%-7s  RAM:%s\n", rssi_str, ram);
    if (status->input_mode == INPUT_MODE_TONE) {
        printf("  Tone: %lu Hz\n", (unsigned long)status->tone_freq_hz);
    }
    printf("\n");

    printf(">rx_rssi:%d,tx_kbps:%lu,nodes:%lu,audio:%d,input:%d,heap_kb:%lu\n",
           status->nearest_rssi,
           (unsigned long)status->bandwidth_kbps,
           (unsigned long)status->connected_nodes,
           status->audio_active ? 1 : 0,
           (int)status->input_mode,
           (unsigned long)get_free_heap_kb());

    fflush(stdout);
}
