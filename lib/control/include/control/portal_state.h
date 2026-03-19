#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include "network/mesh_net.h"

#define PORTAL_MAX_NODES 32
#define PORTAL_STALE_TIMEOUT_MS 6000   // 3 missed heartbeats (2s interval)
#define PORTAL_EXPIRE_TIMEOUT_MS 30000 // Remove after 30s

typedef struct {
    uint8_t mac[6];
    uint8_t role;           // 0=RX, 1=TX
    uint8_t is_root;
    uint8_t layer;
    int8_t rssi;
    uint16_t children_count;
    uint8_t stream_active;
    uint8_t parent_mac[6];
    uint32_t uptime_ms;
    int64_t last_seen_us;   // esp_timer_get_time() when last heartbeat received
    bool stale;
} portal_node_t;

typedef struct {
    portal_node_t nodes[PORTAL_MAX_NODES];
    uint8_t node_count;
    uint8_t self_mac[6];    // "you are here" MAC
} portal_state_t;

esp_err_t portal_state_init(void);
void portal_state_update_from_heartbeat(const uint8_t *sender_mac, const mesh_heartbeat_t *hb);
void portal_state_update_self(void);
void portal_state_expire_stale(void);
int portal_state_serialize_json(char *buf, size_t buf_size);
const portal_state_t *portal_state_get(void);
