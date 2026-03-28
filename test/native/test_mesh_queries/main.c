#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#define ADC_CHANNEL_3 3

#include "../../../lib/network/src/mesh/mesh_queries.c"

static bool stub_mesh_root = false;
static uint8_t stub_mesh_layer = 0;
static int stub_routing_table_size = 0;
static esp_err_t stub_disconnect_result = ESP_OK;
static esp_err_t stub_connect_result = ESP_OK;
static esp_err_t stub_sta_get_ap_info_result = ESP_OK;
static esp_err_t stub_ap_get_sta_list_result = ESP_OK;
static int stub_disconnect_calls = 0;
static int stub_connect_calls = 0;
static uint32_t stub_last_delay_ticks = 0;
static int64_t stub_time_us = 0;
static wifi_ap_record_t stub_ap_info = {0};
static wifi_sta_list_t stub_sta_list = {0};

node_role_t my_node_role = NODE_ROLE_OUT;
uint8_t my_stream_id = 0;
uint8_t my_sta_mac[6] = {0};
char g_src_id[NETWORK_SRC_ID_LEN] = {0};

bool is_mesh_connected = false;
bool is_mesh_root = false;
bool is_mesh_root_ready = false;
uint8_t mesh_layer = 0;
int mesh_children_count = 0;
mesh_addr_t mesh_parent_addr = {{0}};

uint32_t measured_latency_ms = 0;
bool ping_pending = false;
int8_t nearest_child_rssi = -100;
uint32_t nearest_child_latency_ms = 0;
mesh_addr_t nearest_child_addr = {{0}};
bool child_ping_pending = false;
int64_t last_ping_sent_us = 0;
int64_t last_child_ping_sent_us = 0;

static mesh_addr_t stub_cached_root_addr = {{0}};
static bool stub_have_root_addr = false;

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

TaskHandle_t heartbeat_task_handle = NULL;
TaskHandle_t waiting_task_handles[2] = {0};
int waiting_task_count = 0;

network_audio_callback_t audio_rx_callback = NULL;
network_heartbeat_callback_t heartbeat_rx_callback = NULL;

uint32_t total_drops = 0;
uint32_t total_sent = 0;
volatile uint32_t tx_bytes_counter = 0;

uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE] = {0};
const mesh_addr_t audio_multicast_group = {{0}};

bool esp_mesh_is_root(void)
{
    return stub_mesh_root;
}

uint8_t esp_mesh_get_layer(void)
{
    return stub_mesh_layer;
}

int esp_mesh_get_routing_table_size(void)
{
    return stub_routing_table_size;
}

esp_err_t esp_mesh_disconnect(void)
{
    stub_disconnect_calls++;
    return stub_disconnect_result;
}

esp_err_t esp_mesh_connect(void)
{
    stub_connect_calls++;
    return stub_connect_result;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    if (stub_sta_get_ap_info_result == ESP_OK && ap_info) {
        *ap_info = stub_ap_info;
    }
    return stub_sta_get_ap_info_result;
}

esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta_list)
{
    if (stub_ap_get_sta_list_result == ESP_OK && sta_list) {
        *sta_list = stub_sta_list;
    }
    return stub_ap_get_sta_list_result;
}

int64_t esp_timer_get_time(void)
{
    return stub_time_us;
}

void vTaskDelay(uint32_t ticks)
{
    stub_last_delay_ticks = ticks;
}

void mesh_state_notify_waiting_tasks(void)
{
}

bool mesh_state_has_root_addr(void)
{
    return stub_have_root_addr;
}

const mesh_addr_t *mesh_state_get_root_addr(void)
{
    return stub_have_root_addr ? &stub_cached_root_addr : NULL;
}

void mesh_state_set_root_addr(const mesh_addr_t *root_addr)
{
    if (!root_addr) {
        return;
    }
    stub_cached_root_addr = *root_addr;
    stub_have_root_addr = true;
}

void mesh_state_clear_root_addr(void)
{
    memset(&stub_cached_root_addr, 0, sizeof(stub_cached_root_addr));
    stub_have_root_addr = false;
}

static void reset_state(void)
{
    stub_mesh_root = false;
    stub_mesh_layer = 0;
    stub_routing_table_size = 0;
    stub_disconnect_result = ESP_OK;
    stub_connect_result = ESP_OK;
    stub_sta_get_ap_info_result = ESP_OK;
    stub_ap_get_sta_list_result = ESP_OK;
    stub_disconnect_calls = 0;
    stub_connect_calls = 0;
    stub_last_delay_ticks = 0;
    stub_time_us = 0;
    stub_ap_info.rssi = 0;
    memset(&stub_sta_list, 0, sizeof(stub_sta_list));

    is_mesh_connected = false;
    is_mesh_root = false;
    is_mesh_root_ready = false;
    mesh_layer = 0;
    measured_latency_ms = 0;
    stub_have_root_addr = true;
    memset(&stub_cached_root_addr, 0, sizeof(stub_cached_root_addr));
    tx_bytes_counter = 0;
    nearest_child_rssi = -100;
    rejoin_attempt_count = 0;
    rejoin_window_start_ms = 0;
    rejoin_cooldown_until_ms = 0;
    memset(nearest_child_addr.addr, 0, sizeof(nearest_child_addr.addr));
}

