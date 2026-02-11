#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    INPUT_MODE_USB,
    INPUT_MODE_AUX,
    INPUT_MODE_TONE
} input_mode_t;

typedef enum {
    DISPLAY_VIEW_AUDIO,
    DISPLAY_VIEW_NETWORK,
    DISPLAY_VIEW_INFO,
    DISPLAY_VIEW_COUNT
} display_view_t;

typedef struct {
    input_mode_t input_mode;
    bool audio_active;
    uint32_t connected_nodes;
    uint32_t latency_ms;
    uint32_t bandwidth_kbps;
    int rssi;
    uint32_t tone_freq_hz;
} tx_status_t;

typedef struct {
    int rssi;
    uint32_t latency_ms;
    uint8_t buffer_pct;
    bool receiving_audio;
    uint32_t bandwidth_kbps;
    float loss_pct;
} rx_status_t;

typedef struct {
    input_mode_t input_mode;
    bool audio_active;
    uint32_t connected_nodes;
    uint32_t bandwidth_kbps;
    uint32_t tone_freq_hz;
    float output_volume;
    int nearest_rssi;
} combo_status_t;
