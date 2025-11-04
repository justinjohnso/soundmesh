# MeshNet Audio: Control Layer Architecture

**Date:** November 3, 2025  
**Status:** ✅ APPROVED - Target Architecture  
**Goal:** Define unified observability, state management, and external portal interface

## Control Layer Vision

**"Window into the Network"**
- Any device can observe the entire mesh state in real-time
- Unified telemetry schema across all nodes (network, audio, system metrics)
- External access via USB connection (web UI portal)
- Internal sync via mesh control messages (distributed, resilient)
- Auto-recovery with error visibility

## Principle: Separation of Concerns

**`lib/config/` - Static Build Configuration**
- Compile-time constants (sample rates, buffer sizes)
- Hardware pin mappings (GPIO assignments)
- Board-specific definitions
- **Never changes at runtime**

**`lib/control/` - Runtime Interface & Observability**
- Human-machine interface (displays, buttons, LEDs)
- System state management (modes, errors, recovery)
- Telemetry aggregation (network + audio + system metrics)
- External portal gateway (HTTP, WebSocket, USB networking)
- **Changes during operation based on user input or system events**

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────┐
│                    CONTROL LAYER ARCHITECTURE                   │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │              LOCAL HMI (On-Device Interface)              │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │  • OLED Display (SSD1306) - Status visualization         │ │
│  │  • Buttons - Mode switching, view cycling                │ │
│  │  • LEDs (future) - Visual feedback                       │ │
│  └───────────────────────────────────────────────────────────┘ │
│                              │                                  │
│                              ▼                                  │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │           STATE MANAGEMENT (System Orchestration)         │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │  • State machine (Boot → Joining → Ready → Streaming)    │ │
│  │  • Mode management (Input mode, codec, role)             │ │
│  │  • Error handling & auto-recovery                        │ │
│  │  • Settings persistence (NVS)                            │ │
│  └───────────────────────────────────────────────────────────┘ │
│                              │                                  │
│                              ▼                                  │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │         TELEMETRY ENGINE (Unified Observability)          │ │
│  ├───────────────────────────────────────────────────────────┤ │
│  │  • Metrics aggregation (network, audio, system)          │ │
│  │  • State cache (full mesh view)                          │ │
│  │  • Telemetry publisher (mesh broadcasts)                 │ │
│  │  • Schema encoder/decoder (JSON, gzip)                   │ │
│  │  • Time synchronization                                  │ │
│  └───────────────────────────────────────────────────────────┘ │
│               │                                │                │
│               ▼                                ▼                │
│  ┌──────────────────────┐        ┌──────────────────────────┐ │
│  │  MESH CONTROL MSGS   │        │   EXTERNAL PORTAL        │ │
│  │  (Internal Sync)     │        │   (Observer Gateway)     │ │
│  ├──────────────────────┤        ├──────────────────────────┤ │
│  │ • Telemetry bcast    │        │ • HTTP REST API          │ │
│  │ • Heartbeats         │        │ • WebSocket server       │ │
│  │ • Mixer commands     │        │ • USB networking (CDC-ECM) │ │
│  │ • Time sync          │        │ • Web UI (Astro-based)   │ │
│  └──────────────────────┘        └──────────────────────────┘ │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

## Layer 1: Local HMI (Human-Machine Interface)

### Display System (OLED)

**Purpose:** On-device status visualization without external tools

**Hardware:** SSD1306 128×32 OLED via I2C

**Views (Role-Specific):**

**TX Node:**
- **Network View:** Mesh status, root indicator, layer, children count
- **Audio View:** Input mode (Tone/Aux/USB), streaming status, frequency/level
- **Metrics View:** CPU %, heap free, uptime, buffer fill

**RX Node:**
- **Network View:** RSSI, latency, hop count, parent node ID
- **Audio View:** Receiving status, active streams, buffer fill, underruns
- **Metrics View:** CPU %, heap free, uptime, packet loss

**All Nodes:**
- **Mesh Topology View:** Network-wide node list (root, relays, leaves)
- **Error View:** Current errors, recovery status, last error timestamp

**Display API:**
```c
// lib/control/include/control/display.h

typedef enum {
    DISPLAY_VIEW_NETWORK,
    DISPLAY_VIEW_AUDIO,
    DISPLAY_VIEW_METRICS,
    DISPLAY_VIEW_TOPOLOGY,    // NEW: Full mesh view
    DISPLAY_VIEW_ERROR        // NEW: Error status
} display_view_t;

esp_err_t display_init(void);
void display_clear(void);
void display_render_tx(display_view_t view, const tx_status_t *status);
void display_render_rx(display_view_t view, const rx_status_t *status);
void display_render_topology(const mesh_topology_t *topology);  // NEW
void display_render_error(const error_state_t *error);          // NEW
void display_set_brightness(uint8_t level);  // 0-255
```

### Button System

**Purpose:** User input for mode/view switching

**Hardware:** Single GPIO button (expandable to multiple)

**Current Mapping:**
- **Short press:** Cycle display views
- **Long press (TX only):** Cycle input modes (Tone → Aux → USB)

**Future Expansion:**
- **2nd button:** Direct mode switching (separate from view cycling)
- **Rotary encoder:** Parameter control (tone frequency, gain, etc.)

**Button API:**
```c
// lib/control/include/control/buttons.h

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_DOUBLE_PRESS     // Future: Quick command
} button_event_t;

esp_err_t buttons_init(void);
button_event_t buttons_poll(void);
```

**Debouncing:**
- 50ms stabilization time
- State machine: Idle → Pressed → Released → Debounce

