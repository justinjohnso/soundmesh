#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    INPUT_MODE_USB,
    INPUT_MODE_AUX,
    INPUT_MODE_TONE
} input_mode_t;

typedef enum {
    DISPLAY_VIEW_NETWORK,
    DISPLAY_VIEW_AUDIO
} display_view_t;

typedef struct {
    input_mode_t input_mode;
    bool audio_active;
    uint32_t connected_nodes;
    uint32_t latency_ms;
    uint32_t bandwidth_kbps;
} tx_status_t;

typedef struct {
    int rssi;
    uint32_t latency_ms;
    uint32_t hops;
    bool receiving_audio;
    uint32_t bandwidth_kbps;
} rx_status_t;
