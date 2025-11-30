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
static uint32_t last_latency_measurement = 10; // Default 10ms

// Task handles for event notifications (set to NULL if not created)
static TaskHandle_t heartbeat_task_handle = NULL;
static TaskHandle_t waiting_task_handles[2] = {NULL, NULL};  // For startup notifications
static int waiting_task_count = 0;

// Fallback timeout for root election (same for all nodes)
// Primary root preference is via vote_percentage, this is just a safety fallback
// if mesh formation gets stuck
#define MESH_FALLBACK_TIMEOUT_MS 10000

// Event-driven mesh readiness flow:
// 1. esp_timer one-shot timer enforces esp_mesh_fix_root(true) if no connection after 5 seconds
// 2. MESH_EVENT_ROOT_FIXED or MESH_EVENT_PARENT_CONNECTED fires when node becomes ready
// 3. Event handler configures static IP (if root) and sets is_mesh_root_ready = true
// 4. Waiting tasks are notified via xTaskNotifyGive() - they wake up immediately
// 5. Audio transmission begins immediately without polling delays
// Fully event-driven: no polling loops, all state transitions via events/notifications

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
static void mesh_root_timeout_callback(void *arg);  // Timer callback, not a task
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
            break;
        }
        
        case MESH_EVENT_PARENT_DISCONNECTED:
            // For root node, "parent" is the external router - which we don't use
            // Only log disconnection for non-root nodes (actual mesh disconnection)
            if (!is_mesh_root) {
                ESP_LOGI(TAG, "Parent disconnected");
                is_mesh_connected = false;
            }
            // Root nodes ignore this - they have no real parent in standalone mesh
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
            // Continue immediately - blocking receive handles waiting
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
        
        // Check for audio frames (both raw and Opus)
        if (hdr->type == NET_PKT_TYPE_AUDIO_RAW || hdr->type == NET_PKT_TYPE_AUDIO_OPUS) {
            // Debug: count audio frames received
            static uint32_t audio_frames_rx = 0;
            audio_frames_rx++;
            if ((audio_frames_rx % 100) == 1) {
                ESP_LOGI(TAG, "Audio frame RX #%lu: type=%u seq=%u ttl=%u size=%d callback=%s",
                         audio_frames_rx, hdr->type, seq, hdr->ttl, data.size,
                         audio_rx_callback ? "YES" : "NO");
            }
            
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
            
            ESP_LOGD(TAG, "Audio frame type=%u stream=%u seq=%u ttl=%u received", 
                     hdr->type, hdr->stream_id, seq, hdr->ttl);
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
    memcpy((char *)mesh_config.mesh_ap.password, MESH_PASSWORD, strlen(MESH_PASSWORD));
    mesh_config.mesh_ap.max_connection = 10;
    mesh_config.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_config));
    
    // Enable self-organized mode (automatic root election)
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
    
    // Set maximum layer depth
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(6));
    
    // Root preference via vote_percentage (the proper ESP-MESH way!)
    // vote_percentage is a THRESHOLD, not a weight:
    // LOWER threshold = EASIER to become root (node accepts root role more readily)
    // HIGHER threshold = HARDER to become root (node resists becoming root)
    // 
    // TX/COMBO nodes should preferentially become root (audio source at network center)
    // RX nodes should avoid becoming root unless no TX is available
    if (my_node_role == NODE_ROLE_TX) {
        ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.7));
        ESP_LOGI(TAG, "Vote threshold: 0.7 (TX - prefer becoming root)");
    } else {
        ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.95));
        ESP_LOGI(TAG, "Vote threshold: 0.95 (RX - avoid becoming root)");
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
       // Timeout timer below will enforce root after 5 seconds if no connection
       ESP_ERROR_CHECK(esp_mesh_start());
      
      ESP_LOGI(TAG, "Mesh initialized: ID=%s, Channel=%d", MESH_ID, MESH_CHANNEL);
      
      // Start receive task (stack size from config/build.h)
      xTaskCreate(mesh_rx_task, "mesh_rx", MESH_RX_TASK_STACK, NULL, MESH_RX_TASK_PRIO, NULL);
      
      // Start heartbeat task (stack size from config/build.h)
      xTaskCreate(mesh_heartbeat_task, "mesh_hb", HEARTBEAT_TASK_STACK, NULL, HEARTBEAT_TASK_PRIO, &heartbeat_task_handle);
      
      // Create fallback timer for root forcing
      // This is a safety net - primary root selection is via vote_percentage
      // Timer fires after 10s if mesh formation hasn't completed
      const esp_timer_create_args_t timeout_timer_args = {
          .callback = &mesh_root_timeout_callback,
          .name = "mesh_timeout",
          .dispatch_method = ESP_TIMER_TASK
      };
      esp_timer_handle_t timeout_timer;
      ESP_ERROR_CHECK(esp_timer_create(&timeout_timer_args, &timeout_timer));
      ESP_ERROR_CHECK(esp_timer_start_once(timeout_timer, MESH_FALLBACK_TIMEOUT_MS * 1000));
      
      ESP_LOGI(TAG, "Root fallback timeout: %d ms (vote_percentage handles preference)", 
               MESH_FALLBACK_TIMEOUT_MS);
      
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

