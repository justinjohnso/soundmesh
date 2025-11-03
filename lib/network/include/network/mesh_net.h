#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t network_init_ap(void);
esp_err_t network_init_sta(void);
esp_err_t network_udp_send(const uint8_t *data, size_t len);
esp_err_t network_udp_send_audio(const uint8_t *data, size_t len);  // Low-priority audio socket
esp_err_t network_udp_recv(uint8_t *data, size_t max_len, size_t *actual_len, uint32_t timeout_ms);
esp_err_t network_start_latency_measurement(void);
int network_get_rssi(void);
uint32_t network_get_latency_ms(void);
uint32_t network_get_connected_nodes(void);
bool network_is_stream_ready(void);  // True when STA has IP and can receive audio

// Minimal network framing header
#define NET_FRAME_MAGIC 0xA5
#define NET_FRAME_VERSION 1

typedef enum {
	NET_PKT_TYPE_AUDIO_RAW = 1,
	NET_PKT_TYPE_PING = 2,
	NET_PKT_TYPE_CONTROL = 0x10,
} net_pkt_type_t;

// header size (must be multiple of alignment)
#define NET_FRAME_HEADER_SIZE 12

// Frame header (network byte order for multi-byte fields)
typedef struct __attribute__((packed)) {
	uint8_t magic;       // 0xA5
	uint8_t version;     // header version
	uint8_t type;        // net_pkt_type_t
	uint8_t reserved;    // padding for alignment
	uint16_t seq;        // sequence number (big-endian)
	uint32_t timestamp;  // milliseconds
	uint16_t payload_len; // length of payload in bytes (big-endian)
} net_frame_header_t;
