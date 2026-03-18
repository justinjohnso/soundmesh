#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_mesh.h>
#include <esp_mesh_internal.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <string.h>
#include <esp_timer.h>
#include <lwip/ip4_addr.h>

static const char *TAG = "network_mesh";

// Node role for root preference
typedef enum {
    NODE_ROLE_RX = 0,
    NODE_ROLE_TX = 1  // TX and COMBO both report as TX
} node_role_t;

static node_role_t my_node_role = NODE_ROLE_RX;  // Set during init
static uint8_t my_stream_id = 1;  // Unique stream ID derived from MAC hash
static uint8_t my_sta_mac[6] = {0};  // Own STA MAC for routing table self-filtering

// Mesh state
static bool is_mesh_connected = false;
static bool is_mesh_root = false;
static bool is_mesh_root_ready = false;  // Track when root is fully initialized

static uint8_t mesh_layer = 0;
static int mesh_children_count = 0;
static mesh_addr_t mesh_parent_addr;
static uint32_t measured_latency_ms = 0;       // RTT/2 from ping measurements
static bool ping_pending = false;              // Waiting for pong response

// Nearest child tracking (for root node display)
static int8_t nearest_child_rssi = -100;       // Best RSSI from any child
static uint32_t nearest_child_latency_ms = 0;  // RTT/2 to nearest child
static mesh_addr_t nearest_child_addr;         // Address of nearest child
static bool child_ping_pending = false;
static int64_t last_ping_sent_us = 0;
static int64_t last_child_ping_sent_us = 0;

// Root address cache for P2P child→root communication
static mesh_addr_t cached_root_addr;
static bool have_root_addr = false;

// Game-style ping sequence counters
static uint32_t ping_seq = 0;           // RX→root ping sequence
static uint32_t child_ping_seq = 0;     // Root→child ping sequence
static uint32_t pending_ping_id = 0;    // Expected pong ping_id (RX side)
static uint32_t pending_child_ping_id = 0;  // Expected pong ping_id (root side)

// Task handles for event notifications (set to NULL if not created)
static TaskHandle_t heartbeat_task_handle = NULL;
static TaskHandle_t waiting_task_handles[2] = {NULL, NULL};  // For startup notifications
static int waiting_task_count = 0;

// Event-driven mesh readiness flow (User Designated Root pattern):
// 1. TX/COMBO: esp_mesh_set_type(MESH_ROOT) + esp_mesh_fix_root(true) before start
// 2. RX: esp_mesh_fix_root(true) only - waits indefinitely for TX root
// 3. MESH_EVENT_ROOT_FIXED (TX) or MESH_EVENT_PARENT_CONNECTED (RX) fires when ready
// 4. Event handler sets is_mesh_root_ready = true
// 5. Waiting tasks notified via xTaskNotifyGive() - audio starts immediately

// Receive buffer for mesh packets (size defined in config/build.h)
static uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE];

// Duplicate suppression cache for broadcast forwarding (size in config/build.h)
typedef struct {
    uint8_t stream_id;
    uint16_t seq;
    uint32_t timestamp_ms;
} recent_frame_t;

static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];
static int dedupe_index = 0;

// Convert string MESH_ID to 6-byte mesh_addr_t with readable encoding
static void mesh_id_from_string(const char *str, uint8_t *mesh_id) {
    // Encode string as truncated ASCII bytes for partial readability
    // "MeshNet-Audio-48" -> "MshN48" -> {0x4D, 0x73, 0x68, 0x4E, 0x34, 0x38}
    const char *readable = "MshN48";  // Truncated version of MESH_ID
    for (int i = 0; i < 6; i++) {
        mesh_id[i] = (uint8_t)readable[i];
    }
}

// Audio callback for received frames
typedef void (*network_audio_callback_t)(const uint8_t *payload, size_t len, uint16_t seq, uint32_t timestamp);
static network_audio_callback_t audio_rx_callback = NULL;

// Heartbeat callback for portal state
static network_heartbeat_callback_t heartbeat_rx_callback = NULL;

