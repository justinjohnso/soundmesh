#include "control/portal_state.h"
#include "control/usb_portal.h"
#include "control/serial_dashboard.h"
#include "control/memory_monitor.h"
#include "audio/adf_pipeline.h"
#include "audio/usb_audio.h"
#include "config/build.h"
#include "config/build_role.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "portal_state";
static char s_monitor_json[4096];

static portal_state_t state;
static bool s_core0_load_valid = false;
static uint32_t s_core0_load_pct = 0;
#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
static bool s_core0_seeded = false;
static uint32_t s_last_total_runtime = 0;
static uint32_t s_last_core0_runtime = 0;
static int64_t s_last_core0_sample_us = 0;
#endif
static portal_node_t *find_node(const uint8_t *mac);
static portal_node_t *add_node(const uint8_t *mac);

static bool is_zero_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static uint8_t effective_layer_for(const portal_node_t *node) {
    if (!node) return 0;
    if (node->is_root) return 0;
    if (node->has_heartbeat) return node->layer;
    return 1;
}

static bool parent_exists(const uint8_t *parent_mac) {
    if (is_zero_mac(parent_mac)) {
        return false;
    }
    for (int i = 0; i < state.node_count; i++) {
        if (memcmp(state.nodes[i].mac, parent_mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void portal_state_recompute_derived_fields(void) {
    // Normalize parent links and infer missing layers for nodes discovered via routing table.
    for (int i = 0; i < state.node_count; i++) {
        portal_node_t *node = &state.nodes[i];
        if (!node->is_root && !is_zero_mac(node->parent_mac) && !parent_exists(node->parent_mac)) {
            memset(node->parent_mac, 0, sizeof(node->parent_mac));
        }
    }

    for (int pass = 0; pass < state.node_count; pass++) {
        bool changed = false;
        for (int i = 0; i < state.node_count; i++) {
            portal_node_t *node = &state.nodes[i];
            if (node->is_root || node->has_heartbeat) {
                continue;
            }

            if (is_zero_mac(node->parent_mac)) {
                continue;
            }

            portal_node_t *parent = find_node(node->parent_mac);
            if (!parent) {
                continue;
            }

            uint8_t parent_layer = effective_layer_for(parent);
            uint8_t inferred = (parent_layer < UINT8_MAX) ? (uint8_t)(parent_layer + 1) : UINT8_MAX;
            if (node->layer != inferred) {
                node->layer = inferred;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    // Compute child counts from known parent links so totals stay accurate.
    for (int i = 0; i < state.node_count; i++) {
        state.nodes[i].children_count = 0;
    }
    for (int i = 0; i < state.node_count; i++) {
        portal_node_t *node = &state.nodes[i];
        if (is_zero_mac(node->parent_mac)) {
            continue;
        }
        portal_node_t *parent = find_node(node->parent_mac);
        if (parent) {
            parent->children_count++;
        }
    }
}

static void portal_state_sync_with_routing_table(void) {
    mesh_addr_t route_table[PORTAL_MAX_NODES];
    int route_table_size = 0;
    esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
    if (route_table_size < 0) {
        route_table_size = 0;
    }
    if (route_table_size > PORTAL_MAX_NODES) {
        route_table_size = PORTAL_MAX_NODES;
    }

    uint8_t root_mac[6] = {0};
    for (int i = 0; i < state.node_count; i++) {
        if (state.nodes[i].is_root) {
            memcpy(root_mac, state.nodes[i].mac, sizeof(root_mac));
            break;
        }
    }
    if (is_zero_mac(root_mac)) {
        memcpy(root_mac, state.self_mac, sizeof(root_mac));
    }

    for (int i = 0; i < route_table_size; i++) {
        const uint8_t *mac = route_table[i].addr;
        portal_node_t *node = find_node(mac);
        if (!node) {
            node = add_node(mac);
            if (!node) {
                continue;
            }
            memset(node, 0, sizeof(*node));
            memcpy(node->mac, mac, 6);
            node->role = 0;
            node->rssi = -100;
            node->stream_active = 0;
            node->last_seen_us = esp_timer_get_time();
            node->stale = false;
        }

        bool is_self = (memcmp(mac, state.self_mac, 6) == 0);
        node->last_seen_us = esp_timer_get_time();
        node->stale = false;

        if (is_self) {
            continue;
        }

        if (!node->has_heartbeat) {
            node->is_root = 0;
            node->layer = 1;
            if (!is_zero_mac(root_mac) && memcmp(mac, root_mac, 6) != 0) {
                memcpy(node->parent_mac, root_mac, 6);
            } else {
                memset(node->parent_mac, 0, sizeof(node->parent_mac));
            }
        }
    }
}

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
    (void)sender_mac;
    portal_node_t *node = find_node(hb->self_mac);
    if (!node) {
        node = add_node(hb->self_mac);
        if (!node) return;
        memset(node, 0, sizeof(*node));
        memcpy(node->mac, hb->self_mac, 6);
        node->rssi = -100;
        ESP_LOGI(TAG, "New node: " MACSTR " role=%d", MAC2STR(hb->self_mac), hb->role);
    }
    
    node->has_heartbeat = 1;
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
        memset(node, 0, sizeof(*node));
        memcpy(node->mac, state.self_mac, 6);
        node->rssi = -100;
    }

    node->has_heartbeat = 1;
    #if BUILD_IS_SOURCE
        node->role = 1;  // SRC
    #else
        node->role = 0;  // OUT
    #endif

    node->is_root = esp_mesh_is_root() ? 1 : 0;
    node->layer = esp_mesh_get_layer();
    node->rssi = network_get_rssi();
    node->children_count = network_get_connected_nodes();
    node->stream_active = network_is_stream_ready() ? 1 : 0;
    memcpy(node->mac, state.self_mac, 6);
    if (node->is_root) {
        memset(node->parent_mac, 0, 6);
    }
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

static const char *portal_build_label(void) {
#if BUILD_IS_SOURCE
    return "SRC";
#elif BUILD_IS_OUTPUT
    return "OUT";
#else
    return "UNKNOWN";
#endif
}

static const char *portal_mesh_state(void) {
    return network_is_connected() ? "Mesh OK" : "Mesh Degraded";
}

static const char *portal_input_mode_label(adf_input_mode_t mode) {
    if (mode == ADF_INPUT_MODE_USB) {
        return "USB";
    }
    if (mode == ADF_INPUT_MODE_TONE) {
        return "TONE";
    }
    return "AUX";
}

static void portal_state_update_core0_load(void) {
#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
    const int64_t now_us = esp_timer_get_time();
    if (s_last_core0_sample_us != 0 && (now_us - s_last_core0_sample_us) < 1000000) {
        return;
    }
    s_last_core0_sample_us = now_us;

    const UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        s_core0_load_valid = false;
        return;
    }

    TaskStatus_t *tasks = calloc(task_count, sizeof(TaskStatus_t));
    if (!tasks) {
        s_core0_load_valid = false;
        return;
    }

    uint32_t total_runtime = 0;
    const UBaseType_t actual = uxTaskGetSystemState(tasks, task_count, &total_runtime);
    if (actual == 0 || total_runtime == 0) {
        free(tasks);
        s_core0_load_valid = false;
        return;
    }

    uint32_t core0_runtime = 0;
    for (UBaseType_t i = 0; i < actual; i++) {
        const uint32_t rt = tasks[i].ulRunTimeCounter;
        if (tasks[i].xCoreID == 0) {
            core0_runtime += rt;
#if (portNUM_PROCESSORS > 1)
        } else if (tasks[i].xCoreID == tskNO_AFFINITY) {
            core0_runtime += rt / portNUM_PROCESSORS;
#endif
        }
    }

    free(tasks);

    if (!s_core0_seeded) {
        s_last_total_runtime = total_runtime;
        s_last_core0_runtime = core0_runtime;
        s_core0_seeded = true;
        s_core0_load_valid = false;
        return;
    }

    const uint32_t delta_total = total_runtime - s_last_total_runtime;
    const uint32_t delta_core0 = core0_runtime - s_last_core0_runtime;
    s_last_total_runtime = total_runtime;
    s_last_core0_runtime = core0_runtime;

    if (delta_total == 0) {
        s_core0_load_valid = false;
        return;
    }

    float pct = (100.0f * ((float)delta_core0 * (float)portNUM_PROCESSORS)) / (float)delta_total;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    s_core0_load_pct = (uint32_t)(pct + 0.5f);
    s_core0_load_valid = true;
#else
    s_core0_load_valid = false;
#endif
}

int portal_state_serialize_json(char *buf, size_t buf_size) {
    portal_state_update_self();
    portal_state_sync_with_routing_table();
    portal_state_expire_stale();
    portal_state_recompute_derived_fields();
    portal_state_update_core0_load();
    
    char self_mac_str[18];
    mac_to_str(state.self_mac, self_mac_str);
    
    char core0_load_str[16];
    if (s_core0_load_valid) {
        snprintf(core0_load_str, sizeof(core0_load_str), "%lu", (unsigned long)s_core0_load_pct);
    } else {
        snprintf(core0_load_str, sizeof(core0_load_str), "null");
    }

    char netif_str[40] = "usb_ncm (n/a)";
    const esp_netif_ip_info_t *pip = portal_get_ip_info();
    if (pip) {
        snprintf(netif_str, sizeof(netif_str), "usb_ncm (" IPSTR ")", IP2STR(&pip->ip));
    }

    float fft_bins[FFT_PORTAL_BIN_COUNT] = {0};
    bool fft_valid = false;
    adf_pipeline_get_latest_fft_bins(fft_bins, FFT_PORTAL_BIN_COUNT, &fft_valid);

    // Get detailed memory stats
    memory_stats_t mem_stats;
    memory_monitor_get_stats(&mem_stats);

    int off = snprintf(
        buf,
        buf_size,
        "{\"schemaVersion\":%u,\"ts\":%lld,\"self\":\"%s\",\"heapKb\":%lu,"
        "\"memory\":{\"freeHeap\":%lu,\"largestBlock\":%lu,\"fragPct\":%u,\"minEverFree\":%lu},"
        "\"core0LoadPct\":%s,"
        "\"latencyMs\":%lu,\"netIf\":\"%s\",\"buildLabel\":\"%s\","
        "\"meshState\":\"%s\",\"bpm\":null,\"fftBins\":",
        (unsigned)PORTAL_STATUS_SCHEMA_VERSION,
        (long long)(esp_timer_get_time() / 1000),
        self_mac_str,
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
        (unsigned long)mem_stats.free_heap,
        (unsigned long)mem_stats.largest_block,
        (unsigned)mem_stats.fragmentation_pct,
        (unsigned long)mem_stats.min_ever_free_heap,
        core0_load_str,
        (unsigned long)network_get_latency_ms(),
        netif_str,
        portal_build_label(),
        portal_mesh_state());

    if (fft_valid) {
        off += snprintf(buf + off, buf_size - off, "[");
        for (int i = 0; i < FFT_PORTAL_BIN_COUNT && off < (int)buf_size - 32; i++) {
            if (i > 0) {
                off += snprintf(buf + off, buf_size - off, ",");
            }
            off += snprintf(buf + off, buf_size - off, "%.3f", (double)fft_bins[i]);
        }
        off += snprintf(buf + off, buf_size - off, "]");
    } else {
        off += snprintf(buf + off, buf_size - off, "null");
    }

    off += snprintf(buf + off, buf_size - off, ",\"nodes\":[");
    
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
            n->role == 1 ? "SRC" : "OUT",
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
    
    off += snprintf(buf + off, buf_size - off, "]");

    if (off < (int)buf_size - 64) {
        int mon_len = dashboard_serialize_recent_json(s_monitor_json, sizeof(s_monitor_json));
        if (mon_len > 12) {
            const char *payload = strchr(s_monitor_json, ':');
            if (payload) {
                payload++;  // points at '['
                const char *end = strrchr(payload, '}');
                if (end && end > payload) {
                    size_t arr_len = (size_t)(end - payload);
                    if (arr_len > buf_size - (size_t)off - 16) {
                        arr_len = buf_size - (size_t)off - 16;
                    }
                    off += snprintf(buf + off, buf_size - off, ",\"monitor\":");
                    memcpy(buf + off, payload, arr_len);
                    off += (int)arr_len;
                    buf[off] = '\0';
                }
            }
        }
    }

    if (off < (int)buf_size - 64) {
        off += snprintf(buf + off, buf_size - off, ",\"ota\":{\"enabled\":true,\"mode\":\"https\"}");
    }

    if (off < (int)buf_size - 256) {
        off += snprintf(
            buf + off,
            buf_size - off,
            ",\"transport\":{\"profile\":\"%s\",\"rootFanoutMode\":\"%s\",\"toRootMode\":\"%s\"}",
            TRANSPORT_SETTINGS_PROFILE_ID,
            TRANSPORT_ROOT_FANOUT_MODE,
            TRANSPORT_TO_ROOT_MODE);
    }

    if (off < (int)buf_size - 196) {
        network_uplink_status_t uplink = {0};
        if (network_get_uplink_status(&uplink) == ESP_OK) {
            off += snprintf(
                buf + off,
                buf_size - off,
                ",\"uplink\":{\"enabled\":%s,\"configured\":%s,\"rootApplied\":%s,"
                "\"pendingApply\":%s,\"ssid\":\"%s\",\"lastError\":\"%s\",\"updatedMs\":%lu}",
                uplink.enabled ? "true" : "false",
                uplink.configured ? "true" : "false",
                uplink.root_applied ? "true" : "false",
                uplink.pending_apply ? "true" : "false",
                uplink.configured ? "<configured>" : "",
                uplink.last_error,
                (unsigned long)uplink.updated_ms);
        }
    }

    if (off < (int)buf_size - 320) {
        network_mixer_status_t mixer = {0};
        if (network_get_mixer_status(&mixer) == ESP_OK) {
            off += snprintf(
                buf + off,
                buf_size - off,
                ",\"mixer\":{\"schemaVersion\":%u,\"outGainPct\":%u,\"streamCount\":%u,\"applied\":%s,\"pendingApply\":%s,"
                "\"lastError\":\"%s\",\"updatedMs\":%lu,\"streams\":[",
                (unsigned)mixer.schema_version,
                (unsigned)mixer.out_gain_pct,
                (unsigned)mixer.stream_count,
                mixer.applied ? "true" : "false",
                mixer.pending_apply ? "true" : "false",
                mixer.last_error,
                (unsigned long)mixer.updated_ms);
            for (uint8_t i = 0; i < mixer.stream_count && off < (int)buf_size - 128; i++) {
                const network_mixer_stream_status_t *stream = &mixer.streams[i];
                if (i > 0) {
                    off += snprintf(buf + off, buf_size - off, ",");
                }
                off += snprintf(
                    buf + off,
                    buf_size - off,
                    "{\"id\":%u,\"gainPct\":%u,\"enabled\":%s,\"muted\":%s,\"solo\":%s,\"active\":%s}",
                    (unsigned)stream->stream_id,
                    (unsigned)stream->gain_pct,
                    stream->enabled ? "true" : "false",
                    stream->muted ? "true" : "false",
                    stream->solo ? "true" : "false",
                    stream->active ? "true" : "false");
            }
            off += snprintf(buf + off, buf_size - off, "]}");
        }
    }

    if (off < (int)buf_size - 96) {
        off += snprintf(
            buf + off,
            buf_size - off,
            ",\"usb\":{\"inputMode\":\"%s\",\"ready\":%s,\"active\":%s}",
            portal_input_mode_label(adf_pipeline_get_input_mode()),
            usb_audio_is_ready() ? "true" : "false",
            usb_audio_is_active() ? "true" : "false");
    }

    off += snprintf(buf + off, buf_size - off, "}");
    
    return off;
}

const portal_state_t *portal_state_get(void) {
    return &state;
}
