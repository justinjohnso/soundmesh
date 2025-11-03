# From DHCP Crisis to Mesh Vision: Debugging Network Contention and Architecting True Self-Healing Mesh Audio

*November 3, 2025*

Today was a deep dive into network debugging that uncovered fundamental architectural issues and sparked a complete rethinking of the MeshNet Audio network layer. What started as "RX can't get an IP address" evolved into understanding WiFi airtime contention, implementing event-driven stream gating, discovering that our "mesh" isn't actually a mesh, and planning a migration to true ESP-WIFI-MESH architecture.

## The Crisis: TX Drops RX Before DHCP Completes

### Symptoms

The system exhibited a consistent failure pattern:
- RX successfully scans and finds TX AP (RSSI: -45 dBm, excellent signal)
- RX authenticates and associates with TX
- RX reaches WiFi "run" state after ~500ms
- **TX drops RX with disconnect code `fc0` at ~4 seconds**
- RX DHCP fails: "Failed to obtain IP address from DHCP"
- Endless retry loop

Logs showed the timeline:
```
I (3410) wifi:state: init -> auth (b0)           # RX authenticating
I (3810) wifi:state: auth -> assoc (0)           # RX associating  
I (4310) wifi:state: assoc -> run (10)           # RX connected!
I (8240) wifi:state: run -> init (fc0)           # TX drops RX (4s later)
E (13850) network: Failed to obtain IP address   # DHCP timeout
```

### Initial Hypothesis: Display Blocking WiFi Stack

The first theory was that display rendering on TX (I2C transactions every 100ms) was blocking the WiFi stack and preventing DHCP responses. This was plausible because:
- I2C is synchronous and could hold the main task
- Main task might be starving WiFi/LwIP background tasks
- OLED updates coincided with timing of failures

**Test:** Disabled `display_render_tx()` completely.

**Result:** ❌ Issue persisted. Display was not the culprit.

### The Oracle's Diagnosis: Airtime Starvation

Consulting the Oracle (GPT-5 reasoning model) with full context revealed the **actual root cause**:

> *"Your TX SoftAP is flooding the air with low-rate broadcast audio immediately after a STA associates. That starves the DHCP exchange (which is also broadcast), so RX never gets a lease and TX drops the STA a few seconds later."*

**The Real Problem:**

1. **TX starts broadcasting audio immediately** when RX associates (before DHCP)
2. **Audio packets sent at 100 fps** (10ms frames, 1920 bytes each)
3. **Broadcast at lowest basic rate** (1-2 Mbps for 802.11b compatibility)
4. **DHCP also uses broadcast** at the same low rate
5. **Airtime contention:** Audio packets consume so much airtime that DHCP packets are delayed/lost
6. **TX times out waiting for STA activity** and deauthenticates (code fc0)

This was **not a CPU starvation issue** - it was **WiFi airtime contention** at the physical layer.

### Solution Part 1: Event-Driven Stream Gating

Instead of blindly broadcasting audio, implement precise control based on WiFi/IP events:

**Event Flow:**
```
WIFI_EVENT_AP_STACONNECTED 
  → stream_ready = false (PAUSE audio)
  
IP_EVENT_AP_STAIPASSIGNED
  → stream_ready = true (RESUME audio - DHCP complete!)
  
WIFI_EVENT_AP_STADISCONNECTED
  → stream_ready = false (STOP audio)
```

**Implementation:**

```c
// Event handler in lib/network/src/mesh_net.c
static bool stream_ready = false;

static void wifi_ap_event_handler(...) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "STA connected: MAC=%02x:...");
        stream_ready = false;  // Pause streaming until IP assigned
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected: reason=%d", event->reason);
        stream_ready = false;
    }
}

static void ip_ap_event_handler(...) {
    if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ESP_LOGI(TAG, "STA assigned IP: " IPSTR, IP2STR(&event->ip));
        stream_ready = true;  // Resume streaming - DHCP complete!
    }
}
```

**TX main loop change:**
```c
// Old: Always send if audio active
if (status.audio_active) {
    network_udp_send_audio(framed_buffer, size);
}

// New: Gate on DHCP completion
if (status.audio_active && network_is_stream_ready()) {
    network_udp_send_audio(framed_buffer, size);
}
```

**Result:** ✅ **DHCP now completes successfully!**

```
I (11975) network: STA connected: MAC=10:20:ba:03:b8:a0 AID=1
I (12005) esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
I (12015) network: STA assigned IP: 192.168.4.2
I (13575) tx_main: Sent packet seq=127 (1932 bytes)  # Audio resumes after DHCP
```

### Solution Part 2: WiFi Multimedia (WMM) QoS Separation

The Oracle recommended separating control traffic (DHCP, pings) from audio data using 802.11e WMM quality-of-service classes.

**Concept: Separate Sockets with DSCP Tagging**

