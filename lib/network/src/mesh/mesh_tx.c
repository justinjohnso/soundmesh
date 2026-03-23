#include "mesh/mesh_tx.h"
#include "mesh/mesh_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mesh.h>
#include <string.h>

static const char *TAG = "network_mesh";

esp_err_t network_send_audio(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        return ESP_ERR_INVALID_STATE;
    }

    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF,
    };

    esp_err_t err = ESP_OK;

    static uint16_t log_counter = 0;
    bool should_log = ((++log_counter & 0x7F) == 0);

    if (is_mesh_root) {
        mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
        int route_table_size = 0;
        esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
        int descendant_count = route_table_size > 1 ? route_table_size - 1 : 0;
        if (descendant_count <= 0) {
            return ESP_ERR_MESH_NO_ROUTE_FOUND;
        }

        err = esp_mesh_send((mesh_addr_t *)&audio_multicast_group, &mesh_data,
                            MESH_DATA_GROUP | MESH_DATA_NONBLOCK, NULL, 0);
        if (err == ESP_OK) {
            total_sent++;
            tx_bytes_counter += len;
        } else if (err == ESP_ERR_MESH_QUEUE_FULL) {
            total_drops++;
        }

        if (should_log) {
            ESP_LOGI(TAG, "Mesh TX GROUP: descendants=%d result=%s total_sent=%lu drops=%lu (%.1f%%)",
                     descendant_count, esp_err_to_name(err), total_sent, total_drops,
                     (total_sent + total_drops) > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f);
        }
    } else {
        err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_TODS | MESH_DATA_NONBLOCK, NULL, 0);
        if (err == ESP_OK) {
            total_sent++;
            tx_bytes_counter += len;
        }
    }

    return err;
}

esp_err_t network_send_control(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        return ESP_ERR_INVALID_STATE;
    }

    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF,
    };

    esp_err_t err = ESP_OK;
    if (is_mesh_root) {
        mesh_addr_t ctrl_route[MESH_ROUTE_TABLE_SIZE];
        int ctrl_route_size = 0;
        esp_mesh_get_routing_table(ctrl_route, sizeof(ctrl_route), &ctrl_route_size);
        int descendant_count = ctrl_route_size > 1 ? ctrl_route_size - 1 : 0;
        if (descendant_count <= 0) {
            return ESP_ERR_MESH_NO_ROUTE_FOUND;
        }
        int sent_ok = 0;
        esp_err_t first_err = ESP_OK;
        for (int i = 0; i < ctrl_route_size; i++) {
            if (memcmp(ctrl_route[i].addr, my_sta_mac, 6) == 0) {
                continue;
            }
            esp_err_t perr = esp_mesh_send(&ctrl_route[i], &mesh_data,
                                           MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
            if (perr == ESP_OK) {
                sent_ok++;
            } else if (first_err == ESP_OK) {
                first_err = perr;
            }
        }
        err = (sent_ok > 0) ? ESP_OK : (first_err != ESP_OK ? first_err : ESP_ERR_MESH_NO_ROUTE_FOUND);
        if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
            ESP_LOGD(TAG, "Control P2P send failed: %s", esp_err_to_name(err));
        }
    } else {
        if (have_root_addr) {
            err = esp_mesh_send(&cached_root_addr, &mesh_data, MESH_DATA_TODS | MESH_DATA_NONBLOCK, NULL, 0);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
    }

    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Control send failed: %s", esp_err_to_name(err));
    }

    return err;
}