// Forward declarations
static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
static void mesh_rx_task(void *arg);
static void mesh_heartbeat_task(void *arg);
static bool is_duplicate(uint8_t stream_id, uint16_t seq);
static void mark_seen(uint8_t stream_id, uint16_t seq);
static void forward_to_children(const uint8_t *data, size_t len, const mesh_addr_t *sender);
static void send_heartbeat(void);
static void send_stream_announcement(void);
static void send_pong(const mesh_addr_t *dest, uint32_t ping_id);
static void handle_ping(const mesh_addr_t *from, const mesh_ping_t *ping);
static void handle_pong(const mesh_ping_t *pong);

// Duplicate detection
static bool is_duplicate(uint8_t stream_id, uint16_t seq) {
    for (int i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        if (dedupe_cache[i].stream_id == stream_id && 
            dedupe_cache[i].seq == seq) {
            return true;  // Already seen this frame
        }
    }
    return false;
}

static void mark_seen(uint8_t stream_id, uint16_t seq) {
    dedupe_cache[dedupe_index].stream_id = stream_id;
    dedupe_cache[dedupe_index].seq = seq;
    dedupe_cache[dedupe_index].timestamp_ms = esp_timer_get_time() / 1000;
    dedupe_index = (dedupe_index + 1) % DEDUPE_CACHE_SIZE;
}

// Forward frame to all descendants except sender
// Note: esp_mesh_get_routing_table() returns ALL descendants (children, grandchildren, etc.)
// Dedupe+TTL in the receiver handles any duplicates naturally
static void forward_to_children(const uint8_t *data, size_t len, const mesh_addr_t *sender) {
    if (!is_mesh_connected && !is_mesh_root) return;
    
    // Get full routing table - contains all descendants in the mesh tree
    mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    
    esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
    
    if (route_table_size == 0) {
        return;
    }
    
    // Forward to all descendants except the sender
    // The mesh stack handles the actual routing - we just need to initiate sends
    // Dedupe cache and TTL on the receiving end prevent loops/duplicates
    for (int i = 0; i < route_table_size; i++) {
        if (memcmp(&route_table[i], my_sta_mac, 6) == 0) continue;
        if (sender && memcmp(&route_table[i], sender, 6) == 0) {
            continue;
        }
        
        mesh_data_t mesh_data;
        mesh_data.data = (uint8_t *)data;
        mesh_data.size = len;
        mesh_data.proto = MESH_PROTO_BIN;
        mesh_data.tos = MESH_TOS_DEF;
        
        // Use MESH_DATA_P2P for internal mesh communication
        esp_err_t err = esp_mesh_send(&route_table[i], &mesh_data, 
                                      MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
        if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
            ESP_LOGD(TAG, "Failed to forward to descendant: %s", esp_err_to_name(err));
        }
    }
}