void setUp(void)
{
    reset_state();
}

void tearDown(void)
{
}

void test_connected_nodes_clamps_zero_and_excludes_self(void)
{
    stub_routing_table_size = 0;
    TEST_ASSERT_EQUAL_UINT32(0, network_get_connected_nodes());

    stub_routing_table_size = 1;
    TEST_ASSERT_EQUAL_UINT32(0, network_get_connected_nodes());

    stub_routing_table_size = 5;
    TEST_ASSERT_EQUAL_UINT32(4, network_get_connected_nodes());
}

void test_jitter_prefill_penalties_increase_monotonically(void)
{
    const uint8_t expected_baseline =
        (JITTER_PREFILL_FRAMES > JITTER_BUFFER_FRAMES) ? JITTER_BUFFER_FRAMES : JITTER_PREFILL_FRAMES;
    const uint8_t expected_with_single_penalty =
        ((uint16_t)JITTER_PREFILL_FRAMES + 1u > JITTER_BUFFER_FRAMES)
            ? JITTER_BUFFER_FRAMES
            : (uint8_t)(JITTER_PREFILL_FRAMES + 1u);
    const uint8_t expected_with_double_penalty =
        ((uint16_t)JITTER_PREFILL_FRAMES + 2u > JITTER_BUFFER_FRAMES)
            ? JITTER_BUFFER_FRAMES
            : (uint8_t)(JITTER_PREFILL_FRAMES + 2u);

    mesh_layer = 1;
    measured_latency_ms = 50;
    stub_routing_table_size = 2;
    uint8_t baseline = network_get_jitter_prefill_frames();

    mesh_layer = 2;
    measured_latency_ms = 50;
    stub_routing_table_size = 2;
    uint8_t with_hop_penalty = network_get_jitter_prefill_frames();

    mesh_layer = 1;
    measured_latency_ms = 50;
    stub_routing_table_size = 4;
    uint8_t with_node_penalty = network_get_jitter_prefill_frames();

    mesh_layer = 2;
    measured_latency_ms = 51;
    stub_routing_table_size = 2;
    uint8_t with_latency_penalty = network_get_jitter_prefill_frames();

    TEST_ASSERT_EQUAL_UINT8(expected_baseline, baseline);
    TEST_ASSERT_EQUAL_UINT8(expected_with_single_penalty, with_hop_penalty);
    TEST_ASSERT_EQUAL_UINT8(expected_with_single_penalty, with_node_penalty);
    TEST_ASSERT_EQUAL_UINT8(expected_with_double_penalty, with_latency_penalty);

    TEST_ASSERT_TRUE(with_hop_penalty >= baseline);
    TEST_ASSERT_TRUE(with_node_penalty >= baseline);
    TEST_ASSERT_TRUE(with_latency_penalty >= with_hop_penalty);
    TEST_ASSERT_TRUE(with_latency_penalty >= with_node_penalty);
}

void test_jitter_prefill_clamps_to_buffer_limit(void)
{
    mesh_layer = 10;
    measured_latency_ms = 200;
    stub_routing_table_size = 8;

    uint8_t prefill = network_get_jitter_prefill_frames();
    TEST_ASSERT_TRUE(prefill <= JITTER_BUFFER_FRAMES);
    TEST_ASSERT_EQUAL_UINT8(JITTER_BUFFER_FRAMES, prefill);
}

void test_stream_ready_requires_root_ready_on_root_node(void)
{
    is_mesh_root = true;
    is_mesh_connected = false;
    is_mesh_root_ready = false;
    TEST_ASSERT_FALSE(network_is_stream_ready());

    is_mesh_root_ready = true;
    TEST_ASSERT_TRUE(network_is_stream_ready());
}

void test_trigger_rejoin_rejects_when_called_on_root(void)
{
    is_mesh_root = true;
    stub_time_us = 1000;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, network_trigger_rejoin());
    TEST_ASSERT_EQUAL_INT(0, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(0, stub_connect_calls);
}

void test_trigger_rejoin_resets_state_and_reconnects_child(void)
{
    is_mesh_root = false;
    is_mesh_connected = true;
    stub_have_root_addr = true;
    stub_disconnect_result = ESP_ERR_MESH_DISCONNECTED;
    stub_connect_result = ESP_OK;
    stub_time_us = 1000;

    TEST_ASSERT_EQUAL_INT(ESP_OK, network_trigger_rejoin());
    TEST_ASSERT_FALSE(stub_have_root_addr);
    TEST_ASSERT_FALSE(is_mesh_connected);
    TEST_ASSERT_EQUAL_INT(1, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_connect_calls);
    TEST_ASSERT_EQUAL_UINT32(50, stub_last_delay_ticks);
    TEST_ASSERT_EQUAL_UINT32(1, rejoin_attempt_count);
}

