#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "network/uplink_control.h"
#include "network/mixer_control.h"

// ============================================================================
// ESP-WIFI-MESH Network API (v0.1)
// ============================================================================

#define NETWORK_SRC_ID_LEN 12

// Network initialization
esp_err_t network_init_mesh(void);
void derive_src_id(const uint8_t mac[6], char out_src_id[NETWORK_SRC_ID_LEN]);
const char *network_get_src_id(void);

// Startup synchronization (event-driven, not polling)
// Call this before your main loop to wait for network readiness via task notification
esp_err_t network_register_startup_notification(TaskHandle_t task_handle);

// Audio transmission/reception
esp_err_t network_send_audio(const uint8_t *data, size_t len);
esp_err_t network_send_control(const uint8_t *data, size_t len);

typedef struct {
    uint32_t tx_audio_packets;
    uint32_t tx_audio_bytes;
    uint32_t tx_audio_send_failures;
    uint32_t tx_audio_queue_full;
    uint32_t tx_audio_no_route;
    uint32_t tx_audio_invalid_state;
    uint32_t tx_control_packets;
    uint32_t tx_control_send_failures;
    uint32_t tx_control_no_route;
    uint32_t tx_control_invalid_state;
    uint32_t rx_audio_packets;
    uint32_t rx_audio_batches;
    uint32_t rx_audio_batch_frames;
    uint32_t rx_audio_callback_missing;
    uint32_t rx_audio_duplicates;
    uint32_t rx_audio_ttl_expired;
    uint32_t rx_audio_invalid_header;
    uint32_t rx_audio_invalid_version;
    uint32_t rx_audio_invalid_payload;
    uint32_t rx_audio_forwarded;
    uint32_t rx_audio_burst_loss_events;
    uint32_t rx_audio_burst_loss_max;
    uint32_t rx_audio_interarrival_jitter_us;
    uint32_t rx_heartbeat_packets;
    uint32_t rx_control_packets;
    uint32_t rx_ping_packets;
    uint32_t rx_pong_packets;
    uint32_t rx_stream_announce_packets;
    uint32_t mesh_recv_errors;
    uint32_t mesh_recv_empty_packets;
    uint32_t parent_connect_events;
    uint32_t parent_disconnect_events;
    uint32_t no_parent_events;
    uint32_t scan_done_events;
    uint32_t rejoin_trigger_events;
    uint32_t rejoin_blocked_events;
    uint32_t rejoin_circuit_breaker_events;
    uint32_t tx_audio_backpressure_level;
} network_transport_stats_t;

esp_err_t network_get_transport_stats(network_transport_stats_t *out_stats);
esp_err_t network_get_transport_stats_and_reset(network_transport_stats_t *out_stats);

// Audio reception callback (for RX nodes)
typedef void (*network_audio_callback_t)(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp, const char *src_id);
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
esp_err_t network_trigger_rejoin(void);  // Force child rejoin to refresh parent path
bool network_rejoin_allowed(void);   // Circuit-breaker status for rejoin attempts

// Dynamic jitter buffer: returns recommended prefill frames based on network state
// Base is JITTER_PREFILL_FRAMES, adds extra for multi-hop or busy networks
uint8_t network_get_jitter_prefill_frames(void);

// Jitter buffer override: set to 1-16 to pin prefill depth; -1 restores auto mode
void network_set_jitter_override(int frames);
int  network_get_jitter_override(void);

// TX bandwidth measurement
uint32_t network_get_tx_bytes_and_reset(void);  // Returns bytes sent since last call

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

// Ping/Pong packet for RTT measurement (game-style sequence ID)
typedef struct __attribute__((packed)) {
	uint8_t type;           // PING or PONG
	uint8_t reserved[3];    // Padding for alignment
	uint32_t ping_id;       // Sequence number (echoed back in PONG)
} mesh_ping_t;

// Audio frame header.
// NET_FRAME_HEADER_SIZE_V1 is retained for compatibility with older deployed nodes.
#define NET_FRAME_HEADER_SIZE     26  // Current wire header size (used for TX)
#define NET_FRAME_HEADER_SIZE_V1  14  // Older header format (no src_id)

