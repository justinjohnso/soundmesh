# MeshNet Audio: True Mesh Network Architecture

**Date:** November 3, 2025  
**Status:** Architecture Design & Implementation Plan  
**Goal:** Migrate from star topology (AP/STA) to self-healing mesh with automatic root election

## Executive Summary

This document outlines the migration from the current star topology (1 TX AP → N RX STAs) to a true self-healing mesh network using ESP-WIFI-MESH. The new architecture supports:

- **Any number of nodes** joining/leaving dynamically
- **Automatic root election** (first node becomes root, typically RX)
- **Root migration** when nodes leave (no single point of failure)
- **Multi-hop relay** for extended range
- **Tree broadcast** for efficient 1-to-many audio distribution
- **Publisher independence** - TX nodes don't need to be mesh root

## Current Architecture vs Mesh

### Current: Star Topology (Single Point of Failure)

```
        TX (AP - Fixed Root)
        ├─ 192.168.4.1
        └─ UDP Broadcast → 192.168.4.255:3333
              ↓
    ┌─────────┼─────────┐
   RX1       RX2       RX3
  (STA)     (STA)     (STA)
  192.4.2   192.4.3   192.4.4

Issues:
- TX dies → entire network collapses
- No multi-hop (limited range)
- TX must always be AP
- Manual IP management
```

### Target: Self-Healing Mesh (Resilient Tree)

```
        Root (Auto-elected, usually first RX)
        ├─ Child: TX node (publishes audio)
        │   └─ Child: RX3 (leaf, 2 hops from root)
        ├─ Child: RX1 (receives + relays)
        │   └─ Child: RX4 (leaf)
        └─ Child: RX2 (leaf)

Features:
- Root leaves → ESP-MESH auto-elects new root
- TX can be anywhere in tree (not tied to root)
- Multi-hop extends range
- Self-organizing topology
- No manual IP/DHCP management
```

## ESP-WIFI-MESH Fundamentals

### What ESP-WIFI-MESH Provides

**Automatic Capabilities:**
1. **Self-Organization** - Nodes discover each other and form tree
2. **Root Election** - First node becomes root, re-election on failure
3. **Parent Selection** - Nodes choose best parent based on RSSI/routing
4. **Multi-Hop Routing** - Packets forwarded through intermediate nodes
5. **Self-Healing** - Topology reconfigures when nodes join/leave
6. **No IP Layer** - Uses MAC addressing (no DHCP complexity)

**Configuration:**
- **Mesh ID** - Unique identifier (replaces SSID)
- **Mesh Password** - WPA2-PSK security
- **Channel** - Fixed channel (e.g., 6) for all nodes
- **Max Layers** - Depth limit (e.g., 6 for ESP-MESH)
- **Max Connections** - Children per node (e.g., 10)

### What We Must Implement

**Application Layer:**
1. **Role Advertisement** - Nodes declare TX vs RX capability
2. **Audio Stream Announcement** - TX publishes stream metadata
3. **Tree Broadcast Logic** - Forward frames to children (avoid duplicates)
4. **Jitter Buffer Management** - Already implemented, reuse as-is
5. **Metrics Collection** - Latency, hop count, packet loss

## Mesh Network Protocol Design

### 1. Control Plane (Small Messages, High Priority)

**Heartbeat Packet (every 2 seconds):**
```c
typedef struct __attribute__((packed)) {
    uint8_t type;           // 0x01 = HEARTBEAT
    uint8_t role;           // 0=RX, 1=TX
    uint8_t is_root;        // 1 if this node is mesh root
    uint8_t layer;          // Hop count from root
    uint32_t uptime_ms;     // Milliseconds since boot
    uint16_t children_count; // Number of mesh children
    int8_t rssi;            // RSSI to parent
} mesh_heartbeat_t;
```

**Stream Announcement (on TX startup):**
```c
typedef struct __attribute__((packed)) {
    uint8_t type;           // 0x02 = STREAM_ANNOUNCE
    uint8_t stream_id;      // Unique ID for this audio stream
    uint32_t sample_rate;   // 48000
    uint8_t channels;       // 2 (stereo)
    uint8_t bits_per_sample; // 16
    uint16_t frame_size_ms; // 5
} mesh_stream_announce_t;
```

**Control Messages Use:**
- `esp_mesh_send()` with flag `MESH_DATA_P2P` (unicast to parent/root)
- Small payloads (<100 bytes)
- TOS: High priority

### 2. Data Plane (Audio Frames, Lower Priority)

**Audio Frame Format (5ms frames):**
```c
typedef struct __attribute__((packed)) {
    uint8_t magic;          // 0xA5 (NET_FRAME_MAGIC)
    uint8_t version;        // 1
    uint8_t type;           // NET_PKT_TYPE_AUDIO_RAW
    uint8_t stream_id;      // Stream identifier (multi-TX support)
    uint16_t seq;           // Sequence number (network byte order)
    uint32_t timestamp;     // Sender timestamp in ms
    uint16_t payload_len;   // 960 bytes for 5ms @ 48kHz stereo
    uint8_t ttl;            // Hop limit (e.g., 6)
    uint8_t reserved;       // Alignment padding
    // Total: 12 bytes header
    uint8_t payload[960];   // 5ms of 48kHz 16-bit stereo PCM
} mesh_audio_frame_t;
```