// Mesh event handler
static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            // For designated root: ROOT_FIXED won't fire if type was set before start
            // Mark root as ready immediately since mesh AP is now broadcasting
            if (my_node_role == NODE_ROLE_TX && esp_mesh_is_root()) {
                is_mesh_root = true;
                is_mesh_root_ready = true;
                mesh_layer = 0;
                ESP_LOGI(TAG, "Designated root ready: mesh AP broadcasting on channel %d", MESH_CHANNEL);
                for (int i = 0; i < waiting_task_count; i++) {
                    if (waiting_task_handles[i] != NULL) {
                        xTaskNotifyGive(waiting_task_handles[i]);
                    }
                }
                // Don't disable self-organized or call esp_wifi_disconnect() here —
                // doing so immediately at start can prevent children from joining.
                // Self-organized will be disabled after first child connects.
                esp_log_level_set("wifi", ESP_LOG_ERROR);
            }
            break;
            
        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "Mesh stopped");
            is_mesh_connected = false;
            break;
            
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            memcpy(mesh_parent_addr.addr, connected->connected.bssid, 6);
            is_mesh_connected = true;
            is_mesh_root_ready = true;  // Child nodes are immediately ready
            mesh_layer = esp_mesh_get_layer();
            
            ESP_LOGI(TAG, "Parent connected, layer: %d (stream ready)", mesh_layer);
            ESP_LOGI(TAG, "Parent BSSID: " MACSTR, MAC2STR(connected->connected.bssid));
            
            // Notify all waiting tasks - stream is ready
            for (int i = 0; i < waiting_task_count; i++) {
                if (waiting_task_handles[i] != NULL) {
                    xTaskNotifyGive(waiting_task_handles[i]);
                }
            }
            
            // Disable self-organized mode to stop periodic parent monitoring scans
            // These scans pause the radio for ~300ms and cause audio dropouts
            esp_mesh_set_self_organized(false, false);
            esp_wifi_scan_stop();
            ESP_LOGI(TAG, "Self-organized disabled (no more parent scans during streaming)");
            break;
        }
        
        case MESH_EVENT_PARENT_DISCONNECTED:
            // For root node, "parent" is the external router - which we don't use
            // Use esp_mesh_is_root() (live check) instead of cached flag to avoid
            // race during startup where ROOT_FIXED hasn't fired yet
            if (!esp_mesh_is_root()) {
                ESP_LOGI(TAG, "Parent disconnected");
                is_mesh_connected = false;
                have_root_addr = false;
                // Re-enable self-organized mode so mesh can scan and rejoin
                esp_mesh_set_self_organized(true, true);
                ESP_LOGI(TAG, "Self-organized re-enabled for reconnection");
            }
            break;
            
        case MESH_EVENT_CHILD_CONNECTED: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Child connected (routing table: %d)", new_count);
            mesh_children_count = new_count;
            // Now that a child has joined, disable root's self-organized scanning
            // to avoid radio pauses that cause audio dropouts.
            // Also disconnect STA to stop futile "MESHNET_DISABLED" connection attempts.
            // (Moved here from MESH_EVENT_STARTED to avoid racing with children joining)
            if (is_mesh_root) {
                esp_mesh_set_self_organized(false, false);
                esp_wifi_scan_stop();
                esp_wifi_disconnect();
                ESP_LOGI(TAG, "Root: self-organized disabled, STA disconnected (child connected)");
            }
            break;
        }
        
        case MESH_EVENT_CHILD_DISCONNECTED: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Child disconnected (routing table: %d)", new_count);
            mesh_children_count = new_count;
            break;
        }
        
        case MESH_EVENT_ROOT_FIXED:
            // IMPORTANT: ROOT_FIXED fires on ALL nodes when they join a mesh with a fixed root,
            // not just on the actual root! We must check esp_mesh_is_root() to verify.
            if (esp_mesh_is_root()) {
                ESP_LOGI(TAG, "Became mesh root (role=%s)", 
                         my_node_role == NODE_ROLE_TX ? "TX/COMBO" : "RX");
                is_mesh_root = true;
                mesh_layer = 0;
                
                // Mark root as ready - mesh AP is automatically configured by the stack
                is_mesh_root_ready = true;
                ESP_LOGI(TAG, "Root ready: mesh AP broadcasting on channel %d", MESH_CHANNEL);
                
                // Notify all waiting tasks - root is ready
                for (int i = 0; i < waiting_task_count; i++) {
                    if (waiting_task_handles[i] != NULL) {
                        xTaskNotifyGive(waiting_task_handles[i]);
                    }
                }
            } else {
                // We joined a mesh with a fixed root, but we're not the root
                ESP_LOGI(TAG, "Joined mesh with fixed root (we are not root)");
            }
            break;
            
        case MESH_EVENT_ROOT_ADDRESS: {
            mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
            memcpy(cached_root_addr.addr, root_addr->addr, 6);
            have_root_addr = true;
            ESP_LOGI(TAG, "Root addr cached: " MACSTR, MAC2STR(cached_root_addr.addr));
            break;
        }
            
        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
            ESP_LOGI(TAG, "ToDS state: %d", *toDs_state);
            break;
        }
        
        case MESH_EVENT_ROOT_SWITCH_REQ:
            ESP_LOGI(TAG, "Root switch requested");
            // Sync our flags with actual mesh state
            is_mesh_root = esp_mesh_is_root();
            break;
            
        case MESH_EVENT_ROOT_SWITCH_ACK:
            ESP_LOGI(TAG, "Root switch acknowledged");
            // Update root status - we may have become or stopped being root
            is_mesh_root = esp_mesh_is_root();
            is_mesh_root_ready = is_mesh_root;
            
            // If we just became root, notify waiting tasks
            if (is_mesh_root) {
                for (int i = 0; i < waiting_task_count; i++) {
                    if (waiting_task_handles[i] != NULL) {
                        xTaskNotifyGive(waiting_task_handles[i]);
                    }
                }
                ESP_LOGI(TAG, "Now acting as mesh root");
            } else {
                ESP_LOGI(TAG, "No longer mesh root");
            }
            break;
        
        case MESH_EVENT_NO_PARENT_FOUND:
            ESP_LOGW(TAG, "No parent found — retrying scan (channel=%d, mesh_id=%s)", 
                     MESH_CHANNEL, MESH_ID);
            break;

        case MESH_EVENT_FIND_NETWORK: {
            mesh_event_find_network_t *evt = (mesh_event_find_network_t *)event_data;
            ESP_LOGI(TAG, "Found network on channel %d - join in progress", evt->channel);
            break;
        }
        
        case MESH_EVENT_SCAN_DONE: {
            mesh_event_scan_done_t *scan = (mesh_event_scan_done_t *)event_data;
            ESP_LOGD(TAG, "Scan done: found %d APs", (int)scan->number);
            break;
        }
        
        default:
            ESP_LOGD(TAG, "Mesh event: %ld", event_id);
            break;
    }
}