WiFi has 4 access categories (priority queues):
- **AC_VO (Voice)** - Highest priority (DSCP EF = 0xB8)
- **AC_VI (Video)** - High priority
- **AC_BE (Best Effort)** - Normal priority
- **AC_BK (Background)** - Lowest priority (DSCP CS1 = 0x20)

By tagging sockets with DSCP values, we can map traffic to WMM queues:

```c
// Control socket (DHCP, pings)
int tos_vo = 0xB8;  // DSCP EF → AC_VO
setsockopt(udp_sock_ctrl, IPPROTO_IP, IP_TOS, &tos_vo, sizeof(tos_vo));

// Audio socket (bulk data)
int tos_bk = 0x20;  // DSCP CS1 → AC_BK  
setsockopt(udp_sock_audio, IPPROTO_IP, IP_TOS, &tos_bk, sizeof(tos_bk));
```

**Implementation:**
- Created two UDP sockets on TX: `udp_sock_ctrl` (AC_VO) and `udp_sock_audio` (AC_BK)
- Tagged with appropriate DSCP values for WMM priority
- Control traffic (DHCP, pings) uses high-priority queue
- Audio uses low-priority queue, won't starve control

### Solution Part 3: The Socket Port Problem

After implementing dual sockets, **RX received 0 packets** despite TX sending successfully and DHCP working.

**The Issue:**
- TX sends audio on `udp_sock_audio` (port 3333)
- RX listens on `udp_sock_ctrl` (port 3333)
- Both same port, but **different sockets**
- Broadcast UDP doesn't duplicate to multiple sockets on same port

**The Fix (Temporary):**
Broadcast UDP to a given port is delivered based on `(dst_addr, dst_port)`, not DSCP/TOS. Multiple sockets on the same port don't both receive the same broadcast packet. We need either:
- **Option A:** Same socket for send/receive (current quick fix)
- **Option B:** Different destination ports (3333 ctrl, 3334 audio)

Implemented Option A for now - route audio through control socket:

```c
esp_err_t network_udp_send_audio(const uint8_t *data, size_t len) {
    // Quick fix: send via control socket so RX receives it
    int sock = udp_sock_ctrl;  // Both on same socket now
    sendto(sock, data, len, 0, &broadcast_addr, sizeof(broadcast_addr));
}
```

**Note:** True QoS separation requires separate destination ports, which we'll implement in the mesh architecture.

## The Revelation: This Isn't Actually a Mesh

While reviewing the implementation, a critical realization emerged: **despite being called "MeshNet Audio," the current network is a simple star topology, not a mesh.**

### What We Have (Star Topology)
```
        TX (AP - Single Point of Failure)
        └─ UDP Broadcast → 192.168.4.255
              ↓
    ┌─────────┼─────────┐
   RX1       RX2       RX3
  (STA)     (STA)     (STA)
  
Limitations:
- TX dies → network collapses
- No multi-hop routing
- No self-healing
- Fixed root (TX is always AP)
```

### What We Need (True Mesh)
```
        Root (Auto-elected, migrates on failure)
        ├─ TX (publishes audio, can be anywhere)
        ├─ RX1 (receives + relays to children)
        │  └─ RX3 (leaf, 2 hops from TX)
        └─ RX2 (receives only)
        
Features:
- Root leaves → auto re-election
- Multi-hop extends range
- Self-healing topology
- TX independent of root role
```

### Original Vision vs Current Reality

Reading through the project docs revealed the **original vision from January 2025** was indeed ESP-WIFI-MESH:

> *"Key requirements emerged early:*
> *- **ESP-WIFI-MESH** for reliable multi-hop networking*
> *- **Opus codec** for low-latency, high-quality audio compression*
> *- **2.5-10ms frame sizes** for real-time performance"*

The current UDP broadcast approach was a **pragmatic simplification for the MVP**, but now it's time to fulfill the original vision.

## Mesh Architecture Planning

### Protocol Selection: ESP-WIFI-MESH

After evaluating options (ESP-WIFI-MESH, ESP-NOW, hybrid, custom), **ESP-WIFI-MESH is the clear choice** for our use case:

**Advantages:**
- ✅ Automatic root election and migration (no single point of failure)
- ✅ Self-healing topology (nodes join/leave dynamically)
- ✅ Multi-hop routing built-in (extends range)
- ✅ Handles 1.5 Mbps raw audio for 2-3 hops
- ✅ Clean API, minimal manual routing logic
- ✅ Aligns with original vision

**Why Not ESP-NOW:**
- Payload limit ~250 bytes → would need ~8 packets per 5ms frame
- Manual multi-hop routing (complex)
- No automatic topology management
- Better suited for control plane, not bulk audio

### Key Architectural Principles

**1. Publisher Independence from Root:**
The TX node (audio publisher) can be anywhere in the mesh tree. It doesn't need to be the root. This separates concerns:
- **Mesh root** - Network coordination, elected automatically
- **Audio publisher** - Content source, application-level role