**Frame Size Calculation:**
- 5ms @ 48kHz = 240 samples/channel
- 240 samples × 2 channels × 2 bytes = **960 bytes**
- Header: 12 bytes
- **Total: 972 bytes** (well under mesh MTU ~1400 bytes)

**Why 5ms instead of 10ms:**
- Lower latency per hop (5ms vs 10ms)
- Smaller packets easier to forward
- Still 200 packets/sec (manageable)
- Fits comfortably in mesh payload limits

### 3. Tree Broadcast Algorithm

**Core Logic (runs on every node receiving audio):**

```c
// Duplicate suppression cache
typedef struct {
    uint8_t stream_id;
    uint16_t seq;
    uint32_t timestamp_ms;
} recent_frame_t;

#define DEDUPE_CACHE_SIZE 32
static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];
static int dedupe_index = 0;

bool is_duplicate(uint8_t stream_id, uint16_t seq) {
    for (int i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        if (dedupe_cache[i].stream_id == stream_id && 
            dedupe_cache[i].seq == seq) {
            return true;  // Already seen this frame
        }
    }
    return false;
}

void mark_seen(uint8_t stream_id, uint16_t seq) {
    dedupe_cache[dedupe_index].stream_id = stream_id;
    dedupe_cache[dedupe_index].seq = seq;
    dedupe_cache[dedupe_index].timestamp_ms = esp_timer_get_time() / 1000;
    dedupe_index = (dedupe_index + 1) % DEDUPE_CACHE_SIZE;
}

void on_audio_frame_received(mesh_audio_frame_t *frame, mesh_addr_t *sender) {
    // 1. Check for duplicates
    if (is_duplicate(frame->stream_id, ntohs(frame->seq))) {
        return;  // Already forwarded this frame
    }
    
    // 2. Mark as seen
    mark_seen(frame->stream_id, ntohs(frame->seq));
    
    // 3. Enqueue for local playback (if RX role)
    if (local_role == ROLE_RX) {
        ring_buffer_write(jitter_buffer, frame->payload, frame->payload_len);
    }
    
    // 4. Forward to all children except the sender
    mesh_addr_t children[MAX_CHILDREN];
    int num_children = esp_mesh_get_routing_table_size();
    esp_mesh_get_routing_table(children, num_children);
    
    for (int i = 0; i < num_children; i++) {
        if (memcmp(&children[i], sender, 6) != 0) {  // Don't echo back
            // Decrement TTL and forward
            frame->ttl--;
            if (frame->ttl > 0) {
                esp_mesh_send(&children[i], frame, sizeof(*frame), 
                             MESH_DATA_P2P, NULL, 0);
            }
        }
    }
}
```

**Efficiency:**
- One transmission per branch per hop (no flooding)
- De-duplication prevents loops
- TTL prevents infinite relay
- Children list from ESP-MESH routing table

## Root Election Strategy

### ESP-WIFI-MESH Automatic Root Election

**Default Behavior (Self-Organized Mode):**
1. **No existing mesh** → First node becomes root
2. **Existing mesh** → New node joins as child
3. **Root leaves** → ESP-MESH automatically elects new root from children
4. **Election criteria** - RSSI, connection stability, capacity

**Our Approach: Let ESP-MESH Decide (Minimal Intervention)**

```c
// Initialize mesh in self-organized mode
mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
mesh_cfg.channel = 6;
mesh_cfg.mesh_id = MESH_ID;  // Shared identifier
mesh_cfg.router.ssid[0] = 0; // No external router
mesh_cfg.mesh_ap.max_connection = 10;  // Children per node
mesh_cfg.mesh_ap.nonmesh_max_connection = 0;  // No non-mesh STAs

// Self-organized mode (automatic root election)
esp_mesh_set_self_organized(true, false);  // No external router
esp_mesh_set_max_layer(6);  // Max hops
esp_mesh_fix_root(false);   // Allow root migration

esp_mesh_start();
```

**RX-First Preference (Optional Enhancement):**
```c
// On boot, if no mesh beacons detected after 5 seconds
if (scan_for_mesh_beacons() == 0 && local_role == ROLE_RX) {
    esp_mesh_fix_root(true);   // Lock as root temporarily
    ESP_LOGI(TAG, "No mesh found - becoming root (RX preference)");
}

// Later, when other nodes join, release root lock
// ESP-MESH will handle migration if a better root candidate appears
```

### Root Migration Handling