### LED Indicators (Future/v0.3)

**Purpose:** Visual feedback for at-a-glance status

**Proposed LED States:**
- **Solid Green:** Mesh connected, streaming
- **Blinking Green (slow):** Mesh connected, idle
- **Blinking Yellow:** Joining mesh
- **Blinking Red:** Error state
- **Off:** Powered down or initializing

**API (future):**
```c
// lib/control/include/control/leds.h

typedef enum {
    LED_STATE_OFF,
    LED_STATE_MESH_JOINING,
    LED_STATE_CONNECTED_IDLE,
    LED_STATE_STREAMING,
    LED_STATE_ERROR
} led_state_t;

esp_err_t leds_init(void);
void leds_set_state(led_state_t state);
```

## Layer 2: State Management

### System State Machine

**Purpose:** Orchestrate lifecycle, handle errors, enforce valid transitions

**States:**
```
┌──────────┐
│   BOOT   │──────────────┐
└──────────┘              │
     │                    │
     ▼                    ▼
┌──────────┐         ┌─────────┐
│ MESH_JOIN│────────▶│  ERROR  │
└──────────┘         └─────────┘
     │                    ▲
     ▼                    │
┌──────────┐              │
│  READY   │──────────────┘
└──────────┘
     │
     ▼
┌──────────┐
│STREAMING │──────────────┐
└──────────┘              ▲
     │                    │
     └────────────────────┘
```

**State Transitions:**
- **BOOT → MESH_JOIN:** WiFi initialized, scanning for mesh
- **MESH_JOIN → READY:** Parent connected (or became root)
- **READY → STREAMING:** Audio input active (TX) or receiving (RX)
- **STREAMING → READY:** Audio stopped
- **Any → ERROR:** Critical failure (network loss, hardware fault)
- **ERROR → MESH_JOIN:** Auto-recovery after timeout

**Implementation:**
```c
// lib/control/include/control/state_machine.h

typedef enum {
    STATE_BOOT,
    STATE_MESH_JOINING,
    STATE_READY,
    STATE_STREAMING,
    STATE_ERROR
} system_state_t;

typedef enum {
    EVENT_MESH_CONNECTED,
    EVENT_MESH_DISCONNECTED,
    EVENT_AUDIO_STARTED,
    EVENT_AUDIO_STOPPED,
    EVENT_ERROR_OCCURRED,
    EVENT_RECOVERY_TIMEOUT
} state_event_t;

esp_err_t state_machine_init(void);
system_state_t state_machine_get_current(void);
esp_err_t state_machine_handle_event(state_event_t event);
const char* state_machine_get_state_name(system_state_t state);
```

### Mode Management

**TX Input Modes:**
```c
typedef enum {
    INPUT_MODE_TONE,    // Sine wave generator (testing)
    INPUT_MODE_AUX,     // ADC analog input (default)
    INPUT_MODE_USB      // USB audio from computer (future)
} input_mode_t;
```

**Codec Modes (v0.2):**
```c
typedef enum {
    CODEC_MODE_PCM,     // Raw PCM (v0.1)
    CODEC_MODE_OPUS     // Opus compression (v0.2)
} codec_mode_t;
```

**Role Assignment:**
```c
typedef enum {
    NODE_ROLE_TX,       // Transmitter (publishes audio)
    NODE_ROLE_RX,       // Receiver (consumes audio)
    NODE_ROLE_RELAY     // Relay (forwards only, no audio I/O)
} node_role_t;
```

### Error Handling & Auto-Recovery

**Error Types:**
```c
typedef enum {
    ERROR_NONE,
    ERROR_MESH_DISCONNECTED,      // Parent lost, rejoining
    ERROR_AUDIO_UNDERRUN,         // RX buffer starvation
    ERROR_AUDIO_DEVICE_FAULT,     // I2S/ADC hardware failure
    ERROR_MEMORY_EXHAUSTED,       // Heap allocation failed
    ERROR_WATCHDOG_TIMEOUT        // Task hung
} error_type_t;

typedef struct {
    error_type_t type;
    uint32_t timestamp_ms;        // When error occurred
    uint32_t count;               // Times this error occurred
    bool auto_recovery_enabled;
    uint32_t recovery_timeout_ms; // Time until recovery attempt
} error_state_t;
```

**Auto-Recovery Strategies:**
- **Mesh disconnect:** Automatic rejoin (ESP-MESH handles)
- **Audio underrun:** Log event, continue playback (insert silence)
- **Device fault:** Reinitialize peripheral, fallback if persistent
- **Memory exhausted:** Log error, attempt to free caches, reboot if critical
- **Watchdog timeout:** Automatic reboot (FreeRTOS watchdog)

**Implementation:**
```c
// lib/control/include/control/error_handler.h

esp_err_t error_handler_init(void);
void error_handler_report(error_type_t type);
error_state_t error_handler_get_current(void);
void error_handler_clear(error_type_t type);
bool error_handler_should_recover(void);
void error_handler_attempt_recovery(void);
```

### Settings Persistence (NVS)

**Purpose:** Save user preferences, survive reboots

**Stored Settings:**
```c
typedef struct {
    // Mesh configuration
    char mesh_id[33];             // Mesh network ID
    char mesh_password[64];       // WPA2 password
    
    // Audio configuration
    input_mode_t last_input_mode; // Resume last mode on boot
    codec_mode_t codec_mode;      // PCM or Opus
    
    // Display configuration
    uint8_t display_brightness;   // 0-255
    display_view_t default_view;  // Boot into this view
    
    // Mixer presets (v0.3)
    mixer_preset_t presets[8];    // Saved mixer scenes
    
    // Calibration
    int16_t adc_dc_offset;        // DC bias correction for ADC
    
    // Metadata
    char node_name[32];           // User-assigned name (e.g., "Kitchen RX")
    uint32_t settings_version;    // Schema version for migration
} persistent_settings_t;
```

