#include "mesh/mesh_heartbeat.h"
#include "mesh/mesh_state.h"
#include "network/mesh_net.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

static const char *TAG = "network_mesh";

static void send_heartbeat(void) {
    if (is_mesh_root) {
        return;
    }

    mesh_heartbeat_t heartbeat;
    heartbeat.type = NET_PKT_TYPE_HEARTBEAT;
    heartbeat.role = my_node_role;
    heartbeat.is_root = is_mesh_root ? 1 : 0;
    heartbeat.layer = mesh_layer;
    heartbeat.uptime_ms = esp_timer_get_time() / 1000;
    heartbeat.children_count = mesh_children_count;
    heartbeat.rssi = network_get_rssi();
    heartbeat.stream_active = (is_mesh_connected || is_mesh_root_ready) ? 1 : 0;
    memcpy(heartbeat.self_mac, my_sta_mac, 6);
    heartbeat.parent_conn_count = htons((uint16_t)(parent_conn_count & 0xFFFF));
    heartbeat.parent_disc_count = htons((uint16_t)(parent_disc_count & 0xFFFF));
    heartbeat.auth_expire_count = htons((uint16_t)(auth_expire_count & 0xFFFF));
    heartbeat.no_parent_count = htons((uint16_t)(no_parent_count & 0xFFFF));
    memcpy(heartbeat.src_id, g_src_id, NETWORK_SRC_ID_LEN);
    if (is_mesh_root) {
        memset(heartbeat.parent_mac, 0, 6);
    } else {
        memcpy(heartbeat.parent_mac, mesh_parent_addr.addr, 6);
    }

    static uint32_t hb_count = 0;
    hb_count++;
    if ((hb_count % 5) == 1) {
        int rt_size = esp_mesh_get_routing_table_size();
        ESP_LOGI(TAG, "Heartbeat #%lu: root=%d, connected=%d, route_table=%d, children=%d churn(pc=%lu pd=%lu ae=%lu np=%lu)",
                 hb_count, is_mesh_root, is_mesh_connected, rt_size, mesh_children_count,
                 (unsigned long)parent_conn_count, (unsigned long)parent_disc_count,
                 (unsigned long)auth_expire_count, (unsigned long)no_parent_count);
    }

    esp_err_t err = network_send_control((uint8_t *)&heartbeat, sizeof(heartbeat));
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Failed to send heartbeat: %s", esp_err_to_name(err));
    }
}

static void send_stream_announcement(void) {
    if (my_node_role != NODE_ROLE_TX) {
        return;
    }

    if (is_mesh_root) {
        return;
    }

    mesh_stream_announce_t announce;
    announce.type = NET_PKT_TYPE_STREAM_ANNOUNCE;
    announce.stream_id = my_stream_id;
    announce.sample_rate = htonl(AUDIO_SAMPLE_RATE);
    announce.channels = AUDIO_CHANNELS_MONO;
    announce.bits_per_sample = AUDIO_BITS_PER_SAMPLE;
    announce.frame_size_ms = htons(AUDIO_FRAME_MS);

    esp_err_t err = network_send_control((uint8_t *)&announce, sizeof(announce));
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Failed to send stream announcement: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Stream announced: ID=%u, %uHz, %u-bit, %uch, %ums frames",
                 announce.stream_id, (unsigned int)AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE,
                 AUDIO_CHANNELS_MONO, (unsigned int)AUDIO_FRAME_MS);
    }
}

void mesh_heartbeat_task(void *arg) {
    (void)arg;
    const uint32_t HEARTBEAT_INTERVAL_MS = 5000;
    const uint32_t RX_RECOVERY_INTERVAL_MS = 120000;

    ESP_LOGI(TAG, "Heartbeat task started (will send once network is ready)");

    uint32_t waited_ms = 0;
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
        waited_ms += 1000;
        if (my_node_role == NODE_ROLE_RX && (waited_ms % 5000) == 0) {
            ESP_LOGI(TAG, "Still waiting for mesh ready (%lus): connected=%d root_ready=%d no_parent=%lu scans=%lu",
                     (unsigned long)(waited_ms / 1000), is_mesh_connected, is_mesh_root_ready,
                     (unsigned long)no_parent_count, (unsigned long)scan_done_count);
        }
        if (my_node_role == NODE_ROLE_RX &&
            waited_ms >= RX_RECOVERY_INTERVAL_MS &&
            (waited_ms % RX_RECOVERY_INTERVAL_MS) == 0 &&
            recovery_restarts < 1) {
            recovery_restarts++;
            ESP_LOGW(TAG, "RX join recovery #%lu: forcing reconnect after %lus wait",
                     (unsigned long)recovery_restarts, (unsigned long)(waited_ms / 1000));
        }
    }
    ESP_LOGI(TAG, "Network ready - sending heartbeats");

    send_stream_announcement();

    while (1) {
        send_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}
