#include "mesh/mesh_queries.h"
#include "mesh/mesh_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mesh.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <string.h>

static const char *TAG = "network_mesh";

bool network_is_root(void) {
    return esp_mesh_is_root();
}

uint8_t network_get_layer(void) {
    return esp_mesh_get_layer();
}

uint32_t network_get_children_count(void) {
    return esp_mesh_get_routing_table_size();
}

int network_get_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -100;
}

uint32_t network_get_latency_ms(void) {
    return measured_latency_ms;
}

uint8_t network_get_jitter_prefill_frames(void) {
    uint8_t base = JITTER_PREFILL_FRAMES;
    uint8_t extra = 0;

    if (mesh_layer > 1) {
        extra += (mesh_layer - 1);
    }

    uint32_t nodes = network_get_connected_nodes();
    if (nodes >= 3) {
        extra += 1;
    }

    if (measured_latency_ms > 50) {
        extra += 1;
    }

    uint8_t result = base + extra;
    if (result > JITTER_BUFFER_FRAMES) {
        result = JITTER_BUFFER_FRAMES;
    }

    return result;
}

bool network_is_connected(void) {
    return is_mesh_connected || is_mesh_root;
}

bool network_is_stream_ready(void) {
    return is_mesh_connected || (is_mesh_root && is_mesh_root_ready);
}

bool network_rejoin_allowed(void) {
    if (is_mesh_root) {
        return false;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return now_ms >= rejoin_cooldown_until_ms;
}

esp_err_t network_trigger_rejoin(void) {
    if (is_mesh_root) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms < rejoin_cooldown_until_ms) {
        ESP_LOGW(TAG, "Rejoin blocked by cooldown (%lums remaining)",
                 (unsigned long)(rejoin_cooldown_until_ms - now_ms));
        return ESP_ERR_INVALID_STATE;
    }
    if (rejoin_window_start_ms == 0 || (now_ms - rejoin_window_start_ms) > OUT_REJOIN_WINDOW_MS) {
        rejoin_window_start_ms = now_ms;
        rejoin_attempt_count = 0;
    }
    if (rejoin_attempt_count >= OUT_REJOIN_MAX_ATTEMPTS) {
        rejoin_cooldown_until_ms = now_ms + OUT_REJOIN_COOLDOWN_MS;
        ESP_LOGW(TAG, "Rejoin circuit breaker tripped (attempts=%lu, cooldown=%lums)",
                 (unsigned long)rejoin_attempt_count, (unsigned long)OUT_REJOIN_COOLDOWN_MS);
        return ESP_ERR_INVALID_STATE;
    }
    rejoin_attempt_count++;

    ESP_LOGW(TAG, "Triggering OUT rejoin: disconnect + reconnect");
    mesh_state_clear_root_addr();
    is_mesh_connected = false;
    esp_err_t derr = esp_mesh_disconnect();
    if (derr != ESP_OK && derr != ESP_ERR_MESH_DISCONNECTED) {
        ESP_LOGW(TAG, "esp_mesh_disconnect failed: %s", esp_err_to_name(derr));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t cerr = esp_mesh_connect();
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "esp_mesh_connect failed: %s", esp_err_to_name(cerr));
        return cerr;
    }
    ESP_LOGI(TAG, "Rejoin attempt %lu/%d in current window",
             (unsigned long)rejoin_attempt_count, OUT_REJOIN_MAX_ATTEMPTS);
    return ESP_OK;
}

uint32_t network_get_connected_nodes(void) {
    int size = esp_mesh_get_routing_table_size();
    return (size > 1) ? (uint32_t)(size - 1) : 0;
}

uint32_t network_get_tx_bytes_and_reset(void) {
    uint32_t bytes = tx_bytes_counter;
    tx_bytes_counter = 0;
    return bytes;
}

int network_get_nearest_child_rssi(void) {
    if (!is_mesh_root) return -100;

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num == 0) {
        return -100;
    }

    int8_t best_rssi = -100;
    for (int i = 0; i < sta_list.num; i++) {
        if (sta_list.sta[i].rssi > best_rssi) {
            best_rssi = sta_list.sta[i].rssi;
            memcpy(nearest_child_addr.addr, sta_list.sta[i].mac, 6);
        }
    }
    nearest_child_rssi = best_rssi;
    return nearest_child_rssi;
}

uint32_t network_get_nearest_child_latency_ms(void) {
    return nearest_child_latency_ms;
}