**Events to Handle:**
```c
case MESH_EVENT_ROOT_GOT_IP:
    ESP_LOGI(TAG, "I am now the mesh root");
    is_root = true;
    update_display();
    break;

case MESH_EVENT_ROOT_LOST:
    ESP_LOGI(TAG, "Lost root status - rejoining as child");
    is_root = false;
    update_display();
    break;

case MESH_EVENT_ROOT_SWITCH_REQ:
    ESP_LOGI(TAG, "Root switch requested by mesh");
    // Allow switch (return ESP_OK)
    break;
```

**Key Principle:** Publisher (TX) is independent of root role. Audio flows through mesh regardless of who is root.

## Network API Design

### Public API (Backend-Agnostic)

```c
// lib/network/include/network/mesh_net.h

typedef enum {
    NETWORK_BACKEND_STAR,   // Current AP/STA + UDP broadcast
    NETWORK_BACKEND_MESH    // ESP-WIFI-MESH
} network_backend_t;

typedef enum {
    NODE_ROLE_RX = 0,       // Receiver (consumes audio)
    NODE_ROLE_TX = 1        // Transmitter (publishes audio)
} node_role_t;

// Initialization
esp_err_t network_init(node_role_t role, network_backend_t backend);

// Audio transmission (TX nodes only)
esp_err_t network_send_audio(const uint8_t *frame, size_t len);

// Control messages (any node)
esp_err_t network_send_control(const uint8_t *data, size_t len);

// Audio reception callback (RX nodes)
typedef void (*network_audio_callback_t)(const uint8_t *payload, size_t len, 
                                         uint16_t seq, uint32_t timestamp);
esp_err_t network_register_audio_callback(network_audio_callback_t callback);

// Topology queries
bool network_is_root(void);                // Am I the mesh root?
uint32_t network_get_connected_nodes(void); // Total nodes in mesh
uint8_t network_get_layer(void);           // Hops from root
uint32_t network_get_children_count(void); // Direct children

// Metrics (existing API)
int network_get_rssi(void);
uint32_t network_get_latency_ms(void);
bool network_is_stream_ready(void);
```

### Backend Selection (Build-Time)

**In CMakeLists.txt or build flags:**
```cmake
# Select backend
set(NETWORK_BACKEND "MESH")  # or "STAR" for fallback

if(NETWORK_BACKEND STREQUAL "MESH")
    target_sources(network PRIVATE src/network_mesh.c)
    target_compile_definitions(network PRIVATE USE_MESH_BACKEND)
else()
    target_sources(network PRIVATE src/mesh_net.c)
    target_compile_definitions(network PRIVATE USE_STAR_BACKEND)
endif()
```

## Detailed Architecture

### 1. Mesh Formation & Topology

**Node Boot Sequence:**

```
1. Initialize WiFi
2. Configure mesh parameters (ID, password, channel)
3. Start mesh in self-organized mode
4. Scan for existing mesh beacons (5 seconds)
   ├─ No beacons found → Become root (first node)
   └─ Beacons found → Join as child
5. Register event handlers
6. Advertise role (TX or RX) via heartbeat
7. Begin audio processing
```

**Mesh Topology Example (Dynamic):**

```
t=0s:  RX1 boots → becomes root

t=5s:  TX1 boots → joins as child of RX1
       Root: RX1
       └─ TX1 (audio publisher)

t=10s: RX2 boots → joins tree (best parent = RX1 or TX1)
       Root: RX1
       ├─ TX1 (audio publisher)
       └─ RX2

t=15s: RX1 leaves → TX1 or RX2 becomes new root
       Root: TX1 (new root, still publishes audio)
       └─ RX2

t=20s: RX3 boots far away → joins via RX2 (multi-hop)
       Root: TX1
       └─ RX2
          └─ RX3 (2 hops from root)
```

### 2. Audio Streaming Flow

**TX Node (Publisher):**

```c
// Every 5ms in main loop
void tx_main_loop() {
    // Generate/capture 5ms audio frame (960 bytes)
    int16_t stereo_frame[240 * 2];  // 240 samples × 2 channels
    generate_audio(stereo_frame, 240);
    
    // Build mesh audio frame
    mesh_audio_frame_t frame;
    frame.magic = NET_FRAME_MAGIC;
    frame.stream_id = my_stream_id;
    frame.seq = htons(tx_seq++);
    frame.timestamp = esp_timer_get_time() / 1000;
    frame.payload_len = 960;
    frame.ttl = 6;  // Max 6 hops
    memcpy(frame.payload, stereo_frame, 960);
    
    // Broadcast to mesh (toflag = MESH_DATA_TODS for tree distribution)
    network_send_audio(&frame, sizeof(frame));
}
```

**Intermediate Node (Relay):**