**2. Tree Broadcast with Deduplication:**
Instead of flooding (every node rebroadcasts to everyone), use intelligent tree forwarding:
- Node receives audio frame
- Check duplicate cache (stream_id, seq) → if seen, drop
- Mark as seen in LRU cache
- Enqueue for local playback (if RX role)
- Forward to all children except the sender

This gives **one transmission per branch per hop** - highly efficient.

**3. Frame Size Optimization:**
Switch from 10ms frames (1920 bytes) to **5ms frames (960 bytes)**:
- Reduces latency per hop (5ms vs 10ms)
- Smaller packets easier for mesh to forward
- Still well under MTU limits
- Better for real-time feel

## Implementation Roadmap

### Phase 1: Mesh Backend Scaffold (1-3 hours)

**Create parallel mesh backend:**
```
lib/network/src/
├── mesh_net.c → network_star.c  (rename, keep as fallback)
└── network_mesh.c               (NEW - ESP-WIFI-MESH)
```

**Tasks:**
- Initialize ESP-MESH with self-organized mode
- Register event handlers (parent connect, child connect, root switch)
- Implement heartbeat (role advertisement: TX vs RX)
- Expose topology queries: `network_is_root()`, `network_get_layer()`, `network_get_children_count()`

**Backend selection via compile flag:**
```cmake
set(NETWORK_BACKEND "MESH")  # or "STAR" for current behavior
```

### Phase 2: 5ms Audio Framing & Single-Hop (2-4 hours)

**Update frame size:**
```c
// config/build.h
#define AUDIO_FRAME_MS         5    // was 10
#define AUDIO_FRAME_SAMPLES    240  // was 480
#define AUDIO_FRAME_BYTES      960  // was 1920
```

**Implement mesh send/receive:**
```c
// TX: Send audio via ESP-MESH
esp_err_t network_send_audio(const uint8_t *frame, size_t len) {
    mesh_data_t data = {
        .data = frame,
        .size = len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P  // Low priority for audio
    };
    return esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
}

// RX: Callback when mesh data arrives
void on_mesh_data_received(mesh_addr_t *from, mesh_data_t *data) {
    if (audio_callback) {
        audio_callback(data->data, data->size);
    }
}
```

**Test:** Single-hop TX→RX audio streaming via mesh

### Phase 3: Tree Broadcast & Multi-Hop (2-4 hours)

**Implement intelligent forwarding:**

```c
// Duplicate suppression (LRU cache)
#define DEDUPE_CACHE_SIZE 32
typedef struct {
    uint8_t stream_id;
    uint16_t seq;
} recent_frame_t;
static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];

void forward_to_children(mesh_audio_frame_t *frame, mesh_addr_t *sender) {
    mesh_addr_t children[10];
    int num_children;
    esp_mesh_get_routing_table(children, 10 * 6, &num_children);
    
    for (int i = 0; i < num_children; i++) {
        if (memcmp(&children[i], sender, 6) != 0) {  // Don't echo
            frame->ttl--;
            if (frame->ttl > 0) {
                esp_mesh_send(&children[i], frame, len, 
                             MESH_DATA_P2P, NULL, 0);
            }
        }
    }
}
```

**Test:** 3-node chain (root → relay → leaf), verify audio reaches end with no duplicates

### Phase 4: Root Preference & Polish (1-2 hours)

**RX-first root election:**
```c
if (local_role == ROLE_RX && no_mesh_beacons_detected()) {
    esp_mesh_fix_root(true);   // Lock as root initially
    ESP_LOGI(TAG, "First node - becoming root (RX preference)");
    
    // After 30s, allow migration if better root joins
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_mesh_fix_root(false);
}
```

**Surface mesh status to display:**
- Root indicator (crown icon or "ROOT" text)
- Layer number (hops from root)
- Children count
- Total mesh nodes

**Test:** Multiple boot scenarios, verify root preference and migration

## Network Architecture Deep Dive

### Current Star Architecture (What We Built)

**Topology:**
- **1 TX node** = WiFi Access Point (192.168.4.1)
- **N RX nodes** = WiFi Stations (192.168.4.2, .3, .4, ...)
- **Transport:** UDP broadcast to 192.168.4.255:3333
- **Range:** Single hop only (WiFi radio range ~30-50m)

**Strengths:**
- ✅ Simple to implement and debug
- ✅ Low latency (~20ms typical)
- ✅ High bandwidth (supports 10+ RX nodes)
- ✅ Predictable behavior
- ✅ Works with existing WiFi tools (can see AP in WiFi scanner)

**Fatal Weaknesses:**
- ❌ **Single point of failure** - TX dies → network collapses
- ❌ **No self-healing** - manual intervention required
- ❌ **No multi-hop** - limited by radio range
- ❌ **Fixed root** - TX must always be AP
- ❌ **No mesh routing** - RX nodes can't relay

