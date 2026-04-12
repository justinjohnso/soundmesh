#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <unity.h>

// Include the source directly so we can test it
#include "../../../lib/network/src/uplink_control.c"
#include "../../../lib/network/src/mesh/mesh_uplink.c"

node_role_t my_node_role = NODE_ROLE_OUT;
uint8_t my_stream_id = 0;
uint8_t my_sta_mac[6] = {0};
char g_src_id[NETWORK_SRC_ID_LEN] = {0};

bool is_mesh_connected = false;
bool is_mesh_root = false;
bool is_mesh_root_ready = false;
bool mesh_self_organized_mode = false;
bool mesh_runtime_started = false;
uint8_t mesh_layer = 0;
int mesh_children_count = 0;
mesh_addr_t mesh_parent_addr = {{0}};

uint32_t measured_latency_ms = 0;
int8_t mesh_parent_rssi = -100;
bool ping_pending = false;
int8_t nearest_child_rssi = -100;
uint32_t nearest_child_latency_ms = 0;
mesh_addr_t nearest_child_addr = {{0}};
bool child_ping_pending = false;
int64_t last_ping_sent_us = 0;
int64_t last_child_ping_sent_us = 0;

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
uint32_t rejoin_attempt_count = 0;
uint32_t rejoin_window_start_ms = 0;
uint32_t rejoin_cooldown_until_ms = 0;

network_uplink_status_t s_uplink = {0};
char s_uplink_password[UPLINK_PASSWORD_MAX_LEN + 1] = {0};

network_mixer_status_t s_mixer = {0};
network_mixer_apply_callback_t mixer_apply_callback = NULL;

TaskHandle_t heartbeat_task_handle = NULL;
TaskHandle_t waiting_task_handles[2] = {0};
int waiting_task_count = 0;

network_audio_callback_t audio_rx_callback = NULL;
network_heartbeat_callback_t heartbeat_rx_callback = NULL;

uint32_t total_drops = 0;
uint32_t total_sent = 0;
volatile uint32_t tx_bytes_counter = 0;
network_transport_stats_t g_transport_stats = {0};

uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE] = {0};
const mesh_addr_t audio_multicast_group = {{0}};

static int stub_set_router_calls = 0;
static int stub_send_control_calls = 0;
static esp_err_t stub_set_router_result = ESP_OK;
static esp_err_t stub_send_control_result = ESP_OK;
static int64_t stub_time_us = 0;
static uplink_ctrl_packet_t stub_last_control_packet = {0};

esp_err_t esp_mesh_set_router(const mesh_router_t *router)
{
    (void)router;
    stub_set_router_calls++;
    return stub_set_router_result;
}

esp_err_t network_send_control(const uint8_t *data, size_t len)
{
    printf("DEBUG: STUB network_send_control called! calls now=%d\n", stub_send_control_calls + 1);
    stub_send_control_calls++;
    if (data && len == sizeof(stub_last_control_packet)) {
        memcpy(&stub_last_control_packet, data, sizeof(stub_last_control_packet));
    }
    return stub_send_control_result;
}

int64_t esp_timer_get_time(void)
{
    return stub_time_us;
}

void mesh_state_notify_waiting_tasks(void)
{
}

bool mesh_state_has_root_addr(void)
{
    return false;
}

const mesh_addr_t *mesh_state_get_root_addr(void)
{
    return NULL;
}

void mesh_state_set_root_addr(const mesh_addr_t *root_addr)
{
    (void)root_addr;
}

void mesh_state_clear_root_addr(void)
{
}

static void reset_state(void)
{
    memset(&s_uplink, 0, sizeof(s_uplink));
    memset(s_uplink_password, 0, sizeof(s_uplink_password));
    is_mesh_root = false;
    is_mesh_connected = false;
    is_mesh_root_ready = false;
    mesh_self_organized_mode = false;
    mesh_runtime_started = false;
    stub_set_router_calls = 0;
    stub_send_control_calls = 0;
    stub_set_router_result = ESP_OK;
    stub_send_control_result = ESP_OK;
    stub_time_us = 0;
    memset(&stub_last_control_packet, 0, sizeof(stub_last_control_packet));
}

void setUp(void)
{
    reset_state();
}

void tearDown(void)
{
}

void test_root_applies_router_once_and_skips_duplicate_runtime_requests(void)
{
    is_mesh_root = true;
    is_mesh_root_ready = true;
    is_mesh_connected = true;
    s_uplink.root_applied = true;
    stub_time_us = 1234 * 1000;

    uplink_ctrl_message_t m1 = {.enabled = true};
    snprintf(m1.ssid, sizeof(m1.ssid), "Unique1");
    snprintf(m1.password, sizeof(m1.password), "pw123");
    
    TEST_ASSERT_EQUAL_INT(ESP_OK, network_set_uplink_config(&m1));
    TEST_ASSERT_EQUAL_INT(1, stub_set_router_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_send_control_calls); 

    stub_time_us = 1235 * 1000;
    TEST_ASSERT_EQUAL_INT(ESP_OK, network_set_uplink_config(&m1));
    TEST_ASSERT_EQUAL_INT(1, stub_set_router_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_send_control_calls);
}

void test_root_reapplies_when_current_state_not_applied(void)
{
    is_mesh_root = true;
    is_mesh_root_ready = true;
    is_mesh_connected = true;
    s_uplink.root_applied = false;
    stub_time_us = 2000 * 1000;

    uplink_ctrl_message_t m2 = {.enabled = true};
    snprintf(m2.ssid, sizeof(m2.ssid), "Unique2");
    snprintf(m2.password, sizeof(m2.password), "pw123");
    
    TEST_ASSERT_EQUAL_INT(ESP_OK, network_set_uplink_config(&m2));
    TEST_ASSERT_EQUAL_INT(1, stub_set_router_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_send_control_calls);

    s_uplink.root_applied = false;
    stub_time_us = 2001 * 1000;
    snprintf(m2.ssid, sizeof(m2.ssid), "Unique3");
    
    TEST_ASSERT_EQUAL_INT(ESP_OK, network_set_uplink_config(&m2));
    TEST_ASSERT_EQUAL_INT(2, stub_set_router_calls);
    TEST_ASSERT_EQUAL_INT(2, stub_send_control_calls);
}

void test_non_root_forwards_request_without_router_calls(void)
{
    is_mesh_root = false;
    is_mesh_connected = true;
    stub_send_control_result = ESP_OK;
    stub_time_us = 3000 * 1000;

    uplink_ctrl_message_t m3 = {.enabled = true};
    snprintf(m3.ssid, sizeof(m3.ssid), "Unique4");
    snprintf(m3.password, sizeof(m3.password), "pw123");
    
    TEST_ASSERT_EQUAL_INT(ESP_OK, network_set_uplink_config(&m3));
    TEST_ASSERT_EQUAL_INT(0, stub_set_router_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_send_control_calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_root_applies_router_once_and_skips_duplicate_runtime_requests);
    RUN_TEST(test_root_reapplies_when_current_state_not_applied);
    RUN_TEST(test_non_root_forwards_request_without_router_calls);
    return UNITY_END();
}