```c
// On receiving audio frame
void on_mesh_data_received(mesh_addr_t *from, mesh_data_t *data) {
    mesh_audio_frame_t *frame = (mesh_audio_frame_t *)data->data;
    
    // 1. Validate header
    if (frame->magic != NET_FRAME_MAGIC) return;
    
    // 2. Check for duplicate
    if (is_duplicate(frame->stream_id, ntohs(frame->seq))) {
        return;  // Already processed
    }
    mark_seen(frame->stream_id, ntohs(frame->seq));
    
    // 3. If RX role, enqueue for playback
    if (my_role == ROLE_RX) {
        jitter_buffer_write(frame->payload, frame->payload_len);
    }
    
    // 4. Forward to all children except sender
    forward_to_children(frame, from);
}
```

**Leaf RX Node (Consumer):**

```c
// Same as intermediate, but no children to forward to
void on_mesh_data_received(mesh_addr_t *from, mesh_data_t *data) {
    mesh_audio_frame_t *frame = (mesh_audio_frame_t *)data->data;
    
    if (frame->magic != NET_FRAME_MAGIC) return;
    if (is_duplicate(frame->stream_id, ntohs(frame->seq))) return;
    
    mark_seen(frame->stream_id, ntohs(frame->seq));
    
    // Playback only (no forwarding)
    jitter_buffer_write(frame->payload, frame->payload_len);
}
```

### 3. Bandwidth & Latency Analysis

**Audio Bitrate:**
- 48kHz × 16-bit × 2ch = 1,536,000 bps = **1.5 Mbps**
- 5ms frames: 960 bytes × 200 fps × 8 = **1.536 Mbps**
- Header overhead: 12 bytes × 200 fps × 8 = **19.2 kbps** (negligible)

**Per-Hop Airtime:**
- At 802.11g/n rates (~54 Mbps typical), 972 bytes ≈ **150 μs per packet**
- 200 packets/sec = **30 ms/sec airtime** = **3% utilization per hop**

**Multi-Hop Multiplication:**
- **1 hop:** 1.5 Mbps (single transmission)
- **2 hops:** ~3 Mbps aggregate (root→relay, relay→leaf)
- **3 hops:** ~4.5 Mbps aggregate
- **Tree with 2 branches at hop 2:** ~4.5 Mbps (efficient vs flooding)

**Latency Budget:**
- Per-hop forwarding: **2-5ms** (mesh stack + WiFi)
- Jitter buffer: **40-60ms** (configurable, current implementation)
- Audio generation: **5ms** (frame time)
- **Total (2 hops): ~60-80ms** (well within <100ms target)

**Scalability:**
- **Single hop:** 10+ RX nodes (limited by airtime ~30%)
- **Two hops with 3 branches:** 6-8 RX nodes
- **Three hops:** 4-6 RX nodes (airtime becomes limiting)
- **With Opus compression (128 kbps):** 10x more nodes possible

## Implementation Phases

### Phase 1: Mesh Backend Scaffold (1-3 hours)

**Goal:** Replace AP/STA with ESP-MESH, verify mesh formation

**Tasks:**
1. Create `lib/network/src/network_mesh.c`
2. Implement `network_init()` with ESP-MESH initialization
3. Register mesh event handlers (parent connected, child connected, root switch)
4. Implement heartbeat broadcast (role advertisement)
5. Expose `network_is_root()`, `network_get_layer()`, `network_get_children_count()`

**Testing:**
- Boot 2 nodes, verify mesh forms
- Check logs: first becomes root, second joins as child
- Power off root, verify child becomes new root

**Files Modified:**
- `lib/network/src/network_mesh.c` (new)
- `lib/network/include/network/mesh_net.h` (API additions)
- `CMakeLists.txt` (backend selection)

### Phase 2: Audio Framing & Single-Hop (2-4 hours)

**Goal:** TX publishes audio, RX receives via mesh (1 hop)

**Tasks:**
1. Change frame size from 10ms → 5ms in `config/build.h`
2. Update `AUDIO_FRAME_MS = 5`, `AUDIO_FRAME_SAMPLES = 240`
3. Implement `network_send_audio()` using `esp_mesh_send()` with `MESH_DATA_TODS`
4. Implement `network_register_audio_callback()` for RX
5. Register mesh data receive handler → invoke callback
6. Update TX/RX main loops for new frame size

**Testing:**
- TX generates 5ms tone frames
- RX receives and plays audio
- Verify latency <60ms, no glitches
- Check packet counters match

**Files Modified:**
- `lib/network/src/network_mesh.c` (send/recv implementation)
- `lib/config/include/config/build.h` (frame size constants)
- `src/tx/main.c` (5ms frame generation)
- `src/rx/main.c` (callback integration)

### Phase 3: Tree Broadcast & Multi-Hop (2-4 hours)

**Goal:** Multi-hop forwarding with duplicate suppression

**Tasks:**
1. Implement duplicate suppression cache (LRU, 32 entries)
2. Implement `forward_to_children()` logic
3. Add TTL field to frame header
4. Test 3-node topology (root → relay → leaf)
5. Verify audio reaches leaf, no duplicates, no loops

