#include "mesh/mesh_ping.h"
#include "mesh/mesh_state.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <string.h>

static const char *TAG = "network_mesh";

static void send_pong(const mesh_addr_t *dest, uint32_t ping_id) {
    mesh_ping_t pong;
    pong.type = NET_PKT_TYPE_PONG;
    pong.reserved[0] = 0;
    pong.reserved[1] = 0;
    pong.reserved[2] = 0;
    pong.ping_id = htonl(ping_id);

    mesh_data_t mesh_data = {
        .data = (uint8_t *)&pong,
        .size = sizeof(pong),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF,
    };

    esp_err_t err;
    if (is_mesh_root) {
        err = esp_mesh_send(dest, &mesh_data, MESH_DATA_P2P, NULL, 0);
    } else if (have_root_addr) {
        err = esp_mesh_send(&cached_root_addr, &mesh_data, MESH_DATA_P2P, NULL, 0);
    } else {
        ESP_LOGW(TAG, "Cannot send pong: no root addr");
        return;
    }
    ESP_LOGI(TAG, "PONG sent (root=%d, id=%lu): %s", is_mesh_root, ping_id, esp_err_to_name(err));
}

void mesh_ping_handle_ping(const mesh_addr_t *from, const mesh_ping_t *ping) {
    uint32_t id = ntohl(ping->ping_id);
    ESP_LOGI(TAG, "PING from " MACSTR " id=%lu (root=%d)", MAC2STR(from->addr), id, is_mesh_root);
    send_pong(from, id);
}

void mesh_ping_handle_pong(const mesh_ping_t *pong) {
    int64_t now_us = esp_timer_get_time();
    uint32_t id = ntohl(pong->ping_id);

    if (ping_pending && id == pending_ping_id) {
        int64_t rtt_us = now_us - last_ping_sent_us;
        measured_latency_ms = (uint32_t)(rtt_us / 2000);
        ping_pending = false;
        ESP_LOGI(TAG, "Ping RTT: %lld us → %lu ms", rtt_us, measured_latency_ms);
    } else if (child_ping_pending && id == pending_child_ping_id) {
        int64_t rtt_us = now_us - last_child_ping_sent_us;
        nearest_child_latency_ms = (uint32_t)(rtt_us / 2000);
        child_ping_pending = false;
        ESP_LOGI(TAG, "Child RTT: %lld us → %lu ms", rtt_us, nearest_child_latency_ms);
    } else {
        ESP_LOGW(TAG, "PONG unmatched id=%lu (expect ping=%lu child=%lu)", id, pending_ping_id, pending_child_ping_id);
    }
}

esp_err_t network_send_ping(void) {
    if (is_mesh_root || !is_mesh_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!have_root_addr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ping_pending) {
        int64_t elapsed_us = esp_timer_get_time() - last_ping_sent_us;
        if (elapsed_us > 2000000) {
            ping_pending = false;
        } else {
            return ESP_ERR_INVALID_STATE;
        }
    }

    ping_seq++;
    pending_ping_id = ping_seq;

    mesh_ping_t ping;
    ping.type = NET_PKT_TYPE_PING;
    ping.reserved[0] = 0;
    ping.reserved[1] = 0;
    ping.reserved[2] = 0;
    ping.ping_id = htonl(ping_seq);

    last_ping_sent_us = esp_timer_get_time();

    mesh_data_t mesh_data = {
        .data = (uint8_t *)&ping,
        .size = sizeof(ping),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF,
    };

    ping_pending = true;
    esp_err_t err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_TODS | MESH_DATA_NONBLOCK, NULL, 0);
    if (err != ESP_OK) {
        ping_pending = false;
        ESP_LOGW(TAG, "Ping send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PING sent to root id=%lu", ping_seq);
    }

    return err;
}

esp_err_t network_ping_nearest_child(void) {
    if (!is_mesh_root) {
        return ESP_ERR_INVALID_STATE;
    }

    if (child_ping_pending) {
        int64_t elapsed_us = esp_timer_get_time() - last_child_ping_sent_us;
        if (elapsed_us > 2000000) {
            child_ping_pending = false;
        } else {
            return ESP_ERR_INVALID_STATE;
        }
    }

    mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
    int route_size = 0;
    esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_size);

    mesh_addr_t *target = NULL;
    for (int i = 0; i < route_size; i++) {
        if (memcmp(&route_table[i], my_sta_mac, 6) != 0) {
            target = &route_table[i];
            break;
        }
    }
    if (!target) {
        return ESP_ERR_NOT_FOUND;
    }

    child_ping_seq++;
    pending_child_ping_id = child_ping_seq;

    mesh_ping_t ping;
    ping.type = NET_PKT_TYPE_PING;
    ping.reserved[0] = 0;
    ping.reserved[1] = 0;
    ping.reserved[2] = 0;
    ping.ping_id = htonl(child_ping_seq);

    last_child_ping_sent_us = esp_timer_get_time();

    mesh_data_t mesh_data = {
        .data = (uint8_t *)&ping,
        .size = sizeof(ping),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF,
    };

    child_ping_pending = true;
    esp_err_t err = esp_mesh_send(target, &mesh_data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        child_ping_pending = false;
        ESP_LOGW(TAG, "Child ping failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PING sent to child id=%lu", child_ping_seq);
    }

    return err;
}