void test_trigger_rejoin_surfaces_connect_failure(void)
{
    stub_time_us = 1000;
    stub_connect_result = ESP_ERR_INVALID_STATE;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, network_trigger_rejoin());
    TEST_ASSERT_EQUAL_INT(1, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_connect_calls);
}

void test_rejoin_allowed_rejects_root_and_honors_cooldown(void)
{
    is_mesh_root = true;
    TEST_ASSERT_FALSE(network_rejoin_allowed());

    is_mesh_root = false;
    rejoin_cooldown_until_ms = 5000;
    stub_time_us = 4000 * 1000;
    TEST_ASSERT_FALSE(network_rejoin_allowed());

    stub_time_us = 5000 * 1000;
    TEST_ASSERT_TRUE(network_rejoin_allowed());
}

void test_trigger_rejoin_blocked_during_cooldown(void)
{
    is_mesh_root = false;
    rejoin_cooldown_until_ms = 5000;
    stub_time_us = 4500 * 1000;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, network_trigger_rejoin());
    TEST_ASSERT_EQUAL_INT(0, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(0, stub_connect_calls);
}

void test_trigger_rejoin_trips_circuit_breaker_at_attempt_limit(void)
{
    is_mesh_root = false;
    rejoin_window_start_ms = 1000;
    rejoin_attempt_count = OUT_REJOIN_MAX_ATTEMPTS;
    stub_time_us = 1500 * 1000;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, network_trigger_rejoin());
    TEST_ASSERT_TRUE(rejoin_cooldown_until_ms > 1500);
    TEST_ASSERT_EQUAL_INT(0, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(0, stub_connect_calls);
}

void test_trigger_rejoin_resets_attempt_window_after_timeout(void)
{
    is_mesh_root = false;
    is_mesh_connected = true;
    stub_have_root_addr = true;
    rejoin_window_start_ms = 1000;
    rejoin_attempt_count = OUT_REJOIN_MAX_ATTEMPTS;
    stub_time_us = (1000 + OUT_REJOIN_WINDOW_MS + 1) * 1000LL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, network_trigger_rejoin());
    TEST_ASSERT_EQUAL_UINT32(1, rejoin_attempt_count);
    TEST_ASSERT_EQUAL_INT(1, stub_disconnect_calls);
    TEST_ASSERT_EQUAL_INT(1, stub_connect_calls);
}

void test_get_tx_bytes_and_reset_clears_counter(void)
{
    tx_bytes_counter = 321;

    TEST_ASSERT_EQUAL_UINT32(321, network_get_tx_bytes_and_reset());
    TEST_ASSERT_EQUAL_UINT32(0, tx_bytes_counter);
}

void test_get_rssi_returns_default_on_wifi_error(void)
{
    stub_sta_get_ap_info_result = ESP_ERR_INVALID_STATE;

    TEST_ASSERT_EQUAL_INT(-100, network_get_rssi());
}

void test_get_nearest_child_rssi_returns_default_when_not_root(void)
{
    is_mesh_root = false;

    TEST_ASSERT_EQUAL_INT(-100, network_get_nearest_child_rssi());
}

void test_get_nearest_child_rssi_selects_highest_station_rssi(void)
{
    is_mesh_root = true;
    stub_ap_get_sta_list_result = ESP_OK;
    stub_sta_list.num = 3;
    stub_sta_list.sta[0].rssi = -78;
    stub_sta_list.sta[1].rssi = -61;
    stub_sta_list.sta[2].rssi = -65;
    uint8_t expected_mac[6] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
    memcpy(stub_sta_list.sta[1].mac, expected_mac, sizeof(expected_mac));

    TEST_ASSERT_EQUAL_INT(-61, network_get_nearest_child_rssi());
    TEST_ASSERT_EQUAL_MEMORY(expected_mac, nearest_child_addr.addr, sizeof(expected_mac));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_connected_nodes_clamps_zero_and_excludes_self);
    RUN_TEST(test_jitter_prefill_penalties_increase_monotonically);
    RUN_TEST(test_jitter_prefill_clamps_to_buffer_limit);
    RUN_TEST(test_stream_ready_requires_root_ready_on_root_node);
    RUN_TEST(test_trigger_rejoin_rejects_when_called_on_root);
    RUN_TEST(test_trigger_rejoin_resets_state_and_reconnects_child);
    RUN_TEST(test_trigger_rejoin_surfaces_connect_failure);
    RUN_TEST(test_rejoin_allowed_rejects_root_and_honors_cooldown);
    RUN_TEST(test_trigger_rejoin_blocked_during_cooldown);
    RUN_TEST(test_trigger_rejoin_trips_circuit_breaker_at_attempt_limit);
    RUN_TEST(test_trigger_rejoin_resets_attempt_window_after_timeout);
    RUN_TEST(test_get_tx_bytes_and_reset_clears_counter);
    RUN_TEST(test_get_rssi_returns_default_on_wifi_error);
    RUN_TEST(test_get_nearest_child_rssi_returns_default_when_not_root);
    RUN_TEST(test_get_nearest_child_rssi_selects_highest_station_rssi);
    return UNITY_END();
}
