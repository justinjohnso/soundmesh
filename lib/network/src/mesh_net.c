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
static uint8_t my_stream_id = 1;  // Unique stream ID for this TX/COMBO node

// Mesh state
static bool is_mesh_connected = false;
static bool is_mesh_root = false;
static bool is_mesh_root_ready = false;  // Track when root is fully initialized

static uint8_t mesh_layer = 0;
static int mesh_children_count = 0;
static mesh_addr_t mesh_parent_addr;
static uint32_t measured_latency_ms = 0;       // RTT/2 from ping measurements
static uint32_t last_ping_sent_ms = 0;         // Timestamp when last ping was sent
static bool ping_pending = false;              // Waiting for pong response

// Nearest child tracking (for root node display)
static int8_t nearest_child_rssi = -100;       // Best RSSI from any child
static uint32_t nearest_child_latency_ms = 0;  // RTT/2 to nearest child
static mesh_addr_t nearest_child_addr;         // Address of nearest child
static uint32_t last_child_ping_ms = 0;
static bool child_ping_pending = false;

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
static void send_pong(const mesh_addr_t *dest, uint32_t original_timestamp);
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
                // Root doesn't need self-organized scanning either
                esp_mesh_set_self_organized(false, false);
                esp_wifi_scan_stop();
            }
            break;
            
        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "Mesh stopped");
            is_mesh_connected = false;
            break;
            
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            memcpy(&mesh_parent_addr, &connected->connected, sizeof(mesh_addr_t));
            is_mesh_connected = true;
            is_mesh_root_ready = true;  // Child nodes are immediately ready
            mesh_layer = esp_mesh_get_layer();
            
            ESP_LOGI(TAG, "Parent connected, layer: %d (stream ready)", mesh_layer);
            
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
                // Re-enable self-organized mode so mesh can scan and rejoin
                esp_mesh_set_self_organized(true, true);
                ESP_LOGI(TAG, "Self-organized re-enabled for reconnection");
            }
            break;
            
        case MESH_EVENT_CHILD_CONNECTED: {
            int new_count = esp_mesh_get_routing_table_size();
            ESP_LOGI(TAG, "Child connected (routing table: %d)", new_count);
            mesh_children_count = new_count;
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
            ESP_LOGI(TAG, "Root address event received: " MACSTR, MAC2STR(root_addr->addr));
            // Root is already marked ready when ROOT_FIXED fired, this is just informational
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
            // Heartbeat packet
            if (is_mesh_root && data.size >= sizeof(mesh_heartbeat_t)) {
                mesh_heartbeat_t *hb = (mesh_heartbeat_t *)data.data;
                if (hb->rssi > nearest_child_rssi || nearest_child_rssi == -100) {
                    nearest_child_rssi = hb->rssi;
                    memcpy(&nearest_child_addr, &from, sizeof(mesh_addr_t));
                    ESP_LOGD(TAG, "Child heartbeat: RSSI=%d dBm", nearest_child_rssi);
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
    // Both root and children respond to pings
    ESP_LOGD(TAG, "Ping received, sending pong");
    send_pong(from, ntohl(ping->timestamp));
}

// Handle incoming pong - calculate RTT
static void handle_pong(const mesh_ping_t *pong) {
    uint32_t now_ms = esp_timer_get_time() / 1000;
    uint32_t original_ts = ntohl(pong->timestamp);
    
    // Check if this is response to our ping (RX->root or root->child)
    if (ping_pending && original_ts == last_ping_sent_ms) {
        uint32_t rtt = now_ms - original_ts;
        measured_latency_ms = rtt / 2;
        ESP_LOGD(TAG, "Ping RTT: %lu ms, latency: %lu ms", rtt, measured_latency_ms);
        ping_pending = false;
    } else if (child_ping_pending && original_ts == last_child_ping_ms) {
        uint32_t rtt = now_ms - original_ts;
        nearest_child_latency_ms = rtt / 2;
        ESP_LOGD(TAG, "Child ping RTT: %lu ms, latency: %lu ms", rtt, nearest_child_latency_ms);
        child_ping_pending = false;
    }
}

// Send pong response to a specific node
static void send_pong(const mesh_addr_t *dest, uint32_t original_timestamp) {
    mesh_ping_t pong;
    pong.type = NET_PKT_TYPE_PONG;
    pong.reserved[0] = 0;
    pong.reserved[1] = 0;
    pong.reserved[2] = 0;
    pong.timestamp = htonl(original_timestamp);  // Echo back original timestamp
    
    mesh_data_t mesh_data = {
        .data = (uint8_t *)&pong,
        .size = sizeof(pong),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF
    };
    
    esp_err_t err = esp_mesh_send(dest, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to send pong: %s", esp_err_to_name(err));
    }
}

// Send ping to root (called by RX nodes)
esp_err_t network_send_ping(void) {
    if (is_mesh_root || !is_mesh_connected) {
        return ESP_ERR_INVALID_STATE;  // Root doesn't ping itself
    }
    
    if (ping_pending) {
        return ESP_ERR_INVALID_STATE;  // Already waiting for response
    }
    
    mesh_ping_t ping;
    ping.type = NET_PKT_TYPE_PING;
    ping.reserved[0] = 0;
    ping.reserved[1] = 0;
    ping.reserved[2] = 0;
    
    last_ping_sent_ms = esp_timer_get_time() / 1000;
    ping.timestamp = htonl(last_ping_sent_ms);
    
    mesh_data_t mesh_data = {
        .data = (uint8_t *)&ping,
        .size = sizeof(ping),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF
    };
    
    // Send to parent (which routes toward root)
    esp_err_t err = esp_mesh_send(&mesh_parent_addr, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    if (err == ESP_OK) {
        ping_pending = true;
    } else if (err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGW(TAG, "Ping send failed: %s", esp_err_to_name(err));
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
    
    // Generate unique stream ID from MAC address (for TX/COMBO nodes)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    my_stream_id = mac[5];  // Use last byte of MAC as stream ID
    
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
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
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
    mesh_config.router.bssid[0] = 0xFF;  // Invalid BSSID to prevent connection
    
    // Mesh AP configuration
    // CRITICAL: Use strcpy with +1 for null terminator, not memcpy!
    strcpy((char *)mesh_config.mesh_ap.password, MESH_PASSWORD);
    mesh_config.mesh_ap.max_connection = 10;
    mesh_config.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));
    
    // Enable self-organized mode (automatic root election)
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
    
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
    heartbeat.reserved = 0;
    
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
        if (check_size == 0 && backoff_level > 0) {
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
    
    esp_err_t err;
    if (is_mesh_root) {
        // Root broadcasts to all children
        err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    } else {
        // Children send to parent (toward root)
        err = esp_mesh_send(&mesh_parent_addr, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
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
    // Return number of other nodes in mesh (excluding self)
    // Routing table contains all descendants (children, grandchildren, etc.)
    return esp_mesh_get_routing_table_size();
}

int network_get_nearest_child_rssi(void) {
    return nearest_child_rssi;
}

uint32_t network_get_nearest_child_latency_ms(void) {
    return nearest_child_latency_ms;
}

esp_err_t network_ping_nearest_child(void) {
    if (!is_mesh_root) {
        return ESP_ERR_INVALID_STATE;  // Only root pings children
    }
    
    if (child_ping_pending) {
        return ESP_ERR_INVALID_STATE;  // Already waiting
    }
    
    // Need at least one child to ping
    int route_size = esp_mesh_get_routing_table_size();
    if (route_size == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    mesh_ping_t ping;
    ping.type = NET_PKT_TYPE_PING;
    ping.reserved[0] = 0;
    ping.reserved[1] = 0;
    ping.reserved[2] = 0;
    
    last_child_ping_ms = esp_timer_get_time() / 1000;
    ping.timestamp = htonl(last_child_ping_ms);
    
    mesh_data_t mesh_data = {
        .data = (uint8_t *)&ping,
        .size = sizeof(ping),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_DEF
    };
    
    // Send to nearest child (tracked from heartbeats)
    esp_err_t err = esp_mesh_send(&nearest_child_addr, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    if (err == ESP_OK) {
        child_ping_pending = true;
        ESP_LOGD(TAG, "Ping sent to nearest child");
    }
    
    return err;
}

// Register callback for audio frame reception (used by RX nodes)
esp_err_t network_register_audio_callback(network_audio_callback_t callback) {
    audio_rx_callback = callback;
    ESP_LOGI(TAG, "Audio callback registered");
    return ESP_OK;
}