typedef struct __attribute__((packed)) {
	uint8_t magic;          // 0xA5 (NET_FRAME_MAGIC)
	uint8_t version;        // 1
	uint8_t type;           // net_pkt_type_t
	uint8_t stream_id;      // Stream identifier (multi-TX support)
	uint16_t seq;           // Sequence number (network byte order)
	uint32_t timestamp;     // Sender timestamp in ms
	uint16_t payload_len;   // Payload length in bytes (network byte order)
	uint8_t ttl;            // Hop limit (decremented at each relay)
	uint8_t frame_count;    // Number of Opus frames in payload batch
	char src_id[NETWORK_SRC_ID_LEN];  // "SRC_A1B2C3"/"OUT_A1B2C3" + '\0'
} net_frame_header_t;

// Heartbeat packet (sent every 2 seconds by all nodes)
typedef struct __attribute__((packed)) {
	uint8_t type;           // 0x02 = HEARTBEAT
	uint8_t role;           // 0=OUT, 1=SRC
	uint8_t is_root;        // 1 if this node is mesh root
	uint8_t layer;          // Hop count from root
	uint32_t uptime_ms;     // Milliseconds since boot
	uint16_t children_count; // Number of mesh children
	int8_t rssi;            // RSSI to parent
	uint8_t stream_active;  // 1 if currently streaming audio
	uint8_t parent_mac[6];  // Parent node MAC for topology edges
	uint8_t self_mac[6];    // Own MAC for identification
	uint16_t parent_conn_count;   // Child: successful parent connects
	uint16_t parent_disc_count;   // Child: parent disconnect events
	uint16_t auth_expire_count;   // Child: auth-expire disconnect events
	uint16_t no_parent_count;     // Child: no-parent-found events
	char src_id[NETWORK_SRC_ID_LEN];  // "SRC_A1B2C3"/"OUT_A1B2C3" + '\0'
} mesh_heartbeat_t;

// Portal state callback (for root to collect heartbeats)
typedef void (*network_heartbeat_callback_t)(const uint8_t *self_mac, const mesh_heartbeat_t *hb);
esp_err_t network_register_heartbeat_callback(network_heartbeat_callback_t callback);

typedef struct {
    bool enabled;
    bool configured;
    bool root_applied;
    bool pending_apply;
    char ssid[UPLINK_SSID_MAX_LEN + 1];
    char last_error[48];
    uint32_t updated_ms;
} network_uplink_status_t;

esp_err_t network_set_uplink_config(const char *ssid, const char *password, bool enabled);
esp_err_t network_get_uplink_status(network_uplink_status_t *out);

typedef struct {
    uint8_t stream_id;
    uint16_t gain_pct;
    bool enabled;
    bool muted;
    bool solo;
    bool active;
} network_mixer_stream_status_t;

typedef struct {
    uint8_t schema_version;
    uint16_t out_gain_pct;
    uint8_t stream_count;
    network_mixer_stream_status_t streams[MIXER_MAX_STREAMS];
    bool applied;
    bool pending_apply;
    char last_error[48];
    uint32_t updated_ms;
} network_mixer_status_t;

typedef esp_err_t (*network_mixer_apply_callback_t)(const network_mixer_status_t *mixer);
esp_err_t network_register_mixer_apply_callback(network_mixer_apply_callback_t callback);
esp_err_t network_set_mixer_state(const network_mixer_status_t *mixer_state);
esp_err_t network_set_mixer_config(uint16_t out_gain_pct);
esp_err_t network_get_mixer_status(network_mixer_status_t *out);

// Stream announcement (sent by TX/COMBO on startup and mode change)
typedef struct __attribute__((packed)) {
	uint8_t type;           // 0x03 = STREAM_ANNOUNCE
	uint8_t stream_id;      // Unique ID for this audio stream
	uint32_t sample_rate;   // 48000
	uint8_t channels;       // 1 (mono)
	uint8_t bits_per_sample; // 16
	uint16_t frame_size_ms; // 20
} mesh_stream_announce_t;