**Testing:**
- Setup: RX1 (root) ← TX (child) ← RX2 (grandchild)
- TX publishes, verify RX2 receives via relay
- Monitor relay logs: should forward each frame once
- Power off RX1, verify root migration + audio continues

**Files Modified:**
- `lib/network/src/network_mesh.c` (tree broadcast logic)
- `lib/network/include/network/mesh_net.h` (frame format update)

### Phase 4: Root Preference & Metrics (1-2 hours)

**Goal:** Polish root election, surface metrics to UI

**Tasks:**
1. Implement RX-first root preference (fix_root when alone)
2. Release root lock when peers join
3. Update display to show root status, layer, children
4. Implement `network_get_connected_nodes()` - query mesh routing table
5. Latency measurement via ping to parent/root

**Testing:**
- Boot RX first → verify becomes root with indicator
- Boot TX → RX stays root, TX becomes child
- Power cycle scenarios, verify recovery

**Files Modified:**
- `lib/network/src/network_mesh.c` (root preference, metrics)
- `src/tx/main.c` (display mesh status)
- `src/rx/main.c` (display mesh status)
- `lib/control/src/display.c` (add root/layer indicators)

### Phase 5: Integration & Testing (1-2 hours)

**Goal:** End-to-end validation, performance tuning

**Test Scenarios:**
1. **Basic:** 1 TX + 1 RX (verify audio quality)
2. **Multi-RX:** 1 TX + 3 RX (all at 1 hop from root)
3. **Multi-Hop:** RX1 (root) → TX → RX2 → RX3 (3 hops)
4. **Root Migration:** Power off root mid-stream, verify recovery
5. **Publisher Migration:** TX leaves, new TX joins, audio switches
6. **Load Test:** 5+ RX nodes, monitor underruns/drops

**Metrics to Collect:**
- End-to-end latency (TX timestamp → RX playback)
- Packet loss percentage
- Jitter buffer underrun count
- Mesh routing table size
- Airtime utilization (via WiFi stats)

## Configuration Changes

### Network Configuration

**Old (config/build.h):**
```c
#define MESH_SSID              "MeshNet-Audio"
#define MESH_PASSWORD          "meshnet123"
#define UDP_PORT               3333
#define AUDIO_FRAME_MS         10
#define AUDIO_FRAME_SAMPLES    480  // 10ms @ 48kHz
```

**New (config/build.h):**
```c
// Mesh configuration
#define MESH_ID                {0x77, 0x77, 0x77, 0x77, 0x77, 0x77}  // Mesh identifier
#define MESH_PASSWORD          "meshnet123"
#define MESH_CHANNEL           6
#define MESH_MAX_LAYER         6   // Max hops
#define MESH_MAX_CONNECTIONS   10  // Children per node

// Audio configuration (5ms frames for lower latency)
#define AUDIO_FRAME_MS         5
#define AUDIO_FRAME_SAMPLES    240  // 5ms @ 48kHz
#define AUDIO_FRAME_BYTES      960  // 240 × 2ch × 2 bytes

// Backward compatibility (star backend)
#define MESH_SSID              "MeshNet-Audio"  // Used if STAR backend
#define UDP_PORT               3333             // Used if STAR backend
```

### Main Loop Changes (TX)

**Old:**
```c
if (status.audio_active && network_is_stream_ready()) {
    // Send 10ms frame (1920 bytes)
    network_udp_send_audio(framed_buffer, 1932);
}
```

**New:**
```c
if (status.audio_active) {
    // Send 5ms frame (972 bytes) - mesh handles routing
    network_send_audio(framed_buffer, sizeof(mesh_audio_frame_t));
}
// Note: network_is_stream_ready() still used internally for DHCP gating
```

### Main Loop Changes (RX)

**Old:**
```c
// Polling receive every 10ms
esp_err_t ret = network_udp_recv(rx_packet_buffer, MAX_PACKET_SIZE,
                                 &received_len, AUDIO_FRAME_MS);
if (ret == ESP_OK) {
    // Parse and play
}
```

**New:**
```c
// Callback-driven (registered once at init)
void audio_frame_callback(const uint8_t *payload, size_t len, 
                          uint16_t seq, uint32_t timestamp) {
    // Write to jitter buffer (existing code)
    ring_buffer_write(jitter_buffer, payload, len);
    
    // Update stats (existing code)
    packets_received++;
    detect_packet_loss(seq);
}

// In app_main:
network_register_audio_callback(audio_frame_callback);
```

## File Structure Changes

### New Files

```
lib/network/
├── include/network/
│   └── mesh_net.h          (updated with new API)
└── src/
    ├── mesh_net.c          (renamed to network_star.c)
    ├── network_star.c      (AP/STA backend - original code)
    └── network_mesh.c      (NEW - ESP-WIFI-MESH backend)
```

### Modified Files