// Root timeout callback - fallback if mesh formation gets stuck
// Primary root selection is via vote_percentage; this is a safety net
// Called once by one-shot timer; actual readiness is handled by MESH_EVENT_ROOT_FIXED
// IMPORTANT: Only TX/COMBO nodes should ever become root. RX nodes wait indefinitely.
static void mesh_root_timeout_callback(void *arg) {
    if (!is_mesh_connected && !is_mesh_root) {
        // RX nodes should NEVER become root - they wait indefinitely for TX/COMBO
        if (my_node_role == NODE_ROLE_RX) {
            ESP_LOGI(TAG, "RX node: still waiting for TX/COMBO root (will not force root)");
            return;
        }
        
        ESP_LOGI(TAG, "Mesh formation fallback timeout (%d ms) - forcing root", 
                 MESH_FALLBACK_TIMEOUT_MS);
        
        // Set this node as mesh root
        // Order matters: set type FIRST, then fix it
        esp_err_t err = esp_mesh_set_type(MESH_ROOT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set mesh type to ROOT: %s", esp_err_to_name(err));
            return;
        }
        
        err = esp_mesh_fix_root(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to fix root: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGD(TAG, "Mesh connection established before fallback timeout");
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
    if ((++send_count % 64) == 0) {
        int route_table_size = 0;
        mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];
        esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
        ESP_LOGI(TAG, "Mesh TX: route=%d, sent=%lu, drops=%lu (%.1f%%), backoff=%lu", 
                 route_table_size, total_sent, total_drops,
                 total_sent > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f,
                 backoff_level);
    }
    
    // SIMPLIFIED BROADCAST: Single esp_mesh_send() call instead of per-child loop
    // ESP-MESH handles tree distribution internally via its forwarding mechanism
    // This drastically reduces TX queue pressure (1 send vs N sends per frame)
    if (is_mesh_root) {
        // ROOT: Use P2P broadcast to inject into mesh tree
        // NULL destination with P2P flag broadcasts to all descendants
        err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    } else {
        // CHILD: Send upstream to parent, who forwards via tree
        err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_TODS | MESH_DATA_NONBLOCK, NULL, 0);
    }
    
    if (err == ESP_OK) {
        total_sent++;
    } else if (err == ESP_ERR_MESH_QUEUE_FULL) {
        any_queue_full = true;
    }
    
    // Update backoff state based on queue status
    // Key insight: increase quickly on failure, decrease slowly on sustained success
    if (any_queue_full || err == ESP_ERR_MESH_QUEUE_FULL) {
        success_streak = 0;  // Reset success counter
        if (backoff_level < RATE_LIMIT_MAX_LEVEL) {
            backoff_level++;
            ESP_LOGW(TAG, "Mesh TX backoff increased to level %lu (sending every %lu frames)", 
                     backoff_level, backoff_level + 1);
        }
    } else {
        // Gradual recovery: require sustained success before reducing backoff
        success_streak++;
        if (success_streak >= RATE_LIMIT_RECOVERY_STREAK && backoff_level > 0) {
            backoff_level--;
            success_streak = 0;  // Reset for next level
            ESP_LOGI(TAG, "Mesh TX backoff decreased to level %lu", backoff_level);
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
        .tos = MESH_TOS_DEF  // High priority for control
    };
    
    // Use MESH_DATA_P2P for intra-mesh communication (standalone mesh)
    esp_err_t err = esp_mesh_send(NULL, &mesh_data, MESH_DATA_P2P, NULL, 0);
    
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
    // Ready if: (1) connected as child, or (2) root AND fully initialized
    return is_mesh_connected || (is_mesh_root && is_mesh_root_ready);
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
