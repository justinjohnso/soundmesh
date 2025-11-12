#include "network/mesh_net.h"
#include "config/build.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_mesh.h>
#include <esp_mesh_internal.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <string.h>
#include <esp_timer.h>

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
static uint8_t mesh_layer = 0;
static int mesh_children_count = 0;
static mesh_addr_t mesh_parent_addr;
static uint32_t last_latency_measurement = 10; // Default 10ms

// Mesh startup timeout - force root after 5 seconds if no network found
#define MESH_SEARCH_TIMEOUT_MS 5000

// Receive buffer for mesh packets
#define MESH_RX_BUFFER_SIZE 1500
static uint8_t mesh_rx_buffer[MESH_RX_BUFFER_SIZE];

// Duplicate suppression cache for broadcast forwarding
#define DEDUPE_CACHE_SIZE 256  // Covers >1 second @ 200 fps
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
static void mesh_root_timeout_task(void *arg);
static bool is_duplicate(uint8_t stream_id, uint16_t seq);
static void mark_seen(uint8_t stream_id, uint16_t seq);
static void forward_to_children(const uint8_t *data, size_t len, const mesh_addr_t *sender);
static void send_heartbeat(void);
static void send_stream_announcement(void);

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

// Forward frame to all children except sender
static void forward_to_children(const uint8_t *data, size_t len, const mesh_addr_t *sender) {
    if (!is_mesh_connected) return;
    
    // Get routing table (list of children)
    mesh_addr_t route_table[10];
    int route_table_size = 0;
    
    esp_mesh_get_routing_table(route_table, 10 * 6, &route_table_size);
    
    if (route_table_size == 0) {
        return; // No children to forward to
    }
    
    // Forward to each child except the sender
    for (int i = 0; i < route_table_size; i++) {
        // Don't echo back to sender
        if (sender && memcmp(&route_table[i], sender, 6) == 0) {
            continue;
        }
        
        mesh_data_t mesh_data;
        mesh_data.data = (uint8_t *)data;
        mesh_data.size = len;
        mesh_data.proto = MESH_PROTO_BIN;
        mesh_data.tos = MESH_TOS_P2P;
        
        esp_err_t err = esp_mesh_send(&route_table[i], &mesh_data, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Failed to forward to child: %s", esp_err_to_name(err));
        }
    }
}

