#include "mesh/mesh_state.h"
#include <esp_log.h>

node_role_t my_node_role = NODE_ROLE_RX;
uint8_t my_stream_id = 1;
uint8_t my_sta_mac[6] = {0};
char g_src_id[NETWORK_SRC_ID_LEN] = "SRC_000000";

bool is_mesh_connected = false;
bool is_mesh_root = false;
bool is_mesh_root_ready = false;
uint8_t mesh_layer = 0;
int mesh_children_count = 0;
mesh_addr_t mesh_parent_addr;

uint32_t measured_latency_ms = 0;
bool ping_pending = false;
int8_t nearest_child_rssi = -100;
uint32_t nearest_child_latency_ms = 0;
mesh_addr_t nearest_child_addr;
bool child_ping_pending = false;
int64_t last_ping_sent_us = 0;
int64_t last_child_ping_sent_us = 0;

mesh_addr_t cached_root_addr;
bool have_root_addr = false;

uint32_t ping_seq = 0;
uint32_t child_ping_seq = 0;
uint32_t pending_ping_id = 0;
uint32_t pending_child_ping_id = 0;
uint32_t no_parent_count = 0;
uint32_t scan_done_count = 0;
uint32_t join_fail_count = 0;
uint32_t auth_expire_count = 0;
uint32_t recovery_restarts = 0;
uint32_t parent_conn_count = 0;
uint32_t parent_disc_count = 0;

network_uplink_status_t s_uplink = {
    .enabled = false,
    .configured = false,
    .root_applied = false,
    .pending_apply = false,
    .ssid = {0},
    .last_error = {0},
    .updated_ms = 0,
};
char s_uplink_password[UPLINK_PASSWORD_MAX_LEN + 1] = {0};

TaskHandle_t heartbeat_task_handle = NULL;
TaskHandle_t waiting_task_handles[2] = {NULL, NULL};
int waiting_task_count = 0;

network_audio_callback_t audio_rx_callback = NULL;
network_heartbeat_callback_t heartbeat_rx_callback = NULL;

uint32_t total_drops = 0;
uint32_t total_sent = 0;
volatile uint32_t tx_bytes_counter = 0;

uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE];
const mesh_addr_t audio_multicast_group = {
    .addr = {0x01, 0x00, 0x5E, 'A', 'U', 'D'}
};

void mesh_state_notify_waiting_tasks(void) {
    for (int i = 0; i < waiting_task_count; i++) {
        if (waiting_task_handles[i] != NULL) {
            xTaskNotifyGive(waiting_task_handles[i]);
        }
    }
}

esp_err_t network_register_startup_notification(TaskHandle_t task_handle) {
    static const char *TAG = "network_mesh";
    if (waiting_task_count >= 2) {
        return ESP_ERR_NO_MEM;
    }

    waiting_task_handles[waiting_task_count] = task_handle;
    waiting_task_count++;

    ESP_LOGD(TAG, "Task registered for startup notification (count=%d)", waiting_task_count);

    if (is_mesh_root_ready) {
        xTaskNotifyGive(task_handle);
        ESP_LOGD(TAG, "Network already ready - notifying immediately");
    }

    return ESP_OK;
}

esp_err_t network_register_audio_callback(network_audio_callback_t callback) {
    static const char *TAG = "network_mesh";
    audio_rx_callback = callback;
    ESP_LOGI(TAG, "Audio callback registered");
    return ESP_OK;
}

esp_err_t network_register_heartbeat_callback(network_heartbeat_callback_t callback) {
    static const char *TAG = "network_mesh";
    heartbeat_rx_callback = callback;
    ESP_LOGI(TAG, "Heartbeat callback registered");
    return ESP_OK;
}