// Mesh receive task - continuously receives packets from mesh
static void mesh_rx_task(void *arg) {
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    
    ESP_LOGI(TAG, "Mesh RX task started");
    
    // Wait for mesh to start before calling esp_mesh_recv()
    while (!is_mesh_root_ready && !is_mesh_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Mesh RX task: network ready, entering recv loop (root=%d, connected=%d)",
             is_mesh_root, is_mesh_connected);
    
    while (1) {
        // Set up receive buffer
        data.data = mesh_rx_buffer;
        data.size = MESH_RX_BUFFER_SIZE;
        
        // Blocking receive with infinite timeout
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        
        if (err != ESP_OK) {
            if (err == ESP_ERR_MESH_NOT_START) {
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGW(TAG, "Mesh receive error: %s", esp_err_to_name(err));
            }
            continue;
        }
        
        // Check first byte to determine packet type
        // Control packets (heartbeat, ping, pong) have type as first byte
        // Audio frames have magic (0xA5) as first byte
        uint8_t first_byte = data.data[0];
        
        if (first_byte == NET_PKT_TYPE_HEARTBEAT) {
            if (esp_mesh_is_root() && data.size >= sizeof(mesh_heartbeat_t)) {
                mesh_heartbeat_t *hb = (mesh_heartbeat_t *)data.data;
                bool same_child = (memcmp(&from, &nearest_child_addr, 6) == 0);
                bool uninit = (nearest_child_rssi == -100);
                bool better = (hb->rssi > nearest_child_rssi);
                
                if (uninit || better) {
                    memcpy(&nearest_child_addr, &from, sizeof(mesh_addr_t));
                    nearest_child_rssi = hb->rssi;
                } else if (same_child) {
                    nearest_child_rssi = hb->rssi;
                }
                ESP_LOGI(TAG, "Child heartbeat: RSSI=%d dBm", nearest_child_rssi);
                
                // Call portal heartbeat callback if registered
                if (heartbeat_rx_callback) {
                    heartbeat_rx_callback(from.addr, hb);
                }
            }
        } else if (first_byte == NET_PKT_TYPE_PING) {
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *ping = (mesh_ping_t *)data.data;
                handle_ping(&from, ping);
            }
        } else if (first_byte == NET_PKT_TYPE_PONG) {
            if (data.size >= sizeof(mesh_ping_t)) {
                mesh_ping_t *pong = (mesh_ping_t *)data.data;
                handle_pong(pong);
            }
        } else if (first_byte == NET_PKT_TYPE_STREAM_ANNOUNCE) {
            ESP_LOGD(TAG, "Stream announcement received");
        } else if (first_byte == NET_FRAME_MAGIC) {
            // Audio frame with full header
            if (data.size < NET_FRAME_HEADER_SIZE) {
                continue;
            }
            
            net_frame_header_t *hdr = (net_frame_header_t *)data.data;
            if (hdr->version != NET_FRAME_VERSION) {
                continue;
            }
            
            uint16_t seq = ntohs(hdr->seq);
            
            if (hdr->type == NET_PKT_TYPE_AUDIO_RAW || hdr->type == NET_PKT_TYPE_AUDIO_OPUS) {
                static uint32_t audio_frames_rx = 0;
                audio_frames_rx++;
                if ((audio_frames_rx % 500) == 1) {
                    ESP_LOGI(TAG, "Audio frame RX #%lu: seq=%u size=%d",
                             audio_frames_rx, seq, data.size);
                }
                
                if (is_duplicate(hdr->stream_id, seq)) {
                    continue;
                }
                mark_seen(hdr->stream_id, seq);
                
                if (hdr->ttl == 0) {
                    continue;
                }
                
                hdr->ttl--;
                forward_to_children(data.data, data.size, &from);
                
                if (audio_rx_callback && data.size > NET_FRAME_HEADER_SIZE) {
                    uint8_t *payload = data.data + NET_FRAME_HEADER_SIZE;
                    uint16_t total_payload_len = ntohs(hdr->payload_len);
                    uint32_t timestamp = ntohl(hdr->timestamp);
                    uint8_t frame_count = hdr->reserved;
                    
                    if (frame_count <= 1) {
                        audio_rx_callback(payload, total_payload_len, seq, timestamp);
                    } else {
                        // Unpack batched Opus frames: [len_hi][len_lo][data...]...
                        size_t offset = 0;
                        for (uint8_t f = 0; f < frame_count && offset + 2 <= total_payload_len; f++) {
                            uint16_t frame_len = (payload[offset] << 8) | payload[offset + 1];
                            offset += 2;
                            if (frame_len > 0 && offset + frame_len <= total_payload_len) {
                                audio_rx_callback(&payload[offset], frame_len, seq + f, timestamp);
                                offset += frame_len;
                            }
                        }
                    }
                }
            }
        }
    }
}