**API:**
```c
// lib/control/include/control/settings.h

esp_err_t settings_init(void);                                    // Load from NVS
esp_err_t settings_save(void);                                    // Write to NVS
esp_err_t settings_reset_to_defaults(void);                       // Factory reset
const persistent_settings_t* settings_get(void);                  // Read-only access
esp_err_t settings_set_mesh_credentials(const char *id, const char *password);
esp_err_t settings_set_input_mode(input_mode_t mode);
esp_err_t settings_set_node_name(const char *name);
```

**NVS Namespace:** `meshnet_cfg`

## Layer 3: Telemetry Engine (Unified Observability)

### Unified Telemetry Schema

**Purpose:** Single source of truth for all node state, readable format, version-safe

**Schema Definition (JSON):**
```json
{
  "version": "1.0.0",
  "timestamp": 1699012345678,
  "node_id": "TX-A3F2",
  "node_name": "Living Room TX",
  "role": "TX",
  
  "mesh": {
    "is_root": false,
    "layer": 2,
    "parent_id": "RX-B1C4",
    "parent_rssi": -65,
    "children": ["RX-E5D3", "RELAY-F2A1"],
    "children_count": 2
  },
  
  "audio": {
    "mode": "AUX",
    "streaming": true,
    "stream_id": 1,
    "codec": "PCM",
    "sample_rate": 48000,
    "channels": 1,
    "frame_size_ms": 5,
    "buffer_fill": 0.65,
    "buffer_size": 10,
    "underruns": 2,
    "overruns": 0,
    "latency_ms": 34,
    "level_db": -18.5
  },
  
  "system": {
    "uptime_ms": 3845123,
    "cpu_usage": 0.42,
    "heap_free": 187234,
    "heap_total": 327680,
    "temperature": 58,
    "state": "STREAMING"
  },
  
  "streams": [
    {
      "stream_id": 1,
      "tx_node_id": "TX-A3F2",
      "level_db": -12.3,
      "packet_loss": 0.002,
      "latency_ms": 34
    }
  ],
  
  "errors": [
    {
      "type": "AUDIO_UNDERRUN",
      "count": 2,
      "last_timestamp": 1699012344000
    }
  ]
}
```

**Schema Version Policy:**
- **Major version (1.x.x):** Breaking changes (field removed/renamed)
- **Minor version (x.1.x):** New fields added (backward compatible)
- **Patch version (x.x.1):** Documentation/clarification only

**Implementation:**
```c
// lib/control/include/control/telemetry.h

#define TELEMETRY_SCHEMA_VERSION "1.0.0"

typedef struct {
    char version[16];
    uint64_t timestamp;
    char node_id[16];
    char node_name[32];
    node_role_t role;
    
    struct {
        bool is_root;
        uint8_t layer;
        char parent_id[16];
        int8_t parent_rssi;
        char children[MAX_CHILDREN][16];
        uint8_t children_count;
    } mesh;
    
    struct {
        input_mode_t mode;
        bool streaming;
        uint8_t stream_id;
        codec_mode_t codec;
        uint32_t sample_rate;
        uint8_t channels;
        uint8_t frame_size_ms;
        float buffer_fill;       // 0.0-1.0
        uint8_t buffer_size;
        uint32_t underruns;
        uint32_t overruns;
        uint32_t latency_ms;
        float level_db;
    } audio;
    
    struct {
        uint64_t uptime_ms;
        float cpu_usage;         // 0.0-1.0
        uint32_t heap_free;
        uint32_t heap_total;
        int16_t temperature;
        system_state_t state;
    } system;
    
    struct {
        uint8_t stream_id;
        char tx_node_id[16];
        float level_db;
        float packet_loss;       // 0.0-1.0
        uint32_t latency_ms;
    } streams[MAX_ACTIVE_STREAMS];
    uint8_t stream_count;
    
    struct {
        error_type_t type;
        uint32_t count;
        uint64_t last_timestamp;
    } errors[MAX_ERROR_TYPES];
    uint8_t error_count;
} telemetry_snapshot_t;

// Telemetry management
esp_err_t telemetry_init(void);
esp_err_t telemetry_update_own(void);                    // Poll network/audio layers
telemetry_snapshot_t* telemetry_get_own(void);           // Local node state
telemetry_snapshot_t* telemetry_get_node(const char *node_id);  // Cached remote state

// JSON encoding/decoding
esp_err_t telemetry_encode_json(const telemetry_snapshot_t *snapshot, char *json_buf, size_t buf_size, size_t *json_len);
esp_err_t telemetry_decode_json(const char *json, size_t json_len, telemetry_snapshot_t *snapshot);

// Compression (gzip)
esp_err_t telemetry_compress(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_size, size_t *output_len);
esp_err_t telemetry_decompress(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_size, size_t *output_len);
```

### State Cache (Distributed Mesh View)

**Purpose:** Each node maintains snapshot of entire mesh

**Architecture:**
- Hash map: `node_id → telemetry_snapshot_t`
- Age-out stale entries (120 seconds without update)
- Lock-free reads (FreeRTOS mutex for writes)

**Memory Footprint:**
```
10 nodes × 500 bytes (compressed JSON) = 5 KB
Plus hash table overhead: ~2 KB
Total: ~7 KB per node
```

