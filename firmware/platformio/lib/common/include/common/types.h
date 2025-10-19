#pragma once

#include <stdint.h>
#include <stdbool.h>

// Audio input modes (TX)
typedef enum {
    AUDIO_INPUT_TONE = 0,
    AUDIO_INPUT_USB = 1,
    AUDIO_INPUT_AUX = 2
} audio_input_mode_t;

// Display modes
typedef enum {
    DISPLAY_MODE_PRIMARY = 0,
    DISPLAY_MODE_INFO = 1
} display_mode_t;

// TX status model
typedef struct {
    audio_input_mode_t audio_mode;
    bool is_streaming;
    uint32_t packet_count;
    int rx_node_count;
    uint32_t frame_counter;
} tx_status_t;

// RX status model
typedef struct {
    bool is_streaming;
    uint32_t packet_count;
    uint32_t audio_packet_count;
    int wifi_rssi;
    int mesh_hops;
    uint32_t frame_counter;
    uint32_t bytes_received;
} rx_status_t;

// Button events
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS = 1,
    BUTTON_EVENT_LONG_PRESS = 2
} button_event_t;