// Handle incoming ping - respond with pong
static void handle_ping(const mesh_addr_t *from, const mesh_ping_t *ping) {
    uint32_t id = ntohl(ping->ping_id);
    ESP_LOGI(TAG, "PING from " MACSTR " id=%lu (root=%d)", 
             MAC2STR(from->addr), id, is_mesh_root);
    send_pong(from, id);
}

// Handle incoming pong - calculate RTT
static void handle_pong(const mesh_ping_t *pong) {
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
        ESP_LOGW(TAG, "PONG unmatched id=%lu (expect ping=%lu child=%lu)", 
                 id, pending_ping_id, pending_child_ping_id);
    }
}

// Send pong response to a specific node
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
        .tos = MESH_TOS_DEF
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

// Send ping to root (called by RX nodes)
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
        .tos = MESH_TOS_DEF
    };
    
    ping_pending = true;
    esp_err_t err = esp_mesh_send(&cached_root_addr, &mesh_data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ping_pending = false;
        ESP_LOGW(TAG, "Ping send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PING sent to root id=%lu", ping_seq);
    }
    
    return err;
}

// Initialize mesh network
esp_err_t network_init_mesh(void) {
    ESP_LOGI(TAG, "Initializing ESP-WIFI-MESH");
    
    // Determine node role based on build environment
    #if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)
        my_node_role = NODE_ROLE_TX;
        ESP_LOGI(TAG, "Node role: TX/COMBO (root preference enabled)");
    #else
        my_node_role = NODE_ROLE_RX;
        ESP_LOGI(TAG, "Node role: RX");
    #endif
    
    // Save own MAC for routing table self-filtering and stream ID
    esp_read_mac(my_sta_mac, ESP_MAC_WIFI_STA);
    // Derive stream_id from full MAC via XOR fold — uses all 6 bytes instead of just MAC[5]
    my_stream_id = my_sta_mac[0] ^ my_sta_mac[1] ^ my_sta_mac[2] ^
                   my_sta_mac[3] ^ my_sta_mac[4] ^ my_sta_mac[5];
    ESP_LOGI(TAG, "Node MAC: " MACSTR ", stream_id=0x%02X", MAC2STR(my_sta_mac), my_stream_id);
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize networking
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default network interfaces
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(NULL, NULL));
    
    // Initialize WiFi
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    // Use RAM storage to prevent stale NVS WiFi state from blocking mesh join
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());
    
    // Register mesh event handler
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    
    // Get the mesh netif - we'll configure static IP if node becomes root
    esp_netif_t *mesh_netif = esp_netif_get_handle_from_ifkey("WIFI_MESH_ROOT");
    if (mesh_netif == NULL) {
        // Try alternate name used in some configurations
        mesh_netif = esp_netif_get_handle_from_ifkey("WIFI_MESH");
    }
    
    // Configure mesh
    mesh_cfg_t mesh_config = MESH_INIT_CONFIG_DEFAULT();
    
    // Convert string MESH_ID to 6-byte mesh_addr_t
    uint8_t mesh_id_bytes[6];
    mesh_id_from_string(MESH_ID, mesh_id_bytes);
    memcpy((uint8_t *)&mesh_config.mesh_id, mesh_id_bytes, 6);
    
    ESP_LOGI(TAG, "Mesh ID: %02X:%02X:%02X:%02X:%02X:%02X (\"%s\")", 
             mesh_id_bytes[0], mesh_id_bytes[1], mesh_id_bytes[2],
             mesh_id_bytes[3], mesh_id_bytes[4], mesh_id_bytes[5], MESH_ID);
    
    mesh_config.channel = MESH_CHANNEL;
    
    // For standalone mesh mode (no external router):
    // ESP-WIFI-MESH requires a valid router config even in standalone mode
    // Set placeholder SSID but disable router switching
    memset(&mesh_config.router, 0, sizeof(mesh_config.router));
    strcpy((char *)mesh_config.router.ssid, "MESHNET_DISABLED");
    mesh_config.router.ssid_len = strlen("MESHNET_DISABLED");
    mesh_config.router.password[0] = '\0';
    mesh_config.router.allow_router_switch = false;
    memset(mesh_config.router.bssid, 0, 6);  // Zero BSSID — no external router
    
    // Mesh AP configuration
    // CRITICAL: Use strcpy with +1 for null terminator, not memcpy!
    strcpy((char *)mesh_config.mesh_ap.password, MESH_PASSWORD);
    mesh_config.mesh_ap.max_connection = 10;
    mesh_config.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));
    
    // Enable self-organized mode with parent selection for RX nodes
    // select_parent=true ensures RX immediately begins scanning for the root
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, my_node_role == NODE_ROLE_RX));
    
    // Set maximum layer depth
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    
    // User Designated Root Node pattern (ESP-IDF official approach)
    // TX/COMBO nodes are always the root - no election, no scanning delay
    // RX nodes wait indefinitely to join the designated root
    if (my_node_role == NODE_ROLE_TX) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
        ESP_LOGI(TAG, "Designated root: TX/COMBO node set as MESH_ROOT");
    }
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));
      
      // Disable WiFi power save for better real-time performance
      ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
      
      // Create tasks BEFORE mesh start so they can receive notifications
      // from MESH_EVENT_STARTED (fires synchronously during esp_mesh_start)
      xTaskCreate(mesh_rx_task, "mesh_rx", MESH_RX_TASK_STACK, NULL, MESH_RX_TASK_PRIO, NULL);
      xTaskCreate(mesh_heartbeat_task, "mesh_hb", HEARTBEAT_TASK_STACK, NULL, HEARTBEAT_TASK_PRIO, &heartbeat_task_handle);
      
      // Register heartbeat task for startup notification
      network_register_startup_notification(heartbeat_task_handle);
      
      // Start mesh - TX immediately becomes root, RX scans for the TX root
      ESP_ERROR_CHECK(esp_mesh_start());
      
      ESP_LOGI(TAG, "Mesh initialized: ID=%s, Channel=%d", MESH_ID, MESH_CHANNEL);
      
      return ESP_OK;
}