// Mesh event handler
static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            break;
            
        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "Mesh stopped");
            is_mesh_connected = false;
            break;
            
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            memcpy(&mesh_parent_addr, &connected->connected, sizeof(mesh_addr_t));
            is_mesh_connected = true;
            mesh_layer = esp_mesh_get_layer();
            ESP_LOGI(TAG, "Parent connected, layer: %d", mesh_layer);
            break;
        }
        
        case MESH_EVENT_PARENT_DISCONNECTED:
            ESP_LOGI(TAG, "Parent disconnected");
            is_mesh_connected = false;
            break;
            
        case MESH_EVENT_CHILD_CONNECTED:
            ESP_LOGI(TAG, "Child connected");
            mesh_children_count = esp_mesh_get_routing_table_size();
            break;
        
        case MESH_EVENT_CHILD_DISCONNECTED:
            ESP_LOGI(TAG, "Child disconnected");
            mesh_children_count = esp_mesh_get_routing_table_size();
            break;
        
        case MESH_EVENT_ROOT_FIXED:
            ESP_LOGI(TAG, "Became mesh root");
            is_mesh_root = true;
            mesh_layer = 0;
            break;
            
        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
            ESP_LOGI(TAG, "ToDS state: %d", *toDs_state);
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
    
    while (1) {
        // Set up receive buffer
        data.data = mesh_rx_buffer;
        data.size = MESH_RX_BUFFER_SIZE;
        
        // Blocking receive with infinite timeout
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mesh receive error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Validate minimum frame size
        if (data.size < NET_FRAME_HEADER_SIZE) {
            ESP_LOGD(TAG, "Received too-small packet: %d bytes", data.size);
            continue;
        }
        
        // Parse frame header
        net_frame_header_t *hdr = (net_frame_header_t *)data.data;
        
        // Validate frame magic
        if (hdr->magic != NET_FRAME_MAGIC || hdr->version != NET_FRAME_VERSION) {
            ESP_LOGD(TAG, "Invalid frame header: magic=0x%02x version=%d", hdr->magic, hdr->version);
            continue;
        }
        
        uint16_t seq = ntohs(hdr->seq);
        
        // Check for audio frames
        if (hdr->type == NET_PKT_TYPE_AUDIO_RAW) {
            // Duplicate suppression for broadcast
            if (is_duplicate(hdr->stream_id, seq)) {
                ESP_LOGD(TAG, "Duplicate frame stream=%u seq=%u, dropping", hdr->stream_id, seq);
                continue;
            }
            mark_seen(hdr->stream_id, seq);
            
            // Check TTL - drop if expired
            if (hdr->ttl == 0) {
                ESP_LOGD(TAG, "TTL expired for seq=%u, dropping", seq);
                continue;
            }
            
            // Decrement TTL and forward to children (tree broadcast)
            hdr->ttl--;
            forward_to_children(data.data, data.size, &from);
            
            // Call audio callback if registered (for RX nodes)
            if (audio_rx_callback && data.size > NET_FRAME_HEADER_SIZE) {
                uint8_t *payload = data.data + NET_FRAME_HEADER_SIZE;
                uint16_t payload_len = ntohs(hdr->payload_len);
                uint32_t timestamp = ntohl(hdr->timestamp);
                audio_rx_callback(payload, payload_len, seq, timestamp);
            }
            
            ESP_LOGD(TAG, "Audio frame stream=%u seq=%u ttl=%u received", hdr->stream_id, seq, hdr->ttl);
        } else if (hdr->type == NET_PKT_TYPE_HEARTBEAT) {
            // Heartbeat messages - log for debugging
            ESP_LOGD(TAG, "Heartbeat received");
        } else if (hdr->type == NET_PKT_TYPE_STREAM_ANNOUNCE) {
            // Stream announcement - log for debugging
            ESP_LOGD(TAG, "Stream announcement received");
        }
    }
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
    memcpy((char *)mesh_config.mesh_ap.password, MESH_PASSWORD, strlen(MESH_PASSWORD));
    mesh_config.mesh_ap.max_connection = 10;
    mesh_config.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));
    
    // Enable self-organized mode (automatic root election)
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
    
    // Set maximum layer depth
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    
    // Set root preference: TX/COMBO nodes prefer to be root over RX nodes
    // This is only used during natural election events (boot, root failure)
    if (my_node_role == NODE_ROLE_TX) {
        // TX/COMBO nodes: prefer to be root (higher priority)
        ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.9));  // 90% vote weight
        ESP_LOGI(TAG, "Root preference: HIGH (TX/COMBO node)");
    } else {
        // RX nodes: lower root preference
        ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.1));  // 10% vote weight
        ESP_LOGI(TAG, "Root preference: LOW (RX node)");
    }
    
    // Don't fix root initially - let timeout task handle it
    // This allows natural mesh formation if another node is nearby
    ESP_ERROR_CHECK(esp_mesh_fix_root(false));
     
     // Configure root election attempts BEFORE starting mesh
     // Use shorter scan to allow timeout-based root election within 5 seconds
     mesh_attempts_t attempts = {
         .scan = 3,     // Scan 3 times (faster progression to timeout fallback)
         .vote = 100,   // Reasonable vote count (will proceed to next step)
         .fail = 60,    // Keep default fail threshold
         .monitor_ie = 3  // Keep default IE monitoring
     };
     ESP_ERROR_CHECK(esp_mesh_set_attempts(&attempts));
      
      // Disable WiFi power save for better real-time performance
      ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
      
      // Start mesh - this begins the network search
      // Our timeout task will force root after 5 seconds if no network found
      ESP_ERROR_CHECK(esp_mesh_start());
     
     ESP_LOGI(TAG, "Mesh initialized: ID=%s, Channel=%d", MESH_ID, MESH_CHANNEL);
     
     // Start receive task
     xTaskCreate(mesh_rx_task, "mesh_rx", 4096, NULL, 5, NULL);
     
     // Start heartbeat task (2 second interval) - includes root timeout enforcement
     xTaskCreate(mesh_heartbeat_task, "mesh_hb", 3072, NULL, 4, NULL);
     
     // Also enforce timeout immediately in a separate task to guarantee root becomes available quickly
      xTaskCreate(mesh_root_timeout_task, "mesh_timeout", 4096, NULL, 5, NULL);
     
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
    announce.channels = AUDIO_CHANNELS;
    announce.bits_per_sample = AUDIO_BITS_PER_SAMPLE;
    announce.frame_size_ms = htons(AUDIO_FRAME_MS);
    
    esp_err_t err = network_send_control((uint8_t *)&announce, sizeof(announce));
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Failed to send stream announcement: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Stream announced: ID=%u, %uHz, %u-bit, %uch, %ums frames", 
                 announce.stream_id, (unsigned int)AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, 
                 AUDIO_CHANNELS, (unsigned int)AUDIO_FRAME_MS);
    }
}

