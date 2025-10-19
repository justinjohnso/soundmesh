#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

esp_err_t network_init_ap(void);
esp_err_t network_init_sta(void);
esp_err_t network_udp_send(const uint8_t *data, size_t len);
esp_err_t network_udp_recv(uint8_t *data, size_t max_len, size_t *actual_len, uint32_t timeout_ms);
int network_get_rssi(void);
uint32_t network_get_latency_ms(void);
uint32_t network_get_connected_nodes(void);