```
lib/config/include/config/build.h    (frame size, mesh params)
src/tx/main.c                         (5ms frames, mesh API)
src/rx/main.c                         (callback model, 5ms frames)
lib/control/src/display.c            (show root status, layer, hops)
platformio.ini or CMakeLists.txt     (backend selection flag)
```

## ESP-WIFI-MESH API Reference

### Core Initialization

```c
#include <esp_mesh.h>
#include <esp_mesh_internal.h>

// 1. Initialize
esp_mesh_init();

// 2. Configure
mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
mesh_cfg.channel = MESH_CHANNEL;
mesh_cfg.mesh_id = (mesh_addr_t)MESH_ID;
mesh_cfg.mesh_ap.password = MESH_PASSWORD;
mesh_cfg.mesh_ap.max_connection = MESH_MAX_CONNECTIONS;
esp_mesh_set_config(&mesh_cfg);

// 3. Self-organized mode
esp_mesh_set_self_organized(true, false);
esp_mesh_set_max_layer(MESH_MAX_LAYER);
esp_mesh_fix_root(false);  // Allow migration

// 4. Start
esp_mesh_start();
```

### Event Handlers

```c
static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "Mesh started");
            break;
            
        case MESH_EVENT_PARENT_CONNECTED:
            ESP_LOGI(TAG, "Parent connected");
            is_connected = true;
            break;
            
        case MESH_EVENT_PARENT_DISCONNECTED:
            ESP_LOGI(TAG, "Parent disconnected");
            is_connected = false;
            break;
            
        case MESH_EVENT_CHILD_CONNECTED:
            mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
            ESP_LOGI(TAG, "Child connected: " MACSTR, MAC2STR(child->mac));
            update_children_count();
            break;
            
        case MESH_EVENT_CHILD_DISCONNECTED:
            mesh_event_child_disconnected_t *child_disc = (mesh_event_child_disconnected_t *)event_data;
            ESP_LOGI(TAG, "Child disconnected: " MACSTR, MAC2STR(child_disc->mac));
            update_children_count();
            break;
            
        case MESH_EVENT_ROOT_GOT_IP:
            ESP_LOGI(TAG, "Root got IP (not used in pure mesh)");
            is_root = true;
            break;
            
        case MESH_EVENT_ROOT_LOST:
            ESP_LOGI(TAG, "Root lost");
            is_root = false;
            break;
    }
}

// Register
esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL);
```

### Sending Data

```c
// Broadcast to all nodes (tree distribution)
mesh_audio_frame_t frame;
// ... populate frame ...

mesh_data_t data;
data.data = (uint8_t *)&frame;
data.size = sizeof(frame);
data.proto = MESH_PROTO_BIN;  // Binary data
data.tos = MESH_TOS_P2P;       // Low priority for audio

esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
// NULL destination = broadcast to all descendants
```

### Receiving Data

```c
// Receive task (runs continuously)
static void mesh_rx_task(void *arg) {
    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;
    
    while (1) {
        data.size = MAX_PACKET_SIZE;
        data.data = rx_buffer;
        
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        
        if (err == ESP_OK) {
            on_mesh_data_received(&from, &data);
        }
    }
}

// Start receive task
xTaskCreate(mesh_rx_task, "mesh_rx", 4096, NULL, 5, NULL);
```

### Querying Topology

```c
// Am I root?
bool is_root = esp_mesh_is_root();

// My layer (hops from root)
uint8_t layer = esp_mesh_get_layer();

// Number of children
int children_count = esp_mesh_get_routing_table_size();

// Get children addresses
mesh_addr_t children[10];
esp_mesh_get_routing_table(children, 10 * 6, &children_count);

// Parent address
mesh_addr_t parent;
esp_mesh_get_parent_bssid(&parent);
```

## Migration Strategy

### Phased Rollout (Risk Mitigation)

**Week 1: Parallel Development**
- Keep star backend working
- Develop mesh backend alongside
- Both backends share same public API
- Compile-time flag to switch

**Week 2: Mesh Testing**
- Enable mesh backend for testing
- Validate basic formation, audio flow
- Compare latency/quality vs star
- Identify issues

**Week 3: Mesh Refinement**
- Fix discovered issues
- Optimize tree broadcast
- Tune jitter buffer for mesh timing
- Performance testing

**Week 4: Mesh as Default**
- Switch default to mesh backend
- Keep star as fallback option
- Update documentation
- Demonstrate multi-hop capability

### Backward Compatibility

**Configuration Flag:**
```c
// In sdkconfig or build flags
#define NETWORK_BACKEND_MESH 1    // Enable mesh (new)
#define NETWORK_BACKEND_STAR 0    // Fallback to AP/STA (current)
```

**Runtime Selection (Future):**
```c
// Could make this runtime-configurable via button or config file
network_backend_t backend = NETWORK_BACKEND_MESH;
network_init(my_role, backend);
```

## Performance Targets

### Latency