**Implementation:**
```c
// lib/control/include/control/state_cache.h

#define STATE_CACHE_MAX_NODES 32
#define STATE_CACHE_TTL_MS 120000  // 2 minutes

typedef struct {
    char node_id[16];
    telemetry_snapshot_t snapshot;
    uint64_t last_update_ms;
    bool is_stale;
} cached_node_state_t;

esp_err_t state_cache_init(void);
esp_err_t state_cache_update(const char *node_id, const telemetry_snapshot_t *snapshot);
telemetry_snapshot_t* state_cache_get(const char *node_id);
esp_err_t state_cache_get_all(cached_node_state_t *nodes, size_t max_nodes, size_t *node_count);
void state_cache_expire_stale(void);  // Called periodically
void state_cache_clear(void);
```

### Telemetry Publisher (Mesh Broadcasts)

**Purpose:** Share own state with all other nodes via mesh control messages

**Broadcast Frequency:**
- **Dynamic data (high-rate):** 1 Hz (buffer levels, RSSI, audio level)
- **Static data (low-rate):** 0.1 Hz or on-change (topology, mode, errors)

**Compression:**
- JSON → gzip (typically 60% reduction: 500 bytes → 200 bytes)

**Mesh Control Message Format:**
```c
typedef struct __attribute__((packed)) {
    uint8_t magic;               // 0xC0 (CONTROL_FRAME_MAGIC)
    uint8_t version;             // 1
    uint8_t type;                // CTRL_MSG_TELEMETRY
    uint8_t flags;               // BIT0: compressed, BIT1: requires_ack
    uint16_t payload_len;        // Compressed JSON length
    char sender_node_id[16];     // For deduplication
    uint32_t sequence;           // Monotonic counter
    // Payload follows (compressed JSON)
} mesh_control_header_t;

typedef enum {
    CTRL_MSG_HEARTBEAT,          // Periodic keepalive
    CTRL_MSG_TELEMETRY,          // Full state snapshot
    CTRL_MSG_MIXER_COMMAND,      // Mixer control (v0.3)
    CTRL_MSG_TIME_SYNC,          // Clock synchronization
    CTRL_MSG_STREAM_ANNOUNCE     // TX announces new stream
} mesh_control_type_t;
```

**API:**
```c
// lib/control/include/control/telemetry_publisher.h

esp_err_t telemetry_publisher_init(void);
esp_err_t telemetry_publisher_start(void);   // Begin periodic broadcasts
esp_err_t telemetry_publisher_stop(void);
esp_err_t telemetry_publisher_send_now(void);  // Immediate broadcast (on-demand)
void telemetry_publisher_set_rate(uint32_t interval_ms);  // Adjust frequency
```

**Bandwidth Impact:**
```
10 nodes × 200 bytes (compressed) × 1 Hz = 2 KB/sec = 16 kbps
Audio stream: 1152 kbps
Control overhead: 1.4% (negligible)
```

### Time Synchronization

**Purpose:** All timestamps use common reference for latency measurement

**Options:**

**v0.1 - Simple: Boot-relative timestamps**
```c
uint64_t now_ms = esp_timer_get_time() / 1000;  // Milliseconds since boot
// Each node has independent clock (drift acceptable for v0.1)
```

**v0.2 - Mesh Time Sync Protocol**
- Root node is time authority
- Broadcast sync messages every 10 seconds
- Child nodes adjust offset to match root
- Precision: ~10ms (sufficient for audio latency measurement)

**v0.3 - SNTP (if internet gateway available)**
- One node fetches NTP time
- Shares absolute time via mesh sync
- Precision: ~1ms

**API (v0.2+):**
```c
// lib/control/include/control/time_sync.h

esp_err_t time_sync_init(void);
uint64_t time_sync_get_network_time_ms(void);   // Synchronized timestamp
int32_t time_sync_get_offset_ms(void);          // Offset from root clock
bool time_sync_is_synchronized(void);
```

## Layer 4: Mesh Control Messages (Internal Sync)

### Control Message Architecture

**Purpose:** High-priority, low-latency communication between nodes (not audio data)

**Message Types:**

**1. Heartbeat (every 2 seconds)**
```c
typedef struct __attribute__((packed)) {
    char node_id[16];
    node_role_t role;
    system_state_t state;
    uint32_t uptime_ms;
} heartbeat_payload_t;
```

**2. Telemetry Broadcast (every 1 second)**
```c
typedef struct __attribute__((packed)) {
    char node_id[16];
    uint8_t compressed_json[];  // Variable length
} telemetry_payload_t;
```

**3. Mixer Command (v0.3 - on-demand)**
```c
typedef struct __attribute__((packed)) {
    uint8_t stream_id;
    float gain;        // 0.0-2.0 (0-200%)
    float pan;         // -1.0 to 1.0 (L to R)
    bool mute;
    uint16_t hpf_hz;   // High-pass cutoff
    uint16_t lpf_hz;   // Low-pass cutoff
} mixer_command_payload_t;
```

**4. Time Sync (v0.2 - every 10 seconds)**
```c
typedef struct __attribute__((packed)) {
    char root_node_id[16];
    uint64_t root_timestamp_ms;
    uint32_t propagation_delay_ms;
} time_sync_payload_t;
```

### Sending Control Messages

