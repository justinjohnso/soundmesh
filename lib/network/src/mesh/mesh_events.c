#include "mesh/mesh_events.h"
#include "mesh/mesh_state.h"
#include "mesh/mesh_uplink.h"
#include "mesh/mesh_mixer.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mesh.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>

static const char *TAG = "network_mesh";

static bool should_skip_wifi_runtime_ops(const char *op_name) {
    if (mesh_self_organized_mode && mesh_runtime_started) {
        ESP_LOGW(TAG, "Skipping %s while self-organized mesh runtime is active", op_name);
        return true;
    }
    return false;
}

static const char *wifi_disconnect_reason_to_str(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_TIMEOUT";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        default: return "OTHER";
    }
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            mesh_runtime_started = true;
            if (my_node_role == NODE_ROLE_OUT) {
                ESP_LOGI(TAG, "OUT discovery active: waiting for root on channel=%d mesh_id=%s src_id=%s",
                         MESH_CHANNEL, MESH_ID, g_src_id);
            }
            if (my_node_role == NODE_ROLE_SRC && esp_mesh_is_root()) {
                is_mesh_root = true;
                is_mesh_root_ready = true;
                mesh_layer = 0;
                ESP_LOGI(TAG, "Designated root ready: mesh AP broadcasting on channel %d", MESH_CHANNEL);
                mesh_state_notify_waiting_tasks();
                esp_log_level_set("wifi", ESP_LOG_ERROR);
            }
            break;

        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "Mesh stopped");
            is_mesh_connected = false;
            mesh_runtime_started = false;
            break;

        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            memcpy(mesh_parent_addr.addr, connected->connected.bssid, 6);
            is_mesh_connected = true;
            is_mesh_root_ready = true;
            mesh_layer = esp_mesh_get_layer();
            auth_expire_count = 0;
            parent_conn_count++;
            g_transport_stats.parent_connect_events++;

            ESP_LOGI(TAG, "Parent connected, layer: %d (stream ready)", mesh_layer);
            ESP_LOGI(TAG, "Parent BSSID: " MACSTR, MAC2STR(connected->connected.bssid));

            mesh_state_notify_waiting_tasks();

            if (esp_mesh_is_root()) {
                if (!should_skip_wifi_runtime_ops("esp_wifi_scan_stop")) {
                    esp_wifi_scan_stop();
                    ESP_LOGI(TAG, "Root connected: scan stopped");
                }
            } else {
                ESP_LOGI(TAG, "OUT connected: preserving startup self-organized config");
                mesh_uplink_request_sync_from_root();
                mesh_mixer_request_sync_from_root();
            }
            break;
        }

        case MESH_EVENT_PARENT_DISCONNECTED: {
            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
            if (!esp_mesh_is_root()) {
                bool was_connected = is_mesh_connected;
                parent_disc_count++;
                g_transport_stats.parent_disconnect_events++;
                ESP_LOGW(TAG, "Parent disconnected: reason=%d(%s), was_connected=%d, layer=%d",
                         disconnected->reason,
                         wifi_disconnect_reason_to_str(disconnected->reason),
                         was_connected, esp_mesh_get_layer());
                is_mesh_connected = false;
                mesh_state_clear_root_addr();
                if (!was_connected) {
                    join_fail_count++;
                    if (disconnected->reason == WIFI_REASON_AUTH_EXPIRE) {
                        auth_expire_count++;
                    }
                    ESP_LOGI(TAG, "Join attempt failed before parent connect; continuing auto-join without forced reset");
                    if ((join_fail_count % 10) == 0) {
                        ESP_LOGW(TAG, "OUT join still failing (count=%lu)", (unsigned long)join_fail_count);
                    }
                    if (auth_expire_count >= 12) {
                        ESP_LOGW(TAG, "AUTH_EXPIRE persists (count=%lu), keeping native mesh retry path",
                                 (unsigned long)auth_expire_count);
                        auth_expire_count = 0;
                    }
                } else {
                    ESP_LOGI(TAG, "Connected parent lost; waiting for native mesh reconnect");
                }
            }
            break;
        }

        case MESH_EVENT_CHILD_CONNECTED: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Child connected (routing table: %d)", new_count);
            mesh_children_count = new_count;
            if (is_mesh_root) {
                if (!should_skip_wifi_runtime_ops("esp_wifi_scan_stop + esp_wifi_disconnect")) {
                    esp_wifi_scan_stop();
                    esp_err_t disc_err = esp_wifi_disconnect();
                    if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
                        ESP_LOGW(TAG, "Root STA disconnect failed: %s", esp_err_to_name(disc_err));
                    }
                    ESP_LOGI(TAG, "Root: child connected, scan stopped and STA client disconnected");
                }
            }
            break;
        }

        case MESH_EVENT_CHILD_DISCONNECTED: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Child disconnected (routing table: %d)", new_count);
            mesh_children_count = new_count;
            break;
        }

        case MESH_EVENT_ROUTING_TABLE_ADD:
        case MESH_EVENT_ROUTING_TABLE_REMOVE: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Routing table changed: %d entries (descendants: %d)",
                     new_count, new_count > 0 ? new_count - 1 : 0);
            mesh_children_count = new_count;
            break;
        }

        case MESH_EVENT_ROOT_FIXED:
            if (esp_mesh_is_root()) {
                ESP_LOGI(TAG, "Became mesh root (role=%s)", my_node_role == NODE_ROLE_SRC ? "SRC" : "OUT");
                is_mesh_root = true;
                mesh_layer = 0;
                is_mesh_root_ready = true;
                (void)mesh_uplink_apply_router_config();
                ESP_LOGI(TAG, "Root ready: mesh AP broadcasting on channel %d", MESH_CHANNEL);
                mesh_state_notify_waiting_tasks();
            } else {
                ESP_LOGI(TAG, "Joined mesh with fixed root (we are not root)");
            }
            break;

        case MESH_EVENT_ROOT_ADDRESS: {
            mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
            mesh_addr_t tmp = {0};
            memcpy(tmp.addr, root_addr->addr, sizeof(tmp.addr));
            mesh_state_set_root_addr(&tmp);
            ESP_LOGI(TAG, "Root addr cached: " MACSTR, MAC2STR(root_addr->addr));
            break;
        }

        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
            ESP_LOGI(TAG, "ToDS state: %d", *toDs_state);
            break;
        }

        case MESH_EVENT_ROOT_SWITCH_REQ:
            ESP_LOGI(TAG, "Root switch requested");
            is_mesh_root = esp_mesh_is_root();
            break;

        case MESH_EVENT_ROOT_SWITCH_ACK:
            ESP_LOGI(TAG, "Root switch acknowledged");
            is_mesh_root = esp_mesh_is_root();
            is_mesh_root_ready = is_mesh_root;
            if (is_mesh_root) {
                mesh_state_notify_waiting_tasks();
                ESP_LOGI(TAG, "Now acting as mesh root");
            } else {
                ESP_LOGI(TAG, "No longer mesh root");
            }
            break;

        case MESH_EVENT_NO_PARENT_FOUND:
            no_parent_count++;
            g_transport_stats.no_parent_events++;
            ESP_LOGW(TAG, "No parent found (attempt=%lu) — retrying scan (channel=%d, mesh_id=%s, src_id=%s)",
                     (unsigned long)no_parent_count, MESH_CHANNEL, MESH_ID, g_src_id);
            break;

        case MESH_EVENT_FIND_NETWORK: {
            mesh_event_find_network_t *evt = (mesh_event_find_network_t *)event_data;
            if (evt->channel != MESH_CHANNEL) {
                ESP_LOGW(TAG, "Found mesh on unexpected channel %d (expected %d) - join in progress",
                         evt->channel, MESH_CHANNEL);
            } else {
                ESP_LOGI(TAG, "Found network on channel %d - join in progress", evt->channel);
            }
            break;
        }

        case MESH_EVENT_SCAN_DONE: {
            mesh_event_scan_done_t *scan = (mesh_event_scan_done_t *)event_data;
            scan_done_count++;
            g_transport_stats.scan_done_events++;
            if (my_node_role == NODE_ROLE_OUT) {
                ESP_LOGI(TAG, "Scan done #%lu: found %d APs (connected=%d, layer=%d)",
                         (unsigned long)scan_done_count, (int)scan->number, is_mesh_connected, esp_mesh_get_layer());
            } else {
                ESP_LOGD(TAG, "Scan done: found %d APs", (int)scan->number);
            }
            break;
        }

        case MESH_EVENT_LAYER_CHANGE: {
            mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
            mesh_layer = layer_change->new_layer;
            ESP_LOGI(TAG, "Layer changed: %d", mesh_layer);
            break;
        }

        default:
            ESP_LOGD(TAG, "Mesh event: %ld", event_id);
            break;
    }
}