| Scenario | Target | Max Acceptable |
|----------|--------|----------------|
| Single hop (TX→RX) | <30ms | <50ms |
| Two hops (TX→relay→RX) | <60ms | <100ms |
| Three hops | <90ms | <150ms |

### Reliability

| Metric | Target | Measurement |
|--------|--------|-------------|
| Packet loss | <0.1% | Sequence gap detection |
| Jitter buffer underruns | <1/min | Counter in RX |
| Root switches | Seamless | No audio dropout >100ms |

### Scalability

| Configuration | Supported Nodes | Notes |
|---------------|-----------------|-------|
| 1 hop star | 10+ RX | Current capability |
| 1 hop mesh | 8-10 RX | Airtime limit |
| 2 hop, 2 branches | 6-8 RX | Recommended max |
| 3 hop, sparse | 4-6 RX | Latency/airtime limited |

## Known Limitations & Future Work

### Current Limitations

**With Raw PCM (1.5 Mbps):**
- ⚠️ Limited to 2-3 hops before airtime saturates
- ⚠️ ~10 RX nodes max at single hop
- ⚠️ Each branch/hop multiplies bandwidth
- ⚠️ No QoS separation on same port (WMM limited for broadcast)

### Future Enhancements

**Compression (Opus @ 128 kbps):**
- 🎯 10x bandwidth reduction → 100+ nodes possible
- 🎯 6+ hops viable
- 🎯 Multiple simultaneous TX streams
- 🎯 Requires ESP-ADF integration (your Jan 2025 scaffold)

**Advanced Features:**
- **Stream mixing** - Multiple TX → single combined output
- **Selective reception** - RX chooses which stream(s) to play
- **Spatial audio** - Positional metadata in frames
- **Adaptive bitrate** - Compress more at deeper layers
- **Clock synchronization** - Network-wide time alignment
- **Packet FEC** - Forward error correction for reliability

## sdkconfig Requirements

### Mesh-Specific Settings

```ini
# Enable ESP-WIFI-MESH
CONFIG_ESP_WIFI_MESH_ENABLE=y

# Mesh networking
CONFIG_LWIP_MAX_SOCKETS=16          # Enough for mesh + app
CONFIG_ESP_WIFI_MESH_MAX_LAYER=6    # Max tree depth
CONFIG_ESP_WIFI_MESH_AP_CONNECTIONS=10  # Children per node

# WiFi optimization
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16  # Increase for mesh
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=64 # Mesh uses more buffers
CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1         # Dynamic TX buffers
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=64

# FreeRTOS (already configured)
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# Disable power save
CONFIG_PM_ENABLE=n
```

### Shared Settings (Keep Current)

```ini
# ESP32-S3 target
CONFIG_IDF_TARGET="esp32s3"

# Audio (already optimized)
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
```

## Testing Checklist

### Unit Tests (Per Phase)

**Phase 1: Mesh Formation**
- [ ] Single node becomes root
- [ ] Second node joins as child
- [ ] Root switch on power loss
- [ ] Heartbeat messages received
- [ ] Topology metrics accurate

**Phase 2: Single-Hop Audio**
- [ ] TX sends 5ms frames
- [ ] RX receives all frames
- [ ] Sequence numbers incrementing
- [ ] Latency <50ms
- [ ] Audio quality acceptable

**Phase 3: Multi-Hop Relay**
- [ ] Frames forwarded through relay nodes
- [ ] No duplicate playback
- [ ] No infinite loops (TTL works)
- [ ] Latency scales linearly with hops
- [ ] All leaf nodes receive audio

**Phase 4: Root Migration**
- [ ] Root leaves → new root elected
- [ ] Audio continues during migration
- [ ] Dropout <500ms during switch
- [ ] Topology rebuilds correctly
- [ ] Metrics update on all nodes

### Integration Tests

**Topology Scenarios:**
- [ ] 1 TX + 1 RX (baseline)
- [ ] 1 TX + 5 RX (single hop)
- [ ] 1 TX + 3 RX (2 hops via relay)
- [ ] 2 TX + 4 RX (multi-publisher)
- [ ] Mobile TX (walk around with publisher)

**Failure Recovery:**
- [ ] Root power loss
- [ ] TX power loss during stream
- [ ] Intermediate relay node loss
- [ ] Rapid connect/disconnect
- [ ] All nodes power cycle simultaneously

**Performance:**
- [ ] Continuous playback 10+ minutes
- [ ] Packet loss <0.1%
- [ ] Underruns <1/minute
- [ ] Latency stable <100ms
- [ ] CPU usage <50%

## Success Criteria

### v0.1 Mesh MVP (Definition of Done)

**Functional Requirements:**
- ✅ Mesh network forms automatically with 2+ nodes
- ✅ First node (usually RX) becomes root
- ✅ TX node publishes audio from any position in tree
- ✅ All RX nodes receive and play audio
- ✅ Root migration works without >500ms dropout
- ✅ Multi-hop relay functional (tested 2+ hops)
- ✅ Display shows mesh status (root, layer, children)