// Send heartbeat message to mesh
static void send_heartbeat(void) {
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
    if (is_mesh_root) {
        memset(heartbeat.parent_mac, 0, 6);
    } else {
        memcpy(heartbeat.parent_mac, mesh_parent_addr.addr, 6);
    }
    
    static uint32_t hb_count = 0;
    hb_count++;
    if ((hb_count % 5) == 1) {
        int rt_size = esp_mesh_get_routing_table_size();
        ESP_LOGI(TAG, "Heartbeat #%lu: root=%d, connected=%d, route_table=%d, children=%d",
                 hb_count, is_mesh_root, is_mesh_connected, rt_size, mesh_children_count);
    }
    
    esp_err_t err = network_send_control((uint8_t *)&heartbeat, sizeof(heartbeat));
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Failed to send heartbeat: %s", esp_err_to_name(err));
    }
}

// Send stream announcement (TX/COMBO nodes only)
static void send_stream_announcement(void) {
    if (my_node_role != NODE_ROLE_TX) {
        return;  // Only TX/COMBO nodes send stream announcements
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

// Heartbeat task - sends periodic heartbeats
// Starts immediately; heartbeats are only sent when is_mesh_root_ready becomes true
static void mesh_heartbeat_task(void *arg) {
    const uint32_t HEARTBEAT_INTERVAL_MS = 2000;  // 2 seconds
    
    ESP_LOGI(TAG, "Heartbeat task started (will send once network is ready)");
    
    // Wait for network readiness event via notification
    // Event handler will notify us when is_mesh_root_ready = true
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (notify_value > 0) {
        ESP_LOGI(TAG, "Network ready - sending heartbeats");
    }
    
    // Send initial stream announcement (TX/COMBO only)
    send_stream_announcement();
    
    while (1) {
        send_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

// Register a task to be notified when network is stream-ready
// This is used by startup code to wait for network initialization without polling
esp_err_t network_register_startup_notification(TaskHandle_t task_handle) {
    if (waiting_task_count >= 2) {
        return ESP_ERR_NO_MEM;  // Can only wait for 2 tasks
    }
    
    waiting_task_handles[waiting_task_count] = task_handle;
    waiting_task_count++;
    
    ESP_LOGD(TAG, "Task registered for startup notification (count=%d)", waiting_task_count);
    
    // If already ready, notify immediately
    if (is_mesh_root_ready) {
        xTaskNotifyGive(task_handle);
        ESP_LOGD(TAG, "Network already ready - notifying immediately");
    }
    
    return ESP_OK;
}

// Adaptive rate limiting state
static uint32_t backoff_level = 0;           // Current backoff level (0-5)
static uint32_t success_streak = 0;          // Consecutive successes for gradual recovery
static uint32_t total_drops = 0;
static uint32_t total_sent = 0;
static uint32_t skip_counter = 0;
static int64_t last_qfull_us = 0;

// Byte counter for TX bandwidth measurement
static volatile uint32_t tx_bytes_counter = 0;

// Rate limiting thresholds - tuned for 25fps on ESP-MESH (40ms frames)
// At backoff_level N, send every (N+1) frames (0=all, 1=half, 2=third)
// With 25fps base rate, even level 2 still gives ~8fps (acceptable for degraded audio)
#define RATE_LIMIT_MAX_LEVEL 2               // Max backoff: send every 3rd frame (~8fps)
#define RATE_LIMIT_RECOVERY_STREAK 25        // Successes needed (~1 second at 25fps)

// Send audio frame via mesh (broadcast to all nodes)
// Uses adaptive rate limiting with gradual recovery to prevent oscillation
esp_err_t network_send_audio(const uint8_t *data, size_t len) {
    // Allow sending if: (1) connected as child, or (2) root AND ready
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Rate limiting: at level N, only send every (N+1)th frame
    // Level 0 = 100%, Level 1 = 50%, Level 2 = 33%, Level 3 = 25%, Level 4 = 20%
    if (backoff_level > 0) {
        skip_counter++;
        if (skip_counter <= backoff_level) {
            total_drops++;
            return ESP_ERR_MESH_QUEUE_FULL;  // Drop this frame (silent)
        }
        skip_counter = 0;
    }
    
    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF  // Use DEF for non-blocking - P2P blocks even with NONBLOCK flag!
    };
    
    esp_err_t err = ESP_OK;
    bool any_queue_full = false;
    
    // Debug: log stats periodically (every 64 packets at 25fps = ~2.5 seconds)
    static uint16_t send_count = 0;
    if ((++send_count & 0x7F) == 0) {
        int route_table_size = 0;
        mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
        esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
        ESP_LOGI(TAG, "Mesh TX: route=%d, sent=%lu, drops=%lu (%.1f%%), backoff=%lu", 
                 route_table_size, total_sent, total_drops,
                 total_sent > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f,
                 backoff_level);
    }
    
    if (is_mesh_root) {
        // ROOT: Send downstream to each descendant via P2P
        // Use MESH_DATA_P2P for standalone mesh (no external router/DS)
        // MESH_DATA_FROMDS is for root-to-DS bridging which doesn't apply here
        mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
        int route_table_size = 0;
        esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
        
        for (int i = 0; i < route_table_size; i++) {
            if (memcmp(&route_table[i], my_sta_mac, 6) == 0) continue;
            esp_err_t send_err = esp_mesh_send(&route_table[i], &mesh_data, 
                                                MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
            if (send_err == ESP_ERR_MESH_QUEUE_FULL) {
                any_queue_full = true;
            } else if (send_err != ESP_OK) {
                err = send_err;
            }
        }
        if (!any_queue_full && route_table_size > 0) {
            err = ESP_OK;
        }
    } else {
        // CHILD: Send upstream to parent via P2P
        err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    }
    
    if (err == ESP_OK) {
        total_sent++;
        tx_bytes_counter += len;
    } else if (err == ESP_ERR_MESH_QUEUE_FULL) {
        any_queue_full = true;
    }
    
    // Update backoff state based on queue status
    if (any_queue_full || err == ESP_ERR_MESH_QUEUE_FULL) {
        last_qfull_us = esp_timer_get_time();
        success_streak = 0;
        if (backoff_level < RATE_LIMIT_MAX_LEVEL) {
            backoff_level++;
            ESP_LOGW(TAG, "Mesh TX backoff increased to level %lu (sending every %lu frames)", 
                     backoff_level, backoff_level + 1);
        }
    } else {
        // Time-based recovery: if no queue-full for 1 second, recover one level
        int64_t now = esp_timer_get_time();
        if (backoff_level > 0 && (now - last_qfull_us) > 1000000) {
            backoff_level--;
            last_qfull_us = now;  // Rate-limit recovery to once per second
            ESP_LOGI(TAG, "Mesh TX backoff recovered to level %lu", backoff_level);
        }
    }

    // Reset backoff when no children to send to
    if (is_mesh_root) {
        mesh_addr_t check_table[MESH_ROUTE_TABLE_SIZE];
        int check_size = 0;
        esp_mesh_get_routing_table(check_table, sizeof(check_table), &check_size);
        if (check_size <= 1 && backoff_level > 0) {
            backoff_level = 0;
            success_streak = 0;
        }
    }
    
    return err;
}

// Send control message via mesh
esp_err_t network_send_control(const uint8_t *data, size_t len) {
    // Allow sending if: (1) connected as child, or (2) root AND ready
    if (!is_mesh_connected && !(is_mesh_root && is_mesh_root_ready)) {
        return ESP_ERR_INVALID_STATE;
    }
    
    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF
    };
    
    esp_err_t err = ESP_OK;
    if (is_mesh_root) {
        // Root: iterate routing table and send to each descendant (P2P)
        // esp_mesh_send(NULL) with MESH_DATA_P2P is invalid for root
        mesh_addr_t ctrl_route[MESH_ROUTE_TABLE_SIZE];
        int ctrl_route_size = 0;
        esp_mesh_get_routing_table(ctrl_route, sizeof(ctrl_route), &ctrl_route_size);
        
        for (int i = 0; i < ctrl_route_size; i++) {
            if (memcmp(&ctrl_route[i], my_sta_mac, 6) == 0) continue;
            esp_err_t send_err = esp_mesh_send(&ctrl_route[i], &mesh_data, 
                                                MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
            if (send_err == ESP_OK) {
                err = ESP_OK;
            } else if (send_err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
                ESP_LOGD(TAG, "Control send to descendant failed: %s", esp_err_to_name(send_err));
            }
        }
    } else {
        if (have_root_addr) {
            err = esp_mesh_send(&cached_root_addr, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Control send failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Topology queries
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
    return measured_latency_ms;  // RTT/2 from ping measurements
}

bool network_is_connected(void) {
    return is_mesh_connected || is_mesh_root;
}

bool network_is_stream_ready(void) {
    // Ready if: (1) connected as child, or (2) root AND fully initialized
    return is_mesh_connected || (is_mesh_root && is_mesh_root_ready);
}

uint32_t network_get_connected_nodes(void) {
    int size = esp_mesh_get_routing_table_size();
    // Routing table includes self — subtract 1 for actual peer count
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
        .tos = MESH_TOS_DEF
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

// Register callback for audio frame reception (used by RX nodes)
esp_err_t network_register_audio_callback(network_audio_callback_t callback) {
    audio_rx_callback = callback;
    ESP_LOGI(TAG, "Audio callback registered");
    return ESP_OK;
}

// Register callback for heartbeat reception (used by portal)
esp_err_t network_register_heartbeat_callback(network_heartbeat_callback_t callback) {
    heartbeat_rx_callback = callback;
    ESP_LOGI(TAG, "Heartbeat callback registered");
    return ESP_OK;
}


