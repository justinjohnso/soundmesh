#pragma once

#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_mesh.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Shared runtime state owned by mesh_state.c

typedef enum {
    NODE_ROLE_RX = 0,
    NODE_ROLE_TX = 1
} node_role_t;

extern node_role_t my_node_role;
extern uint8_t my_stream_id;
extern uint8_t my_sta_mac[6];
extern char g_src_id[NETWORK_SRC_ID_LEN];

extern bool is_mesh_connected;
extern bool is_mesh_root;
extern bool is_mesh_root_ready;
extern uint8_t mesh_layer;
extern int mesh_children_count;
extern mesh_addr_t mesh_parent_addr;

extern uint32_t measured_latency_ms;
extern bool ping_pending;
extern int8_t nearest_child_rssi;
extern uint32_t nearest_child_latency_ms;
extern mesh_addr_t nearest_child_addr;
extern bool child_ping_pending;
extern int64_t last_ping_sent_us;
extern int64_t last_child_ping_sent_us;

extern mesh_addr_t cached_root_addr;
extern bool have_root_addr;

extern uint32_t ping_seq;
extern uint32_t child_ping_seq;
extern uint32_t pending_ping_id;
extern uint32_t pending_child_ping_id;
extern uint32_t no_parent_count;
extern uint32_t scan_done_count;
extern uint32_t join_fail_count;
extern uint32_t auth_expire_count;
extern uint32_t recovery_restarts;
extern uint32_t parent_conn_count;
extern uint32_t parent_disc_count;

extern network_uplink_status_t s_uplink;
extern char s_uplink_password[UPLINK_PASSWORD_MAX_LEN + 1];

extern TaskHandle_t heartbeat_task_handle;
extern TaskHandle_t waiting_task_handles[2];
extern int waiting_task_count;

extern network_audio_callback_t audio_rx_callback;
extern network_heartbeat_callback_t heartbeat_rx_callback;

extern uint32_t total_drops;
extern uint32_t total_sent;
extern volatile uint32_t tx_bytes_counter;

extern uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE];
extern const mesh_addr_t audio_multicast_group;

void mesh_state_notify_waiting_tasks(void);