**Performance Requirements:**
- ✅ Latency <100ms for ≤2 hops
- ✅ Packet loss <0.5%
- ✅ Supports 5+ RX nodes at single hop
- ✅ Supports 3+ RX nodes at two hops
- ✅ Audio quality indistinguishable from star topology

**Reliability Requirements:**
- ✅ Network survives root node loss
- ✅ Network survives TX node loss (audio stops gracefully)
- ✅ Nodes rejoin automatically after power cycle
- ✅ No crashes or watchdog resets during normal operation

## Future Vision: Advanced Mesh Features

### v0.2: Compression & Scaling
- **Opus codec** (64-128 kbps) via ESP-ADF
- Supports 50+ nodes, 4-6 hops
- Multiple concurrent TX streams
- Adaptive bitrate based on hop count

### v0.3: Advanced Routing
- **Stream-aware routing** - Optimize path per stream
- **Load balancing** - Distribute children across branches
- **QoS enforcement** - Bandwidth reservation per stream
- **Multicast groups** - RX nodes subscribe to specific streams

### v0.4: Synchronization
- **Network time protocol** - Sync clocks across mesh
- **Synchronized playback** - Frame-accurate timing
- **Multi-speaker arrays** - Coherent spatial audio
- **TDMA scheduling** - Collision-free transmission

## References

### ESP-IDF Documentation
- [ESP-WIFI-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/esp-wifi-mesh.html)
- [ESP-MESH API Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp-mesh.html)
- [WiFi Configuration](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi.html)

### Examples
- ESP-IDF: `examples/mesh/internal_communication`
- ESP-IDF: `examples/mesh/manual_networking`

### Academic References
- IEEE 802.11s (Mesh Networking Standard)
- ESP-WIFI-MESH whitepaper (Espressif)
- Real-time audio over wireless mesh (various papers)

## Appendix A: Comparison Matrix

| Feature | Current Star | ESP-WIFI-MESH | ESP-NOW | Custom Mesh |
|---------|--------------|---------------|---------|-------------|
| Self-healing | ❌ No | ✅ Auto | ⚠️ Manual | ⚠️ Manual |
| Root migration | ❌ No | ✅ Auto | ⚠️ Manual | ⚠️ Manual |
| Multi-hop | ❌ No | ✅ Built-in | ⚠️ Manual | ⚠️ Manual |
| Bandwidth | ✅ High | ✅ Good | ❌ Limited | ✅ Good |
| Latency | ✅ ~20ms | ✅ ~30-60ms | ✅ ~10-20ms | ✅ ~30-60ms |
| Implementation | ✅ Done | ⚠️ 1-2 days | ❌ 1-2 weeks | ❌ 2-4 weeks |
| Reliability | ⚠️ Low | ✅ High | ⚠️ Medium | ⚠️ Medium |
| Scalability | ⚠️ 10 nodes | ✅ 50+ nodes | ⚠️ 20 nodes | ✅ 50+ nodes |

**Recommendation: ESP-WIFI-MESH** - Best balance of features, effort, and reliability for audio streaming use case.

## Appendix B: Mesh Packet Format Evolution

### Current (Star Topology)
```
[12-byte Header][1920-byte PCM Audio (10ms)]
Total: 1932 bytes @ 100 fps = 1.546 Mbps
```

### Mesh v1 (Raw PCM, 5ms)
```
[12-byte Header][960-byte PCM Audio (5ms)]
Total: 972 bytes @ 200 fps = 1.555 Mbps
```

### Mesh v2 (Opus Compressed - Future)
```
[12-byte Header][~80-byte Opus Frame (5ms @ 128kbps)]
Total: 92 bytes @ 200 fps = 147 kbps (10x reduction)
```

## Appendix C: Root Election Algorithm

**ESP-MESH Default (Automatic):**
1. Scan for mesh beacons (5 seconds)
2. No beacons → **Become root**
3. Beacons found → Join best parent (highest RSSI)
4. Root failure detected → Children vote for new root
5. Election winner broadcasts root announcement
6. Network reorganizes under new root

**Our Enhancement (RX-First Preference):**
```c
void apply_root_preference() {
    // On boot, check if we're alone
    if (local_role == ROLE_RX) {
        mesh_addr_t parent;
        esp_err_t err = esp_mesh_get_parent_bssid(&parent);
        
        if (err != ESP_OK) {
            // No parent found after initial scan
            ESP_LOGI(TAG, "No mesh found - becoming root (RX preference)");
            esp_mesh_fix_root(true);  // Lock as root temporarily
            
            // After 30 seconds, release lock to allow better root if one joins
            vTaskDelay(pdMS_TO_TICKS(30000));
            esp_mesh_fix_root(false);  // Allow migration
            ESP_LOGI(TAG, "Released root lock - mesh can now migrate");
        }
    }
}
```

---

*Document created for mesh network migration planning. Ready for implementation.*