**Integration with ESP-WIFI-MESH:**
```c
// lib/control/src/mesh_control.c

esp_err_t mesh_control_send(mesh_control_type_t type, const uint8_t *payload, size_t len) {
    mesh_control_header_t header;
    header.magic = CONTROL_FRAME_MAGIC;
    header.version = 1;
    header.type = type;
    header.flags = 0;  // No compression for small messages
    header.payload_len = len;
    strncpy(header.sender_node_id, my_node_id, 16);
    header.sequence = control_seq++;
    
    // Build packet
    uint8_t packet[sizeof(header) + len];
    memcpy(packet, &header, sizeof(header));
    memcpy(packet + sizeof(header), payload, len);
    
    // Send via mesh (broadcast to all nodes)
    mesh_data_t data;
    data.data = packet;
    data.size = sizeof(packet);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;  // High priority
    
    return esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
}
```

### Receiving Control Messages

**Dedicated RX task (separate from audio):**
```c
static void mesh_control_rx_task(void *arg) {
    mesh_addr_t from;
    mesh_data_t data;
    uint8_t rx_buffer[512];
    
    while (1) {
        data.data = rx_buffer;
        data.size = sizeof(rx_buffer);
        
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) continue;
        
        // Parse header
        mesh_control_header_t *header = (mesh_control_header_t*)data.data;
        if (header->magic != CONTROL_FRAME_MAGIC) continue;
        
        // Dispatch by type
        switch (header->type) {
            case CTRL_MSG_HEARTBEAT:
                handle_heartbeat(header, data.data + sizeof(*header));
                break;
            case CTRL_MSG_TELEMETRY:
                handle_telemetry_broadcast(header, data.data + sizeof(*header));
                break;
            case CTRL_MSG_MIXER_COMMAND:
                handle_mixer_command(header, data.data + sizeof(*header));
                break;
            case CTRL_MSG_TIME_SYNC:
                handle_time_sync(header, data.data + sizeof(*header));
                break;
        }
    }
}
```

**Telemetry Handler:**
```c
static void handle_telemetry_broadcast(mesh_control_header_t *header, uint8_t *payload) {
    // Decompress if needed
    uint8_t json_buf[1024];
    size_t json_len;
    
    if (header->flags & CTRL_FLAG_COMPRESSED) {
        telemetry_decompress(payload, header->payload_len, json_buf, sizeof(json_buf), &json_len);
    } else {
        memcpy(json_buf, payload, header->payload_len);
        json_len = header->payload_len;
    }
    
    // Parse JSON
    telemetry_snapshot_t snapshot;
    telemetry_decode_json((char*)json_buf, json_len, &snapshot);
    
    // Update state cache
    state_cache_update(header->sender_node_id, &snapshot);
    
    ESP_LOGI(TAG, "Updated cache for node %s", header->sender_node_id);
}
```

## Layer 5: External Portal (Observer Gateway)

### USB Networking (CDC-ECM)

**Purpose:** Plug USB cable into node → node becomes network interface → access web UI

**TinyUSB CDC-ECM Configuration:**
```c
// Enable in sdkconfig
CONFIG_TINYUSB_ENABLED=y
CONFIG_TINYUSB_NET_MODE_ECM=y

// Network configuration
#define RNDIS_IP_ADDR "10.48.0.1"
#define RNDIS_NETMASK "255.255.255.0"
#define RNDIS_GATEWAY "10.48.0.1"

// Host sees node at 10.48.0.1
// Web UI accessible at http://10.48.0.1/
```

**Implementation:**
```c
// lib/control/src/usb_networking.c

esp_err_t usb_networking_init(void) {
    // Configure TinyUSB CDC-ECM descriptor
    tusb_desc_device_t desc = {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = TUSB_CLASS_MISC,
        .bDeviceSubClass = MISC_SUBCLASS_COMMON,
        .bDeviceProtocol = MISC_PROTOCOL_IAD,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = 0xCAFE,  // Your vendor ID
        .idProduct = 0x4001,
        .bcdDevice = 0x0100,
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x03,
        .bNumConfigurations = 0x01
    };
    
    // Initialize TinyUSB with CDC-ECM
    tinyusb_config_t usb_cfg = {
        .device_descriptor = &desc,
        .string_descriptor = ecm_string_desc,
        .external_phy = false,
    };
    
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb_cfg));
    
    // Configure IP address
    esp_netif_t *netif = esp_netif_new(&ecm_netif_config);
    esp_netif_set_ip_info(netif, &ecm_ip_info);
    
    ESP_LOGI(TAG, "USB networking ready at %s", RNDIS_IP_ADDR);
    return ESP_OK;
}
```

**User Experience:**
1. Plug USB-C cable from node to computer/phone
2. OS detects network adapter (drivers built-in for Win/Mac/Linux)
3. Computer auto-assigns IP via DHCP (node is gateway at 10.48.0.1)
4. Open browser: `http://10.48.0.1/` → Web UI loads

### HTTP REST API

**Purpose:** Read mesh state via HTTP (static snapshot)

**Endpoints:**

**GET `/api/status`** - Own node telemetry
```json
{
  "version": "1.0.0",
  "timestamp": 1699012345678,
  "node_id": "TX-A3F2",
  ...
}
```

**GET `/api/mesh/nodes`** - All cached nodes
```json
{
  "nodes": [
    {"node_id": "TX-A3F2", "role": "TX", "is_root": false, ...},
    {"node_id": "RX-B1C4", "role": "RX", "is_root": true, ...}
  ],
  "total_nodes": 5,
  "cached_at": 1699012345678
}
```

**GET `/api/mesh/topology`** - Tree structure
```json
{
  "root": {
    "node_id": "RX-B1C4",
    "children": [
      {
        "node_id": "TX-A3F2",
        "children": [
          {"node_id": "RX-E5D3", "children": []}
        ]
      }
    ]
  }
}
```

