#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

bool network_is_root(void);
uint8_t network_get_layer(void);
uint32_t network_get_children_count(void);
int network_get_rssi(void);
uint32_t network_get_latency_ms(void);
uint8_t network_get_jitter_prefill_frames(void);
void network_set_jitter_override(int frames);
int network_get_jitter_override(void);
bool network_is_connected(void);
bool network_is_stream_ready(void);
esp_err_t network_trigger_rejoin(void);
uint32_t network_get_connected_nodes(void);
uint32_t network_get_tx_bytes_and_reset(void);
int network_get_nearest_child_rssi(void);
uint32_t network_get_nearest_child_latency_ms(void);