**Audio Flow:**
```
TX: Generate 1920-byte frame every 10ms
    → Build UDP packet with header (12 bytes)
    → sendto(192.168.4.255:3333) on udp_sock_ctrl
    → WiFi driver queues packet
    → Radio broadcasts on Channel 6
    
RX: Radio receives packet on Channel 6
    → WiFi driver delivers to socket
    → recvfrom(0.0.0.0:3333) with 10ms timeout
    → Validate header (magic, version, type)
    → Check sequence number for gaps
    → Write payload to jitter buffer (100ms ring buffer)
    → I2S DMA pulls audio continuously → DAC → Speaker
```

**Event-Driven Gating (Current Implementation):**
```
STA joins → WIFI_EVENT_AP_STACONNECTED
  → stream_ready = false
  → TX stops sending audio
  → DHCP exchange completes uninterrupted (~1-2 seconds)
  → IP_EVENT_AP_STAIPASSIGNED
  → stream_ready = true
  → TX resumes audio broadcast
```

**Current Bandwidth:**
- 48kHz × 16-bit × 2ch = 1.536 Mbps (raw)
- Header overhead: ~19 kbps
- Total: **~1.55 Mbps**
- At broadcast rate 6-54 Mbps: **~3-25% airtime**

### Target Mesh Architecture (What We're Building)

**Topology: Self-Organizing Tree**

```
Dynamic topology that adapts to node presence:

Scenario 1 (RX boots first):
    RX1 (Root)
    
Scenario 2 (TX joins):
    RX1 (Root)
    └─ TX1 (Child, publishes audio)
    
Scenario 3 (More RX join):
    RX1 (Root)
    ├─ TX1 (Child, publishes)
    ├─ RX2 (Child, receives + can relay)
    └─ RX3 (Child)
    
Scenario 4 (RX1 leaves):
    TX1 (New Root, still publishes!)
    ├─ RX2 (Child)
    └─ RX3 (Child)
    
Scenario 5 (Multi-hop for range):
    RX1 (Root)
    └─ TX1 (1 hop)
        └─ RX2 (2 hops, relay node)
            └─ RX3 (3 hops, leaf - extends range!)
```

**Key Properties:**
- **No single point of failure** - root migrates when nodes leave
- **Self-healing** - topology reconfigures automatically
- **Multi-hop** - extends effective range through relay nodes
- **Publisher independence** - TX can be anywhere in tree
- **N:M communication** - future: multiple TX → multiple RX

**ESP-WIFI-MESH Responsibilities:**
- Mesh beacon scanning and formation
- Parent selection (chooses best RSSI/route)
- Root election algorithm
- Routing table maintenance
- Packet forwarding at mesh layer
- Network self-healing

**Application Layer Responsibilities (What We Implement):**
- Role advertisement (TX vs RX)
- Audio stream announcement (sample rate, channels, codec)
- Tree broadcast logic (forward to children with deduplication)
- Jitter buffer management (already done!)
- Metrics collection (latency, packet loss, hop count)

### Audio Protocol: Tree Broadcast

**Tree Broadcast Algorithm:**

```
Publisher (TX node at any position):
  1. Generate 5ms audio frame (960 bytes PCM)
  2. Build mesh_audio_frame_t with header
  3. esp_mesh_send(NULL, ..., MESH_DATA_TODS)  
     → NULL = broadcast to descendants via tree
  4. Mesh stack routes to all children

Every Node on Receive Path:
  1. Validate header (magic, version, stream_id)
  2. Check duplicate cache:
     - If (stream_id, seq) seen recently → DROP (already forwarded)
     - Else → continue
  3. Mark frame as seen in LRU cache
  4. If local role == RX:
     - Write payload to jitter buffer for playback
  5. Get list of children from mesh routing table
  6. For each child except sender:
     - Decrement TTL
     - If TTL > 0: esp_mesh_send(child, frame, MESH_DATA_P2P)
```

**Efficiency Analysis:**
- **Star broadcast:** 1 TX → N receivers = **1 transmission total**
- **Blind flooding mesh:** 1 TX → every node rebroadcasts = **N transmissions** (terrible)
- **Tree broadcast:** 1 TX → relay to children only = **H transmissions** where H = hops × avg_children
  - Example: 2 hops, 2 children per node = ~4 transmissions (excellent)

**Deduplication is Critical:**
Without the LRU cache of (stream_id, seq), nodes would forward the same frame multiple times as they receive it from different parents/siblings, creating packet storms and infinite loops.

### Frame Format Evolution

**Current (Star, 10ms):**
```c
typedef struct __attribute__((packed)) {
    uint8_t magic;       // 0xA5
    uint8_t version;     // 1
    uint8_t type;        // 1=AUDIO_RAW
    uint8_t reserved;
    uint16_t seq;        // Network byte order
    uint32_t timestamp;  // Milliseconds
    uint16_t payload_len;
} net_frame_header_t;  // 12 bytes

Payload: 1920 bytes (10ms @ 48kHz stereo)
Total: 1932 bytes @ 100 fps
```