**GET `/api/streams`** - Active audio streams
```json
{
  "streams": [
    {
      "stream_id": 1,
      "tx_node_id": "TX-A3F2",
      "rx_nodes": ["RX-B1C4", "RX-E5D3"],
      "codec": "PCM",
      "sample_rate": 48000
    }
  ]
}
```

**POST `/api/mixer/command`** - Mixer control (v0.3)
```json
{
  "stream_id": 1,
  "gain": 0.8,
  "pan": -0.5,
  "mute": false
}
```

**Implementation:**
```c
// lib/control/src/http_server.c

esp_err_t http_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    
    // Register URI handlers
    httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handle_api_status,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_status);
    
    // ... register other endpoints
    
    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}

static esp_err_t handle_api_status(httpd_req_t *req) {
    // Get own telemetry
    telemetry_snapshot_t *snapshot = telemetry_get_own();
    
    // Encode to JSON
    char json_buf[2048];
    size_t json_len;
    telemetry_encode_json(snapshot, json_buf, sizeof(json_buf), &json_len);
    
    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buf, json_len);
    return ESP_OK;
}
```

### WebSocket Server (Real-Time Push)

**Purpose:** Stream dynamic metrics to connected observers (<1 second latency)

**Protocol:**
- Client connects to `ws://10.48.0.1/ws`
- Server pushes telemetry updates as they arrive (from mesh broadcasts)
- Client subscribes to specific node IDs or all nodes

**Message Format:**
```json
{
  "type": "telemetry_update",
  "node_id": "TX-A3F2",
  "timestamp": 1699012345678,
  "delta": {
    "audio.buffer_fill": 0.72,
    "audio.level_db": -14.2,
    "system.cpu_usage": 0.38
  }
}
```

**Delta Updates (bandwidth optimization):**
- Only send changed fields
- Full snapshot every 10 seconds
- Delta updates in between

**Implementation:**
```c
// lib/control/src/websocket_server.c

typedef struct {
    int fd;
    char subscribed_node_ids[MAX_SUBSCRIPTIONS][16];
    uint8_t subscription_count;
} ws_client_t;

static ws_client_t active_clients[MAX_WS_CLIENTS];

esp_err_t websocket_server_init(httpd_handle_t http_server) {
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true
    };
    httpd_register_uri_handler(http_server, &ws);
    
    // Start broadcast task
    xTaskCreate(ws_broadcast_task, "ws_broadcast", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static void ws_broadcast_task(void *arg) {
    while (1) {
        // Wait for telemetry update event
        xEventGroupWaitBits(telemetry_events, TELEMETRY_UPDATED, pdTRUE, pdFALSE, portMAX_DELAY);
        
        // Get updated snapshot
        telemetry_snapshot_t *snapshot = telemetry_get_own();
        
        // Encode as JSON delta
        char json_buf[512];
        encode_telemetry_delta(snapshot, json_buf, sizeof(json_buf));
        
        // Broadcast to all connected clients
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (active_clients[i].fd > 0) {
                httpd_ws_frame_t ws_pkt;
                ws_pkt.payload = (uint8_t*)json_buf;
                ws_pkt.len = strlen(json_buf);
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                
                httpd_ws_send_frame_async(http_server, active_clients[i].fd, &ws_pkt);
            }
        }
    }
}
```

### Web UI (Astro-Based)

**Purpose:** Browser-based dashboard for mixer, meters, topology visualization

**Technology Stack:**
- **Framework:** Astro (static site generation + islands architecture)
- **UI Components:** Svelte islands (for interactive mixer faders, meters)
- **Styling:** Tailwind CSS (responsive design)
- **Charts:** D3.js or Chart.js (topology graph, level meters)
- **WebSocket Client:** Native WebSocket API

**Pages:**

**1. Dashboard (`/index.html`)**
- Overview: Total nodes, mesh health, active streams
- Quick stats cards (CPU, heap, uptime)
- Stream activity indicators

**2. Topology View (`/topology.html`)**
- Tree graph visualization (root → children)
- Node colors by role (TX green, RX blue, RELAY yellow)
- Click node → show detailed telemetry popup

**3. Mixer Console (`/mixer.html`)** (v0.3)
- Per-stream faders (gain, pan)
- VU meters (real-time audio levels via WebSocket)
- Mute/solo buttons
- HPF/LPF controls for subwoofer routing

**4. Metrics (`/metrics.html`)**
- Time-series graphs (CPU, bandwidth, latency)
- Buffer fill visualization
- Error log table

**5. Settings (`/settings.html`)**
- Node name, mesh credentials
- Display brightness, default view
- Factory reset button

**File Structure:**
```
lib/control/web/
├── public/
│   ├── favicon.ico
│   └── assets/
│       ├── logo.svg
│       └── styles.css
├── src/
│   ├── layouts/
│   │   └── Layout.astro
│   ├── pages/
│   │   ├── index.astro         # Dashboard
│   │   ├── topology.astro      # Mesh tree view
│   │   ├── mixer.astro         # Mixer console (v0.3)
│   │   ├── metrics.astro       # Telemetry graphs
│   │   └── settings.astro      # Configuration
│   ├── components/
│   │   ├── NodeCard.svelte     # Telemetry card (interactive)
│   │   ├── TopologyGraph.svelte # D3.js tree visualization
│   │   ├── Fader.svelte        # Mixer fader control (v0.3)
│   │   └── VUMeter.svelte      # Audio level meter
│   └── lib/
│       ├── api.ts              # REST API client
│       └── websocket.ts        # WebSocket client
├── astro.config.mjs
└── package.json
```

