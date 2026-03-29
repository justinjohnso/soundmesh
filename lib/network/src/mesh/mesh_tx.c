#include "mesh/mesh_tx.h"
#include "mesh/mesh_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mesh.h>
#include <string.h>

static const char *TAG = "network_mesh";
static const int kAudioRootFanoutFlags = MESH_DATA_GROUP | MESH_DATA_NONBLOCK;
static const int kAudioToRootFlags = MESH_DATA_TODS | MESH_DATA_NONBLOCK;

static uint8_t transport_backpressure_level(uint32_t queue_full_streak)
{
    if (queue_full_streak >= 32) {
        return 3;
    }
    if (queue_full_streak >= 8) {
        return 2;
    }
    if (queue_full_streak >= 2) {
        return 1;
    }
    return 0;
}

static void transport_record_audio_tx_result(esp_err_t err, size_t len)
{
    static uint32_t queue_full_streak = 0;
    if (err == ESP_OK) {
        g_transport_stats.tx_audio_packets++;
        g_transport_stats.tx_audio_bytes += (uint32_t)len;
        if (queue_full_streak > 0) {
            queue_full_streak--;
        }
    } else {
        g_transport_stats.tx_audio_send_failures++;
        if (err == ESP_ERR_MESH_QUEUE_FULL) {
            g_transport_stats.tx_audio_queue_full++;
            queue_full_streak++;
        } else if (err == ESP_ERR_MESH_NO_ROUTE_FOUND) {
            g_transport_stats.tx_audio_no_route++;
        } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_MESH_DISCONNECTED) {
            g_transport_stats.tx_audio_invalid_state++;
        }
    }
    g_transport_stats.tx_audio_backpressure_level = transport_backpressure_level(queue_full_streak);
}

static void transport_record_control_tx_result(esp_err_t err)
{
    if (err == ESP_OK) {
        g_transport_stats.tx_control_packets++;
        return;
    }
    g_transport_stats.tx_control_send_failures++;
    if (err == ESP_ERR_MESH_NO_ROUTE_FOUND) {
        g_transport_stats.tx_control_no_route++;
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_MESH_DISCONNECTED) {
        g_transport_stats.tx_control_invalid_state++;
    }
}

esp_err_t network_send_audio(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        transport_record_audio_tx_result(ESP_ERR_INVALID_STATE, len);
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
            transport_record_audio_tx_result(ESP_ERR_MESH_NO_ROUTE_FOUND, len);
            return ESP_ERR_MESH_NO_ROUTE_FOUND;
        }

        err = esp_mesh_send((mesh_addr_t *)&audio_multicast_group, &mesh_data,
                            kAudioRootFanoutFlags, NULL, 0);
        transport_record_audio_tx_result(err, len);
        if (err == ESP_OK) {
            total_sent++;
            tx_bytes_counter += len;
        } else if (err == ESP_ERR_MESH_QUEUE_FULL) {
            total_drops++;
        }

        if (should_log) {
            ESP_LOGI(TAG, "Mesh TX %s: descendants=%d result=%s total_sent=%lu drops=%lu (%.1f%%)",
                     TRANSPORT_ROOT_FANOUT_MODE, descendant_count, esp_err_to_name(err), total_sent, total_drops,
                     (total_sent + total_drops) > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f);
            ESP_LOGI(TAG,
                     "TX OBS: audio_ok=%lu fail=%lu qfull=%lu noroute=%lu inv=%lu bp=%lu",
                     (unsigned long)g_transport_stats.tx_audio_packets,
                     (unsigned long)g_transport_stats.tx_audio_send_failures,
                     (unsigned long)g_transport_stats.tx_audio_queue_full,
                     (unsigned long)g_transport_stats.tx_audio_no_route,
                     (unsigned long)g_transport_stats.tx_audio_invalid_state,
                     (unsigned long)g_transport_stats.tx_audio_backpressure_level);
        }
    } else {
        err = esp_mesh_send(NULL, &mesh_data, kAudioToRootFlags, NULL, 0);
        transport_record_audio_tx_result(err, len);
        if (err == ESP_OK) {
            total_sent++;
            tx_bytes_counter += len;
        }
    }

    return err;
}

esp_err_t network_send_control(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        transport_record_control_tx_result(ESP_ERR_INVALID_STATE);
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
            transport_record_control_tx_result(ESP_ERR_MESH_NO_ROUTE_FOUND);
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
        transport_record_control_tx_result(err);
        if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
            ESP_LOGD(TAG, "Control P2P send failed: %s", esp_err_to_name(err));
        }
    } else {
        const mesh_addr_t *root_addr = mesh_state_get_root_addr();
        if (root_addr) {
            err = esp_mesh_send(root_addr, &mesh_data, kAudioToRootFlags, NULL, 0);
            transport_record_control_tx_result(err);
        } else {
            err = ESP_ERR_INVALID_STATE;
            transport_record_control_tx_result(err);
        }
    }

    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Control send failed: %s", esp_err_to_name(err));
    }

    return err;
}