**Mesh (5ms, with TTL):**
```c
typedef struct __attribute__((packed)) {
    uint8_t magic;       // 0xA5
    uint8_t version;     // 1
    uint8_t type;        // 1=AUDIO_RAW
    uint8_t stream_id;   // Multi-TX support
    uint16_t seq;        // Network byte order
    uint32_t timestamp;  // Milliseconds
    uint16_t payload_len;
    uint8_t ttl;         // Hop limit (e.g., 6)
    uint8_t reserved;    // Alignment
} mesh_audio_frame_t;  // 12 bytes

Payload: 960 bytes (5ms @ 48kHz stereo)
Total: 972 bytes @ 200 fps
```

**Changes:**
- Added `stream_id` for multi-publisher support
- Added `ttl` (Time To Live) for loop prevention
- Halved frame size (5ms vs 10ms)
- Doubled packet rate (200 fps vs 100 fps)
- Bandwidth unchanged (~1.55 Mbps)

## Bandwidth & Latency Analysis

### Current Star Topology

**Single Hop:**
- Latency: **20-30ms** (WiFi + jitter buffer)
- Airtime: **3-25%** (depends on broadcast rate)
- Nodes: **10+ RX** (limited by airtime only)

### Mesh Topology (Projected)

**One Hop (TX and RX both children of root):**
- Latency: **30-40ms** (mesh overhead + jitter buffer)
- Airtime: **~6%** (TX→root, root→RX = 2 transmissions)
- Nodes: **8-10 RX**

**Two Hops (TX → relay → RX):**
- Latency: **50-70ms** (2× mesh hop + jitter buffer)
- Airtime: **~9%** (3 transmissions in chain)
- Nodes: **6-8 RX** (assuming balanced tree)

**Three Hops:**
- Latency: **70-100ms** (approaching limit)
- Airtime: **~12-15%** (4 transmissions)
- Nodes: **4-6 RX** (limited by latency budget)

**With Opus Compression (Future):**
- 128 kbps stereo = **~10x reduction**
- Same topology: **60-80 nodes** possible
- Latency budget: **+10-20ms** for encode/decode
- Still well within <150ms target

### Scalability Comparison

| Configuration | Star | Mesh (Raw) | Mesh (Opus) |
|---------------|------|------------|-------------|
| 1 hop, 10 RX | ✅ Works | ✅ Works | ✅ Works |
| 2 hops, 6 RX | ❌ N/A | ✅ Works | ✅ Works |
| 3 hops, 4 RX | ❌ N/A | ⚠️ Borderline | ✅ Works |
| 6 hops, 50 RX | ❌ N/A | ❌ No | ✅ Works |

## Root Election Strategy

### ESP-WIFI-MESH Automatic Election

**Default Behavior (Self-Organized Mode):**
1. Node boots, scans for mesh beacons (5 seconds)
2. **No beacons found** → Become root
3. **Beacons found** → Join as child (select best parent by RSSI)
4. **Root leaves** → Children detect loss, vote for new root
5. **Election winner** → Broadcasts root announcement
6. **Network reorganizes** → All nodes find new paths

**Election Criteria:**
- RSSI to potential parent
- Routing cost to root
- Number of connections (load balancing)
- Connection stability

### Our Enhancement: RX-First Preference

