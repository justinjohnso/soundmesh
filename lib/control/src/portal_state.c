#include "control/portal_state.h"
#include "control/status.h"
#include "control/display.h"
#include "control/serial_dashboard.h"
#include "audio/adf_pipeline.h"
#include "network/mesh_net.h"
#include "network/uplink_control.h"
#include "network/mixer_control.h"
#include "audio/usb_audio.h"
#include "control/memory_monitor.h"
#include "config/build.h"
#include "config/build_role.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "portal_state";
static portal_state_t state;

extern adf_pipeline_handle_t adf_pipeline_get_latest_pipeline(void);

esp_err_t portal_state_init(void) {
    memset(&state, 0, sizeof(state));
    esp_read_mac(state.self_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Portal state initialized for self: " MACSTR, MAC2STR(state.self_mac));
    return ESP_OK;
}

static portal_node_t *find_node(const uint8_t *mac) {
    for (int i = 0; i < state.node_count; i++) {
        if (memcmp(state.nodes[i].mac, mac, 6) == 0) return &state.nodes[i];
    }
    return NULL;
}

static portal_node_t *add_node(const uint8_t *mac) {
    if (state.node_count >= PORTAL_MAX_NODES) return NULL;
    portal_node_t *n = &state.nodes[state.node_count++];
    memcpy(n->mac, mac, 6);
    return n;
}

void portal_state_update_position(const uint8_t *mac, float x, float y, float z) {
    portal_node_t *node = find_node(mac);
    if (!node) node = add_node(mac);
    if (!node) return;

    node->x = x; node->y = y; node->z = z;
    node->last_seen_us = esp_timer_get_time();
    node->stale = false;

    if (memcmp(mac, state.self_mac, 6) == 0) {
        adf_pipeline_set_position(adf_pipeline_get_latest_pipeline(), x, y, z);
    }
}

void portal_state_update_self(void) {
    portal_node_t *node = find_node(state.self_mac);
    if (!node) node = add_node(state.self_mac);
    if (!node) return;

    node->is_root = esp_mesh_is_root() ? 1 : 0;
    node->layer = esp_mesh_get_layer();
    node->rssi = network_get_rssi();
    node->children_count = network_get_connected_nodes();
    node->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    node->last_seen_us = esp_timer_get_time();
    node->stale = false;
}

void portal_state_expire_stale(void) {
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < state.node_count; i++) {
        if (memcmp(state.nodes[i].mac, state.self_mac, 6) == 0) continue;
        if ((now_us - state.nodes[i].last_seen_us) / 1000 > PORTAL_EXPIRE_TIMEOUT_MS) {
            if (i < state.node_count - 1) {
                memmove(&state.nodes[i], &state.nodes[i+1], (state.node_count - i - 1) * sizeof(portal_node_t));
            }
            state.node_count--;
            i--;
        }
    }
}

int portal_state_serialize_json(char *buf, size_t buf_size) {
    portal_state_update_self();
    portal_state_expire_stale();
    
    adf_pipeline_handle_t p = adf_pipeline_get_latest_pipeline();
    adf_input_mode_t mode = adf_pipeline_get_input_mode(p);
    
    return snprintf(buf, buf_size, 
        "{\"ts\":%llu,\"nodes\":%d,\"mode\":\"%s\",\"heap\":%lu}",
        (unsigned long long)(esp_timer_get_time()/1000),
        state.node_count,
        (mode == ADF_INPUT_MODE_USB) ? "USB" : (mode == ADF_INPUT_MODE_TONE) ? "TONE" : "AUX",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

const portal_state_t *portal_state_get(void) { return &state; }
void portal_state_expire_all(void) { state.node_count = 0; }
void portal_state_update_core0_load(void) {}
void portal_state_recompute_derived_fields(void) {}
void portal_state_sync_with_routing_table(void) {}