// Root timeout task - becomes root if no existing mesh found after 5 seconds
// This prevents nodes from hanging indefinitely in search mode
static void mesh_root_timeout_task(void *arg) {
    uint32_t start_time = esp_timer_get_time() / 1000;
    
    ESP_LOGI(TAG, "Root timeout monitor started - will become root after 5 seconds if no network found");
    
    // Wait for mesh to connect OR timeout
    while (!is_mesh_connected && !is_mesh_root) {
        uint32_t elapsed = (esp_timer_get_time() / 1000) - start_time;
        
        if (elapsed >= MESH_SEARCH_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Timeout enforced after %lu ms - fixing this node as root", (unsigned long)elapsed);
            
            // Fix this node as root permanently (for single device or until external mesh forms)
            esp_err_t err = esp_mesh_fix_root(true);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Fixed as root node");
                
                // Wait for mesh root event to be processed (state machine in ESP-IDF takes time)
                uint32_t root_wait_start = esp_timer_get_time() / 1000;
                while (!is_mesh_root) {
                    uint32_t root_wait_elapsed = (esp_timer_get_time() / 1000) - root_wait_start;
                    if (root_wait_elapsed > 2000) {
                        ESP_LOGW(TAG, "Timeout waiting for root event after %lu ms", (unsigned long)root_wait_elapsed);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                if (is_mesh_root) {
                    ESP_LOGI(TAG, "Mesh root established successfully");
                    
                    // Wait additional time for mesh to fully initialize and stabilize
                    // During this time, mesh child connections and parent AP setup occur
                    ESP_LOGI(TAG, "Waiting for mesh to stabilize...");
                    vTaskDelay(pdMS_TO_TICKS(2000));  // Wait 2 more seconds for full stabilization
                    
                    ESP_LOGI(TAG, "Mesh network is now stable and ready for transmission");
                }
            } else {
                ESP_LOGW(TAG, "Failed to fix as root: %s", esp_err_to_name(err));
            }
            break;  // Exit after one attempt
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Task complete, delete self
    vTaskDelete(NULL);
}

// Heartbeat task - sends periodic heartbeats
static void mesh_heartbeat_task(void *arg) {
    const uint32_t HEARTBEAT_INTERVAL_MS = 2000;  // 2 seconds
    
    ESP_LOGI(TAG, "Heartbeat task started");
    
    // Wait for mesh to be connected or timeout enforced
    // (The separate mesh_root_timeout_task handles root election)
    uint32_t wait_start = esp_timer_get_time() / 1000;
    while (!is_mesh_connected && !is_mesh_root) {
        uint32_t elapsed = (esp_timer_get_time() / 1000) - wait_start;
        // Log periodically for debugging
        if (elapsed % 5000 < 100) {  // Every 5 seconds
            ESP_LOGD(TAG, "Waiting for mesh (elapsed: %lu ms)", (unsigned long)elapsed);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Send initial stream announcement (TX/COMBO only)
    send_stream_announcement();
    
    while (1) {
        send_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

// Send audio frame via mesh (broadcast to all nodes)
esp_err_t network_send_audio(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !is_mesh_root) {
        return ESP_ERR_INVALID_STATE;
    }
    
    mesh_data_t mesh_data;
    mesh_data.data = (uint8_t *)data;
    mesh_data.size = len;
    mesh_data.proto = MESH_PROTO_BIN;
    mesh_data.tos = MESH_TOS_P2P;  // Low priority for audio
    
    // Broadcast to all descendants (NULL destination = broadcast)
    esp_err_t err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_TODS, NULL, 0);
    
    if (err != ESP_OK && err != ESP_ERR_MESH_NO_ROUTE_FOUND) {
        ESP_LOGD(TAG, "Mesh send failed: %s", esp_err_to_name(err));
    }
    
    return err;
}

// Send control message via mesh
esp_err_t network_send_control(const uint8_t *data, size_t len) {
    if (!is_mesh_connected && !is_mesh_root) {
        return ESP_ERR_INVALID_STATE;
    }
    
    mesh_data_t mesh_data;
    mesh_data.data = (uint8_t *)data;
    mesh_data.size = len;
    mesh_data.proto = MESH_PROTO_BIN;
    mesh_data.tos = MESH_TOS_DEF;  // High priority for control
    
    esp_err_t err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_TODS, NULL, 0);
    
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
    return last_latency_measurement;
}

bool network_is_stream_ready(void) {
    return is_mesh_connected || is_mesh_root;
}

uint32_t network_get_connected_nodes(void) {
    // This is an approximation - in true mesh, we'd need to query all nodes
    return mesh_children_count + 1;
}

// Register callback for audio frame reception (used by RX nodes)
esp_err_t network_register_audio_callback(network_audio_callback_t callback) {
    audio_rx_callback = callback;
    ESP_LOGI(TAG, "Audio callback registered");
    return ESP_OK;
}

esp_err_t network_start_latency_measurement(void) {
    // Mesh latency can be estimated from hop count
    // For now, use a simple formula: 5ms per hop
    last_latency_measurement = mesh_layer * 5;
    return ESP_OK;
}