**API Client Example (`api.ts`):**
```typescript
export async function getMeshNodes() {
  const response = await fetch('http://10.48.0.1/api/mesh/nodes');
  return await response.json();
}

export async function getTopology() {
  const response = await fetch('http://10.48.0.1/api/mesh/topology');
  return await response.json();
}
```

**WebSocket Client Example (`websocket.ts`):**
```typescript
export class TelemetryClient {
  private ws: WebSocket;
  
  connect() {
    this.ws = new WebSocket('ws://10.48.0.1/ws');
    
    this.ws.onmessage = (event) => {
      const update = JSON.parse(event.data);
      this.handleTelemetryUpdate(update);
    };
  }
  
  subscribe(nodeId: string) {
    this.ws.send(JSON.stringify({ action: 'subscribe', node_id: nodeId }));
  }
  
  private handleTelemetryUpdate(update: any) {
    // Update reactive state (Svelte store, etc.)
    telemetryStore.update(update.node_id, update.delta);
  }
}
```

**Svelte Component Example (`NodeCard.svelte`):**
```svelte
<script>
  export let nodeId;
  let telemetry = $telemetryStore[nodeId];
</script>

<div class="card">
  <h3>{telemetry.node_name} ({nodeId})</h3>
  <p>Role: {telemetry.role}</p>
  <p>CPU: {(telemetry.system.cpu_usage * 100).toFixed(1)}%</p>
  <p>Heap: {(telemetry.system.heap_free / 1024).toFixed(0)} KB</p>
  {#if telemetry.role === 'RX'}
    <p>Buffer: {(telemetry.audio.buffer_fill * 100).toFixed(0)}%</p>
  {/if}
</div>
```

### Build & Deployment

**Build Web UI (development machine):**
```bash
cd lib/control/web
npm install
npm run build  # → dist/ folder (static HTML/CSS/JS)
```

**Embed in Firmware:**
```c
// Use ESP-IDF's SPIFFS or LittleFS to embed web assets
// Files in dist/ → compiled into firmware binary

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t handle_index(httpd_req_t *req) {
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char*)index_html_start, len);
    return ESP_OK;
}
```

## Implementation Roadmap

### v0.1: Local Observability (Current Focus)

**Scope:**
- ✅ Display shows own node + mesh topology
- ✅ Buttons cycle views, change input modes
- ✅ Unified telemetry schema defined
- ✅ Metrics aggregation (pull from network/audio layers)
- ✅ Settings persistence (mesh creds, last mode)
- ✅ Auto-recovery for mesh disconnect

**Files to Create/Modify:**
```
lib/control/
├── include/control/
│   ├── state_machine.h         [NEW]
│   ├── settings.h              [NEW]
│   ├── metrics.h               [NEW]
│   └── telemetry.h             [NEW]
├── src/
│   ├── state_machine.c         [NEW]
│   ├── settings.c              [NEW]
│   ├── metrics.c               [NEW]
│   ├── telemetry.c             [NEW]
│   └── display_ssd1306.c       [MODIFY - add topology/metrics views]
```

**Testing:**
- Boot 3 nodes, verify each shows correct topology
- Disconnect parent node, verify auto-recovery
- Power cycle, verify settings restored from NVS

### v0.2: Internal Mesh Sync

**Scope:**
- 🔜 Mesh control messages (heartbeat, telemetry broadcast)
- 🔜 State cache (all nodes cache full mesh view)
- 🔜 Display shows **any** node's metrics (not just own)
- 🔜 Time synchronization (mesh-wide clock)
- 🔜 Telemetry compression (gzip JSON)

**Files to Create:**
```
lib/control/
├── include/control/
│   ├── state_cache.h
│   ├── telemetry_publisher.h
│   ├── mesh_control.h
│   └── time_sync.h
├── src/
│   ├── state_cache.c
│   ├── telemetry_publisher.c
│   ├── mesh_control.c
│   └── time_sync.c
```

**Testing:**
- Boot 5 nodes, verify each caches all 5 states
- Kill one node, verify others age out stale entry
- Measure bandwidth overhead (<5% of audio)
- Verify time sync accuracy (<50ms drift)

### v0.3: External Portal

**Scope:**
- 🔮 USB networking (CDC-ECM) - plug in and access web UI
- 🔮 HTTP REST API (read mesh state)
- 🔮 WebSocket server (real-time telemetry push)
- 🔮 Web UI (Astro + Svelte)
- 🔮 Mixer control (gain, pan, mute per stream)
- 🔮 LED indicators

**Files to Create:**
```
lib/control/
├── include/control/
│   ├── usb_networking.h
│   ├── http_server.h
│   ├── websocket_server.h
│   ├── mixer_control.h
│   └── leds.h
├── src/
│   ├── usb_networking.c
│   ├── http_server.c
│   ├── websocket_server.c
│   ├── mixer_control.c
│   └── leds.c
├── web/                        [NEW - Astro project]
│   ├── src/pages/
│   │   ├── index.astro
│   │   ├── topology.astro
│   │   ├── mixer.astro
│   │   ├── metrics.astro
│   │   └── settings.astro
│   └── src/components/
│       ├── NodeCard.svelte
│       ├── TopologyGraph.svelte
│       ├── Fader.svelte
│       └── VUMeter.svelte
```

**Testing:**
- Plug USB into node, verify network interface appears
- Access http://10.48.0.1/, verify dashboard loads
- Connect WebSocket, verify real-time level meters
- Adjust mixer fader, verify gain applied to stream
- Test on Windows, Mac, Linux, Android (USB OTG)

