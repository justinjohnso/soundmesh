#include "control/portal_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "portal_state";

static portal_state_t state;

esp_err_t portal_state_init(void) {
    memset(&state, 0, sizeof(state));
    esp_read_mac(state.self_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Portal state init, self MAC: " MACSTR, MAC2STR(state.self_mac));
    portal_state_update_self();
    return ESP_OK;
}

static portal_node_t *find_node(const uint8_t *mac) {
    for (int i = 0; i < state.node_count; i++) {
        if (memcmp(state.nodes[i].mac, mac, 6) == 0) {
            return &state.nodes[i];
        }
    }
    return NULL;
}

static portal_node_t *add_node(const uint8_t *mac) {
    if (state.node_count >= PORTAL_MAX_NODES) {
        ESP_LOGW(TAG, "Portal state full, cannot add node");
        return NULL;
    }
    portal_node_t *node = &state.nodes[state.node_count];
    memcpy(node->mac, mac, 6);
    state.node_count++;
    return node;
}

void portal_state_update_from_heartbeat(const uint8_t *sender_mac, const mesh_heartbeat_t *hb) {
    portal_node_t *node = find_node(hb->self_mac);
    if (!node) {
        node = add_node(hb->self_mac);
        if (!node) return;
        ESP_LOGI(TAG, "New node: " MACSTR " role=%d", MAC2STR(hb->self_mac), hb->role);
    }
    
    node->role = hb->role;
    node->is_root = hb->is_root;
    node->layer = hb->layer;
    node->rssi = hb->rssi;
    node->children_count = hb->children_count;
    node->stream_active = hb->stream_active;
    memcpy(node->parent_mac, hb->parent_mac, 6);
    node->uptime_ms = hb->uptime_ms;
    node->last_seen_us = esp_timer_get_time();
    node->stale = false;
}

void portal_state_update_self(void) {
    portal_node_t *node = find_node(state.self_mac);
    if (!node) {
        node = add_node(state.self_mac);
        if (!node) return;
    }

    #if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)
        node->role = 1;  // TX
    #else
        node->role = 0;  // RX
    #endif

    node->is_root = esp_mesh_is_root() ? 1 : 0;
    node->layer = esp_mesh_get_layer();
    node->rssi = network_get_rssi();
    node->children_count = network_get_connected_nodes();
    node->stream_active = network_is_stream_ready() ? 1 : 0;
    memcpy(node->mac, state.self_mac, 6);
    memset(node->parent_mac, 0, 6);  // Root has no parent
    node->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    node->last_seen_us = esp_timer_get_time();
    node->stale = false;
}

void portal_state_expire_stale(void) {
    int64_t now_us = esp_timer_get_time();
    int i = 0;
    while (i < state.node_count) {
        // Don't expire self
        if (memcmp(state.nodes[i].mac, state.self_mac, 6) == 0) {
            i++;
            continue;
        }
        
        int64_t age_ms = (now_us - state.nodes[i].last_seen_us) / 1000;
        
        if (age_ms > PORTAL_EXPIRE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Expiring node: " MACSTR, MAC2STR(state.nodes[i].mac));
            if (i < state.node_count - 1) {
                memmove(&state.nodes[i], &state.nodes[i + 1],
                        (state.node_count - i - 1) * sizeof(portal_node_t));
            }
            state.node_count--;
            continue;
        } else if (age_ms > PORTAL_STALE_TIMEOUT_MS) {
            state.nodes[i].stale = true;
        }
        i++;
    }
}

static void mac_to_str(const uint8_t *mac, char *str) {
    sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool is_zero_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static const char *portal_build_label(void) {
#if defined(CONFIG_COMBO_BUILD)
    return "COMBO";
#elif defined(CONFIG_TX_BUILD)
    return "TX";
#elif defined(CONFIG_RX_BUILD)
    return "RX";
#else
    return "UNKNOWN";
#endif
}

static const char *portal_mesh_state(void) {
    return network_is_connected() ? "Mesh OK" : "Mesh Degraded";
}

int portal_state_serialize_json(char *buf, size_t buf_size) {
    portal_state_update_self();
    portal_state_expire_stale();
    
    char self_mac_str[18];
    mac_to_str(state.self_mac, self_mac_str);
    
    int off = snprintf(
        buf,
        buf_size,
        "{\"ts\":%lld,\"self\":\"%s\",\"heapKb\":%lu,\"core0LoadPct\":null,"
        "\"latencyMs\":%lu,\"netIf\":\"%s\",\"buildLabel\":\"%s\","
        "\"meshState\":\"%s\",\"bpm\":null,\"fftBins\":null,\"nodes\":[",
        (long long)(esp_timer_get_time() / 1000),
        self_mac_str,
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        (unsigned long)network_get_latency_ms(),
        "usb_ncm (10.48.0.1)",
        portal_build_label(),
        portal_mesh_state());
    
    for (int i = 0; i < state.node_count && off < (int)buf_size - 200; i++) {
        portal_node_t *n = &state.nodes[i];
        char mac_str[18], parent_str[18];
        mac_to_str(n->mac, mac_str);
        
        if (i > 0) {
            off += snprintf(buf + off, buf_size - off, ",");
        }
        
        off += snprintf(buf + off, buf_size - off,
            "{\"mac\":\"%s\",\"role\":\"%s\",\"root\":%s,"
            "\"layer\":%d,\"rssi\":%d,\"children\":%d,"
            "\"streaming\":%s,",
            mac_str,
            n->role == 1 ? "TX" : "RX",
            n->is_root ? "true" : "false",
            n->layer,
            n->rssi,
            n->children_count,
            n->stream_active ? "true" : "false");
        
        if (is_zero_mac(n->parent_mac)) {
            off += snprintf(buf + off, buf_size - off, "\"parent\":null,");
        } else {
            mac_to_str(n->parent_mac, parent_str);
            off += snprintf(buf + off, buf_size - off, "\"parent\":\"%s\",", parent_str);
        }
        
        off += snprintf(buf + off, buf_size - off,
            "\"uptime\":%lu,\"stale\":%s}",
            (unsigned long)n->uptime_ms,
            n->stale ? "true" : "false");
    }
    
    off += snprintf(buf + off, buf_size - off, "]}");
    
    return off;
}

const portal_state_t *portal_state_get(void) {
    return &state;
}
