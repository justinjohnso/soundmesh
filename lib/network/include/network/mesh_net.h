#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// ESP-WIFI-MESH Network API (v0.1)
// ============================================================================

// Network initialization
esp_err_t network_init_mesh(void);

// Startup synchronization (event-driven, not polling)
// Call this before your main loop to wait for network readiness via task notification
esp_err_t network_register_startup_notification(TaskHandle_t task_handle);

// Audio transmission/reception
esp_err_t network_send_audio(const uint8_t *data, size_t len);
esp_err_t network_send_control(const uint8_t *data, size_t len);

// Audio reception callback (for RX nodes)
typedef void (*network_audio_callback_t)(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp);
esp_err_t network_register_audio_callback(network_audio_callback_t callback);

// Mesh topology queries
bool network_is_root(void);
uint8_t network_get_layer(void);
uint32_t network_get_children_count(void);

// Network status
int network_get_rssi(void);
uint32_t network_get_latency_ms(void);   // RTT/2 from ping measurements
uint32_t network_get_connected_nodes(void);
bool network_is_stream_ready(void);  // True when connected to mesh
bool network_is_connected(void);     // True when connected to mesh (non-root) or root
esp_err_t network_send_ping(void);   // Send ping to root (RX nodes only)

// Root node: info about nearest child
int network_get_nearest_child_rssi(void);
uint32_t network_get_nearest_child_latency_ms(void);
esp_err_t network_ping_nearest_child(void);  // Root pings nearest child

// Network framing header (aligned with mesh-network-architecture.md)
#define NET_FRAME_MAGIC 0xA5
#define NET_FRAME_VERSION 1

typedef enum {
	NET_PKT_TYPE_AUDIO_RAW = 1,
	NET_PKT_TYPE_HEARTBEAT = 2,
	NET_PKT_TYPE_STREAM_ANNOUNCE = 3,
	NET_PKT_TYPE_CONTROL = 0x10,
	NET_PKT_TYPE_AUDIO_OPUS = 0x11,  // Opus-compressed audio frame
	NET_PKT_TYPE_PING = 0x20,        // Latency measurement request
	NET_PKT_TYPE_PONG = 0x21,        // Latency measurement response
} net_pkt_type_t;

// Ping/Pong packet for RTT measurement
typedef struct __attribute__((packed)) {
	uint8_t type;           // PING or PONG
	uint8_t reserved[3];    // Padding for alignment
	uint32_t timestamp;     // Sender's local timestamp (ms)
} mesh_ping_t;

// Audio frame header (14 bytes, aligned for mesh)
// CRITICAL: This MUST match sizeof(net_frame_header_t)!
#define NET_FRAME_HEADER_SIZE 14

typedef struct __attribute__((packed)) {
	uint8_t magic;          // 0xA5 (NET_FRAME_MAGIC)
	uint8_t version;        // 1
	uint8_t type;           // net_pkt_type_t
	uint8_t stream_id;      // Stream identifier (multi-TX support)
	uint16_t seq;           // Sequence number (network byte order)
	uint32_t timestamp;     // Sender timestamp in ms
	uint16_t payload_len;   // Payload length in bytes (network byte order)
	uint8_t ttl;            // Hop limit (decremented at each relay)
	uint8_t reserved;       // Alignment padding
} net_frame_header_t;

// Heartbeat packet (sent every 2 seconds by all nodes)
typedef struct __attribute__((packed)) {
	uint8_t type;           // 0x02 = HEARTBEAT
	uint8_t role;           // 0=RX, 1=TX (COMBO reports as TX)
	uint8_t is_root;        // 1 if this node is mesh root
	uint8_t layer;          // Hop count from root
	uint32_t uptime_ms;     // Milliseconds since boot
	uint16_t children_count; // Number of mesh children
	int8_t rssi;            // RSSI to parent
	uint8_t reserved;       // Padding
} mesh_heartbeat_t;

// Stream announcement (sent by TX/COMBO on startup and mode change)
typedef struct __attribute__((packed)) {
	uint8_t type;           // 0x03 = STREAM_ANNOUNCE
	uint8_t stream_id;      // Unique ID for this audio stream
	uint32_t sample_rate;   // 48000
	uint8_t channels;       // 1 (mono)
	uint8_t bits_per_sample; // 24
	uint16_t frame_size_ms; // 5
} mesh_stream_announce_t;