## Performance Metrics

### Memory Footprint (per node)

| Component | RAM Usage | Notes |
|-----------|-----------|-------|
| Telemetry schema | 1 KB | Own node snapshot |
| State cache (10 nodes) | 7 KB | Compressed JSON + hash table |
| Control message queue | 4 KB | 8 messages × 512 bytes |
| HTTP server | 8 KB | ESP-IDF httpd (if enabled) |
| WebSocket buffers | 4 KB | 2 clients × 2 KB |
| **Total (v0.1)** | **12 KB** | No external portal |
| **Total (v0.3)** | **24 KB** | With HTTP/WS |

### CPU Usage (estimated)

| Task | Frequency | CPU % |
|------|-----------|-------|
| Telemetry update | 1 Hz | <1% |
| JSON encoding | 1 Hz | <2% |
| gzip compression | 1 Hz | <3% |
| State cache lookup | On-demand | <1% |
| Display rendering | 10 Hz | <5% |
| HTTP request handling | On-demand | <2% |
| WebSocket broadcast | 1 Hz | <2% |
| **Total** | - | **<15%** |

### Bandwidth (control layer only)

| Message Type | Size | Frequency | Bandwidth |
|--------------|------|-----------|-----------|
| Heartbeat | 50 bytes | 0.5 Hz | 200 bps |
| Telemetry (compressed) | 200 bytes | 1 Hz | 1.6 kbps |
| Mixer command | 32 bytes | On-demand | <100 bps |
| Time sync | 64 bytes | 0.1 Hz | 51 bps |
| **Total per node** | - | - | **~2 kbps** |
| **10-node mesh** | - | - | **~20 kbps** |

**Percentage of audio bandwidth:** 20 kbps / 1152 kbps = **1.7%** (negligible)

## Configuration Files

### sdkconfig additions

```ini
# Control layer
CONFIG_CONTROL_TELEMETRY_ENABLE=y
CONFIG_CONTROL_TELEMETRY_COMPRESSION=y
CONFIG_CONTROL_TELEMETRY_RATE_HZ=1
CONFIG_CONTROL_STATE_CACHE_SIZE=32

# USB networking (v0.3)
CONFIG_TINYUSB_ENABLED=y
CONFIG_TINYUSB_NET_MODE_ECM=y

# HTTP server (v0.3)
CONFIG_ESP_HTTP_SERVER_ENABLE=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=512
CONFIG_HTTPD_MAX_URI_LEN=512

# WebSocket (v0.3)
CONFIG_HTTPD_WS_SUPPORT=y

# NVS (settings persistence)
CONFIG_NVS_ENCRYPTION=n  # Optional: encrypt settings
```

### build.h additions

```c
// Control layer configuration
#define CONTROL_TELEMETRY_RATE_MS    1000   // 1 Hz
#define CONTROL_HEARTBEAT_RATE_MS    2000   // 0.5 Hz
#define CONTROL_STATE_CACHE_TTL_MS   120000 // 2 minutes
#define CONTROL_STATE_CACHE_MAX_NODES 32

// USB networking
#define USB_ECM_IP_ADDR     "10.48.0.1"
#define USB_ECM_NETMASK     "255.255.255.0"

// HTTP server
#define HTTP_SERVER_PORT    80
#define WS_MAX_CLIENTS      4
```

## Known Limitations

### v0.1 Limitations
- No remote access (display-only observability)
- Time sync not implemented (timestamps relative to boot)
- No mixer control
- Single button interface (limited input)

### v0.2 Limitations
- Mesh control messages add 2-3% bandwidth overhead
- State cache limited to 32 nodes (memory constraint)
- Time sync accuracy ~10-50ms (sufficient for audio latency measurement)
- No external access yet

### v0.3 Limitations
- USB networking requires driver (built-in for Win/Mac/Linux, may need app for iOS)
- Web UI requires modern browser (ES6+, WebSocket support)
- Only one USB connection at a time (could support multiple WebSocket clients)
- Mixer applies globally or per-RX (no per-user sessions)

## Future Enhancements

### v0.4: Advanced UI
- Mobile app (React Native - iOS/Android)
- Desktop app (Electron - Win/Mac/Linux)
- VR/AR spatial audio mixer (Quest, Vision Pro)
- MIDI controller support (physical faders → mixer commands)

### v0.5: Cloud Integration
- MQTT bridge to cloud broker (AWS IoT, HiveMQ)
- Cloud dashboard (view mesh from anywhere)
- Firmware OTA updates via cloud
- Usage analytics, crash reporting

### v0.6: AI/ML Features
- Automatic gain adjustment (normalize levels across streams)
- Acoustic room modeling (reverb, EQ based on RX location)
- Predictive buffering (anticipate network jitter)
- Anomaly detection (alert on unusual patterns)

## References

### ESP-IDF Documentation
- [HTTP Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html)
- [WebSocket Support](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/http-server.html#websocket)
- [TinyUSB CDC-ECM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
- [NVS (Non-Volatile Storage)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_flash.html)

### Web Technologies
- [Astro Framework](https://astro.build/)
- [Svelte](https://svelte.dev/)
- [D3.js Visualization](https://d3js.org/)
- [WebSocket API](https://developer.mozilla.org/en-US/docs/Web/API/WebSocket)

### JSON & Compression
- [cJSON Library](https://github.com/DaveGamble/cJSON) (ESP-IDF built-in)
- [miniz (zlib/gzip)](https://github.com/richgel999/miniz)

---

*Document created for control layer architecture planning. Defines unified observability, external portal, and phased implementation roadmap.*