**Problem:** We want RX nodes to preferentially become root (they're more stable, less likely to move).

**Solution (Minimal Intervention):**
```c
void apply_root_preference() {
    if (local_role == ROLE_RX) {
        // Check if we're the first node
        mesh_addr_t parent;
        if (esp_mesh_get_parent_bssid(&parent) != ESP_OK) {
            // No parent = no existing mesh
            ESP_LOGI(TAG, "First node - locking as root (RX preference)");
            esp_mesh_fix_root(true);
            
            // After 30 seconds, release lock
            // Allows TX nodes with better connectivity to take root if needed
            vTaskDelay(pdMS_TO_TICKS(30000));
            esp_mesh_fix_root(false);
            ESP_LOGI(TAG, "Released root lock - mesh can migrate freely");
        }
    }
}
```

**Philosophy:** Suggest RX as root initially, but don't fight the mesh. ESP-WIFI-MESH's algorithm is robust - trust it.

### Root Migration Example

**Timeline:**
```
t=0s:   RX1 boots
        → No mesh found → becomes root
        
t=5s:   TX boots  
        → Finds RX1's mesh beacon → joins as child
        Topology: RX1(root) → TX(child)
        
t=10s:  RX2 boots
        → Joins as child of RX1 (best RSSI)
        Topology: RX1(root) → TX, RX2
        
t=20s:  RX1 power loss!
        → TX and RX2 detect parent loss
        → ESP-MESH runs election (TX vs RX2)
        → Winner becomes new root (likely TX due to first-joined)
        Topology: TX(root) → RX2(child)
        
t=21s:  Audio continues!
        → TX still publishes (publisher ≠ root)
        → RX2 still receives (via new topology)
        → Dropout: <500ms during election
```

**Key Insight:** Because publisher and root are separate roles, **TX can be the mesh root and still publish audio normally**. The tree broadcast algorithm works regardless of where TX sits in the topology.

## Technical Lessons Learned

### 1. Airtime is a Shared Resource

WiFi is a shared medium. When you broadcast at 100 fps with 1920-byte packets at 1 Mbps (802.11b rate), you consume:
- 1920 bytes × 8 bits × 100 fps = **1.536 Mbps**
- At 1 Mbps rate: **100% airtime utilization** (saturated!)

DHCP packets trying to share that medium get delayed, retransmitted, and eventually time out. This is fundamentally an **airtime starvation problem**, not a CPU or stack issue.

### 2. Event-Driven is Better Than Polling

**Polling approach (original):**
- Every loop iteration: check `network_get_connected_nodes()`
- If count changes 0→1, start 3-second timer
- Wait 3 seconds, then resume audio
- **Problems:** Fixed delay (either too short or wastes time), race conditions

**Event-driven approach (implemented):**
- WiFi stack fires events at exact moments
- `IP_EVENT_AP_STAIPASSIGNED` means DHCP **definitely** completed
- No guessing, no arbitrary delays, no races
- More responsive and robust

### 3. Broadcast UDP ≠ Socket Multiplexing

A common misconception: "If I have two sockets bound to port 3333, they'll both receive broadcast packets."

**Reality:**
- Broadcast packet to `192.168.4.255:3333` is delivered based on `(dst_addr, dst_port)` only
- DSCP/TOS affects **transmission scheduling**, not **reception demuxing**
- OS picks one socket (typically first bound) to deliver the packet
- `SO_REUSEPORT` can load-balance, but doesn't duplicate to all sockets

**Implications:**
- WMM QoS tagging is for **transmit prioritization**, not receive separation
- Need **different destination ports** (3333, 3334) for true socket separation
- Or use single socket and demux in application layer (what we're doing)

### 4. The Name "Mesh" Carries Expectations

Calling this project "MeshNet Audio" created an implicit contract:
- Users/reviewers expect self-healing capabilities
- "Mesh" implies multi-hop and resilience
- Star topology violates these expectations

**Resolution:** Either rename to "WirelessNet Audio" or implement actual mesh (we're choosing the latter).

### 5. Publisher Role ≠ Network Root Role

This is a **critical architectural insight** for mesh audio systems:

**Traditional thinking:** "TX must be the root because it originates data"
**Reality:** Root is a **network coordination role**, publisher is an **application role**

In ESP-WIFI-MESH:
- Root manages topology and routing
- Any node can publish application data
- Root can be a receiver that just relays others' streams
- TX can be a leaf node deep in the tree

This separation enables:
- Root migration without interrupting audio
- TX nodes can be mobile/portable
- More stable nodes (RX) can be roots
- Better fault tolerance

## Implementation Effort & Timeline

### Total Effort Estimate: 6-13 hours (1-2 days)

**Phase 1:** Mesh Backend Scaffold - **1-3 hours**
- ESP-MESH init, events, heartbeat
- Topology queries
- Compile-time backend switch

**Phase 2:** 5ms Framing & Single-Hop - **2-4 hours**
- Update frame constants
- Implement mesh send/receive
- Test TX→RX via mesh
- Validate audio quality

**Phase 3:** Tree Broadcast & Multi-Hop - **2-4 hours**
- Duplicate suppression cache
- Child enumeration and forwarding
- TTL implementation
- Multi-hop testing

**Phase 4:** Root Preference & Polish - **1-2 hours**
- RX-first root lock/release
- Display mesh status
- Metrics integration
- End-to-end testing

### Risk Mitigation

**Parallel Development:**
- Keep star backend as `network_star.c`
- Build mesh alongside in `network_mesh.c`
- Same public API for both
- Can revert to star if mesh has issues

**Incremental Testing:**
- Test each phase thoroughly before moving forward
- Maintain audio quality benchmarks
- Monitor latency and packet loss
- Validate across power cycles

**Fallback Strategy:**
- If mesh introduces unacceptable latency/complexity
- Revert to star for demos
- Continue mesh development offline
- Mesh becomes v0.2 feature

## What's Next

### Immediate (This Week)
1. ✅ Document mesh architecture (this post!)
2. ⏳ Implement Phase 1 (mesh backend scaffold)
3. ⏳ Test mesh formation with 2 nodes
4. ⏳ Validate root election and migration

### Short-Term (Next Week)
5. ⏳ Implement 5ms audio framing
6. ⏳ Single-hop mesh audio streaming
7. ⏳ Tree broadcast with deduplication
8. ⏳ Multi-hop testing (3-node chain)

### Medium-Term (2-4 Weeks)
9. ⏳ Stress testing (10+ nodes)
10. ⏳ Range testing (multi-hop)
11. ⏳ Root migration reliability
12. ⏳ UI polish (show mesh topology on display)

### Long-Term (Future Versions)
13. ⏳ Opus compression integration (ESP-ADF scaffold from Jan 2025)
14. ⏳ Multiple TX streams (mixer on RX)
15. ⏳ Selective stream subscription
16. ⏳ Time synchronization for multi-speaker
17. ⏳ Web interface for monitoring/configuration

## Reflections on the Journey

### From Crisis to Clarity

What started as a frustrating bug ("why won't DHCP work?!") became a masterclass in:
- **Systems thinking** - understanding interactions between layers (app → WiFi → PHY)
- **Root cause analysis** - going beyond symptoms to mechanisms
- **Protocol debugging** - reading WiFi events, understanding disconnect codes
- **Architecture evaluation** - recognizing when a design doesn't meet requirements

### The Value of the Oracle

Using GPT-5's reasoning capabilities via the Oracle tool proved invaluable:
- **Hypothesis generation** - quickly identified airtime contention
- **Solution validation** - confirmed event-driven gating approach
- **Protocol expertise** - explained WMM, DSCP, broadcast UDP semantics
- **Architecture guidance** - recommended ESP-WIFI-MESH with clear rationale

Having an AI reasoning partner that can:
1. Analyze complex codebases
2. Understand embedded networking protocols
3. Propose concrete solutions with implementation details
4. Anticipate failure modes and mitigation strategies

...is transformative for embedded systems development.

### Embracing Complexity

The progression from "simple UDP broadcast" to "event-driven QoS-separated mesh" reflects growing understanding:
- **Stage 1:** "Just send packets!" (naive, broke immediately)
- **Stage 2:** "Use events to coordinate!" (works, but not scalable)
- **Stage 3:** "Implement true mesh!" (aligned with vision, future-proof)

Each stage built on lessons from the previous, and the codebase is now positioned for the mesh migration with minimal disruption.

## Current Status

### What's Working (Star Topology)
- ✅ TX AP mode with DHCP server
- ✅ RX STA mode with automatic connection
- ✅ Event-driven stream gating (prevents DHCP starvation)
- ✅ UDP broadcast audio transmission
- ✅ Jitter buffer with underrun handling
- ✅ I2S playback at 48kHz stereo
- ✅ OLED display with dual views
- ✅ Button controls (short/long press)
- ✅ Multiple audio sources (Tone, USB, AUX)
- ✅ Real-time frequency control via ADC knob

### What's Documented (Mesh Vision)
- ✅ ESP-WIFI-MESH architecture design
- ✅ Tree broadcast algorithm
- ✅ Root election strategy
- ✅ Implementation phases with effort estimates
- ✅ Migration path from star to mesh
- ✅ Performance targets and scalability analysis

### What's Next (Implementation)
- ⏳ Phase 1: Mesh backend scaffold
- ⏳ Phase 2: 5ms audio framing
- ⏳ Phase 3: Tree broadcast logic
- ⏳ Phase 4: Root preference and polish
- ⏳ End-to-end mesh validation

## Conclusion: Star → Mesh Transformation

This debugging session revealed something profound: **we built the wrong network topology for the stated goals**. The star topology works beautifully for simple 1:N broadcast with low latency, but it fundamentally cannot deliver:
- Self-healing (no single point of failure)
- Multi-hop (extended range)
- Root migration (network survives any node loss)
- True mesh routing (nodes relay for each other)

The good news: **the application layer is solid**. Audio generation, jitter buffering, I2S playback, display rendering, event handling - all of this transfers directly to the mesh architecture. The network layer is a clean abstraction that we can swap out.

The path forward is clear:
1. Implement ESP-WIFI-MESH backend (1-2 days)
2. Maintain star backend as fallback (compile-time flag)
3. Test extensively (formation, migration, multi-hop)
4. Make mesh the default
5. Continue toward Opus compression for massive scalability

The original vision from January 2025 - ESP-WIFI-MESH with Opus codec for resilient multi-hop audio distribution - is within reach. Today's work debugging DHCP, understanding airtime contention, and planning the mesh migration brings us significantly closer to that goal.

**Status:** Star topology working reliably. Mesh architecture designed and documented. Ready for implementation.

---

*This post documents the November 3, 2025 debugging session and mesh planning. See [mesh-network-architecture.md](../planning/mesh-network-architecture.md) for complete technical specifications.*

## Technical Appendix: Code Snippets

### Event-Driven Stream Gating (Implemented)

**lib/network/src/mesh_net.c:**
```c
static bool stream_ready = false;

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        stream_ready = false;  // Pause audio
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        stream_ready = false;  // Stop audio
    }
}

static void ip_ap_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
        stream_ready = true;  // Resume - DHCP complete!
    }
}

bool network_is_stream_ready(void) {
    return stream_ready;
}
```

**src/tx/main.c:**
```c
// In main loop, every 10ms
if (status.audio_active && network_is_stream_ready()) {
    // Build and send audio frame
    network_udp_send_audio(framed_buffer, frame_size);
}
```

### Tree Broadcast Algorithm (To Be Implemented)

**network_mesh.c (future):**
```c
// Duplicate suppression
#define DEDUPE_CACHE_SIZE 32
typedef struct {
    uint8_t stream_id;
    uint16_t seq;
    uint32_t timestamp_ms;
} recent_frame_t;
static recent_frame_t dedupe_cache[DEDUPE_CACHE_SIZE];
static int dedupe_idx = 0;

bool is_duplicate(uint8_t stream_id, uint16_t seq) {
    for (int i = 0; i < DEDUPE_CACHE_SIZE; i++) {
        if (dedupe_cache[i].stream_id == stream_id && 
            dedupe_cache[i].seq == seq) {
            return true;
        }
    }
    return false;
}

void on_audio_frame(mesh_audio_frame_t *frame, mesh_addr_t *from) {
    // 1. Deduplicate
    uint16_t seq = ntohs(frame->seq);
    if (is_duplicate(frame->stream_id, seq)) {
        return;
    }
    
    // 2. Mark as seen
    dedupe_cache[dedupe_idx].stream_id = frame->stream_id;
    dedupe_cache[dedupe_idx].seq = seq;
    dedupe_cache[dedupe_idx].timestamp_ms = esp_timer_get_time() / 1000;
    dedupe_idx = (dedupe_idx + 1) % DEDUPE_CACHE_SIZE;
    
    // 3. Local playback (if RX)
    if (my_role == ROLE_RX) {
        ring_buffer_write(jitter_buffer, frame->payload, frame->payload_len);
    }
    
    // 4. Forward to children
    mesh_addr_t children[10];
    int num_children;
    esp_mesh_get_routing_table(children, 10 * 6, &num_children);
    
    for (int i = 0; i < num_children; i++) {
        if (memcmp(&children[i], from, 6) != 0) {  // Don't echo
            frame->ttl--;
            if (frame->ttl > 0) {
                mesh_data_t data = {
                    .data = (uint8_t *)frame,
                    .size = sizeof(*frame),
                    .proto = MESH_PROTO_BIN,
                    .tos = MESH_TOS_P2P
                };
                esp_mesh_send(&children[i], &data, MESH_DATA_P2P, NULL, 0);
            }
        }
    }
}
```

### Root Election Enhancement (To Be Implemented)

**network_mesh.c (future):**
```c
void handle_mesh_started_event() {
    if (local_role == ROLE_RX) {
        // Check if any parent exists
        mesh_addr_t parent;
        esp_err_t err = esp_mesh_get_parent_bssid(&parent);
        
        if (err != ESP_OK) {
            // We're alone - become root with preference
            ESP_LOGI(TAG, "No mesh found - becoming root (RX preference)");
            esp_mesh_fix_root(true);
            
            // Spawn task to release lock after 30s
            xTaskCreate(release_root_lock_task, "root_release", 
                       2048, NULL, 5, NULL);
        }
    }
}

static void release_root_lock_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_mesh_fix_root(false);
    ESP_LOGI(TAG, "Released root lock - mesh self-organizing");
    vTaskDelete(NULL);
}
```

## Bandwidth Calculations Reference

### Raw PCM (Current & Mesh v1)
```
Sample rate: 48,000 Hz
Bit depth: 16 bits
Channels: 2 (stereo)

Per-sample: 16 bits × 2 channels = 4 bytes
Per-second: 48,000 samples × 4 bytes = 192,000 bytes = 1,536,000 bps

Frame sizes:
- 5ms: 240 samples × 4 bytes = 960 bytes @ 200 fps
- 10ms: 480 samples × 4 bytes = 1920 bytes @ 100 fps

Both = ~1.55 Mbps (identical bandwidth, different latency)
```

### Opus Compressed (Future)
```
Bitrates: 64, 96, 128, 160 kbps (configurable)

At 128 kbps stereo:
- 5ms frame: ~80 bytes @ 200 fps = 128 kbps
- 10ms frame: ~160 bytes @ 100 fps = 128 kbps

Compression ratio: 1.536 Mbps / 128 kbps = 12:1
Bandwidth saving: ~92%
```

### Airtime Utilization
```
Assumptions:
- Broadcast rate: 6 Mbps (802.11g minimum with 802.11b disabled)
- Packet overhead: 28 bytes (UDP) + 8 bytes (IP) + 14 bytes (Ethernet)
- Total frame: 972 bytes + 50 bytes overhead = 1022 bytes

Per-packet airtime:
- At 6 Mbps: 1022 bytes × 8 bits / 6,000,000 bps = 1.36 ms

Per-second airtime (200 fps):
- 1.36 ms × 200 = 272 ms = 27.2% utilization

Multi-hop multiplication:
- 1 hop: 27.2%
- 2 hops (chain): 54.4% (two transmissions)
- 2 hops (2 branches): 40.8% (three transmissions: root→child1, root→child2)
- 3 hops (chain): 81.6% (approaching saturation!)
```

**Conclusion:** Raw PCM viable for 2 hops with light tree. Opus essential for 3+ hops or dense topologies.

---

*End of blog post. Ready to begin mesh implementation when you are!*
