# SoundMesh Mesh Network Reliability Fix

**Date:** March 19, 2026  
**Status:** Investigation Complete, Implementation Pending  
**Author:** Copilot Analysis

## Problem Summary

Three critical issues have been identified with the ESP-WIFI-MESH audio streaming system:

1. **Connection reliability**: Only 1-2 of 3 OUT nodes connect reliably; the 3rd gets stuck in "Mesh joining" state indefinitely
2. **Inconsistent audio reception**: Connected nodes don't consistently receive audio, and conditions for success aren't reproducible
3. **Multi-node degradation**: Adding a 2nd working node causes both OUTs to stutter (nodes interfere with each other rather than cooperating)

Additionally, these issues have reportedly worsened over time, suggesting possible state accumulation within sessions.

---

## Analysis Methodology

This investigation examined:

1. **Codebase audit** of `lib/network/src/mesh_net.c` (920+ lines of mesh networking logic)
2. **Audio pipeline review** of `lib/audio/src/adf_pipeline.c` (encode/decode/playback flow)
3. **Configuration review** of `sdkconfig.*.defaults` files and `lib/config/include/config/build.h`
4. **Comparison with ESP-IDF examples** from the official [esp-idf mesh internal_communication example](https://github.com/espressif/esp-idf/tree/master/examples/mesh/internal_communication)
5. **ESP-WIFI-MESH API documentation** from ESP-IDF v5.x reference

---

## Root Cause #1: Connection Reliability (3rd Node Stuck)

### Finding: Self-Organized Mode Disabled Prematurely

**Location:** `lib/network/src/mesh_net.c`, lines 302-306

```c
case MESH_EVENT_PARENT_CONNECTED: {
    // ... connection handling ...
    
    // Disable self-organized mode to stop periodic parent monitoring scans
    // These scans pause the radio for ~300ms and cause audio dropouts
    esp_mesh_set_self_organized(false, false);
    esp_wifi_scan_stop();
    ESP_LOGI(TAG, "Self-organized disabled (no more parent scans during streaming)");
    break;
}
```

**The Problem:**

When an OUT node successfully connects to the root and becomes `PARENT_CONNECTED`, this code disables `self-organized` mode. The intent was to stop periodic scanning that causes audio dropouts. However, **this code runs on all nodes including the root**.

For RX/OUT nodes, disabling self-organized is fine—they've already joined.

For the ROOT node (TX/COMBO), disabling self-organized means **the mesh AP stops accepting new child associations**. The 3rd OUT node attempts to join but the root's SoftAP is no longer processing new connections.

**Evidence:**

- ESP-IDF documentation states: "When self-organized networking is disabled, the mesh stack will not perform operations that change the network's topology" ([ESP-MESH Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/esp-wifi-mesh.html))
- The code at line 358-363 tries to handle this for CHILD_CONNECTED, but by then the damage is done

**Citation:** Lines 302-306 of `mesh_net.c`

### Finding: Missing Mesh Queue Configuration

**Location:** `lib/network/src/mesh_net.c`, `network_init_mesh()` function (lines 736-859)

The function never calls `esp_mesh_set_xon_qsize()` to configure the mesh RX queue. Default is 32 packets.

**The Problem:**

With 3 nodes receiving ~25 audio packets/second each (50fps/2 batch = 25pps), the total is 75 packets/second flowing through the root. During bursts or when one node is slow to drain, the queue fills up and new connections are rejected.

**Evidence:**

ESP-IDF header at `esp_mesh.h` line 740:
```c
// Using esp_mesh_set_xon_qsize() users may configure the RX queue size, default:32.
// If this size is too large, memory may become insufficient.
```

**Citation:** `esp_mesh.h` line 740, `mesh_net.c` lines 736-859

### Finding: AUTH_EXPIRE Recovery Loops

**Location:** `lib/network/src/mesh_net.c`, lines 329-344

```c
if (disconnected->reason == WIFI_REASON_AUTH_EXPIRE) {
    auth_expire_count++;
}
// ...
if (auth_expire_count >= 5) {
    trigger_rx_rejoin();
    auth_expire_count = 0;
}
```

**The Problem:**

The 3rd node attempting to join hits AUTH_EXPIRE repeatedly because the root's AP isn't processing new associations (due to self-organized being disabled). The recovery logic waits for 5 failures before forcing a rejoin, but the underlying cause isn't addressed.

**Citation:** Lines 329-344 of `mesh_net.c`

---

## Root Cause #2: Inconsistent Audio Reception

### Finding: Global Backoff Penalizes All Children

**Location:** `lib/network/src/mesh_net.c`, lines 978-1099

The root sends audio by iterating the routing table and calling `esp_mesh_send()` to each child:

```c
for (int i = 0; i < route_table_size; i++) {
    if (memcmp(&route_table[i], my_sta_mac, 6) == 0) continue;
    esp_err_t send_err = esp_mesh_send(&route_table[i], &mesh_data, 
                                        MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    if (send_err == ESP_ERR_MESH_QUEUE_FULL) {
        any_queue_full = true;
    }
}
```

When **any** send returns `ESP_ERR_MESH_QUEUE_FULL`, the global `backoff_level` is incremented:

```c
if (any_queue_full || err == ESP_ERR_MESH_QUEUE_FULL) {
    // ...
    if (backoff_level < RATE_LIMIT_MAX_LEVEL) {
        backoff_level++;
    }
}
```

**The Problem:**

If OUT1's queue is backed up but OUT2 and OUT3 are fine, the backoff affects ALL children equally. The rate limiting at lines 1003-1012 then drops frames for everyone:

```c
if (backoff_level > 0) {
    skip_counter++;
    if (skip_counter <= backoff_level) {
        total_drops++;
        return ESP_ERR_MESH_QUEUE_FULL;  // Drop this frame (silent)
    }
    skip_counter = 0;
}
```

**Citation:** Lines 978-1099 of `mesh_net.c`

### Finding: P2P Sends Create N× TX Load

**Location:** `lib/network/src/mesh_net.c`, lines 1036-1053

The code uses `MESH_DATA_P2P` flag for all sends. This requires separate WiFi transmissions per destination.

**The Problem:**

ESP-WIFI-MESH supports `MESH_DATA_GROUP` for multicast addressing (see `esp_mesh.h` line 128):
```c
#define MESH_DATA_GROUP  (0x40)  // identify this packet is target to a group address
```

With P2P, sending to 3 children means 3 separate WiFi frames. With GROUP, one frame reaches all children in the broadcast domain. This reduces airtime by 3×.

**Note:** GROUP requires registering group addresses via `esp_mesh_set_group_id()`, which isn't currently implemented.

**Citation:** `esp_mesh.h` line 128, `mesh_net.c` lines 1036-1053

---

## Root Cause #3: Multi-Node Degradation (Stutter When Adding 2nd Node)

### Finding: WiFi Channel Contention

**Configuration:** `lib/config/include/config/build.h` line 101

```c
#define MESH_CHANNEL  6
```

**The Problem:**

All nodes share channel 6. WiFi uses CSMA/CA (Carrier Sense Multiple Access with Collision Avoidance). When the root sends to OUT1, all nodes hear it. Then OUT1 sends ACK. Then root sends to OUT2. Then OUT2 sends ACK.

With 1 node: 2 transmissions per packet  
With 2 nodes: 4 transmissions per packet  
With 3 nodes: 6 transmissions per packet

The airtime increases linearly, and so does the probability of collisions and retries.

**Citation:** `build.h` line 101

### Finding: Root Scanning During Audio

**Location:** `lib/network/src/mesh_net.c`, lines 358-363

The root only disables self-organized and stops scanning **after the first child connects**:

```c
case MESH_EVENT_CHILD_CONNECTED: {
    // ...
    if (is_mesh_root) {
        esp_mesh_set_self_organized(false, false);
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
    }
}
```

**The Problem:**

1. Before any child connects, the root may still be scanning periodically (up to 300ms pauses)
2. Even after this code runs, there's a race condition—if the root was mid-scan when the event fires, the scan may complete anyway

**Evidence from ESP-IDF docs:**
> "During scanning, the radio cannot transmit or receive data frames. This causes a gap in connectivity."

**Citation:** Lines 358-363 of `mesh_net.c`

### Finding: Fixed Jitter Buffer Size

**Location:** `lib/config/include/config/build.h`, lines 131-132

```c
#define JITTER_BUFFER_FRAMES  6    // 6 × 20ms = 120ms max depth
#define JITTER_PREFILL_FRAMES 3    // 3 × 20ms = 60ms startup latency
```

**The Problem:**

The jitter buffer prefill is fixed at 60ms. This works for single-hop, low-contention scenarios. With multiple nodes or multi-hop topologies, network latency variance increases, but the jitter buffer doesn't adapt.

When the 2nd node joins and contention increases:
- Packet arrival times become more variable
- 60ms prefill may not be enough to absorb jitter
- Underruns occur, causing audio gaps

**Citation:** `build.h` lines 131-132

---

## Root Cause #4: Session Degradation Over Time

### Finding: Global State Accumulation

**Location:** `lib/network/src/mesh_net.c`, lines 979-984

```c
static uint32_t backoff_level = 0;
static uint32_t success_streak = 0;
static uint32_t total_drops = 0;
static uint32_t total_sent = 0;
static uint32_t skip_counter = 0;
static int64_t last_qfull_us = 0;
```

These static variables accumulate throughout a session. `total_drops` and `total_sent` grow unbounded. While they don't directly cause issues, they're used in the periodic logging at line 1026-1033:

```c
ESP_LOGI(TAG, "Mesh TX: route=%d, sent=%lu, drops=%lu (%.1f%%), backoff=%lu", 
         route_table_size, total_sent, total_drops,
         total_sent > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f,
         backoff_level);
```

**The Problem:**

The `backoff_level` state can get "stuck" at higher levels if there are periodic bursts of `QUEUE_FULL` errors. The recovery logic at lines 1081-1087 only reduces by one level per second:

```c
if (backoff_level > 0 && (now - last_qfull_us) > 1000000) {
    backoff_level--;
    last_qfull_us = now;  // Rate-limit recovery
}
```

If the network has intermittent issues every few seconds, backoff never fully recovers, and audio quality degrades progressively.

**Citation:** Lines 979-1087 of `mesh_net.c`

### Finding: No Routing Table Cleanup

The routing table (`esp_mesh_get_routing_table()`) can accumulate stale entries if nodes disconnect without proper `CHILD_DISCONNECTED` events (e.g., power loss, hard reset). The current code doesn't validate that routing table entries are still alive.

**Citation:** `mesh_net.c` `forward_to_children()` and `network_send_audio()`

---

## Proposed Fixes

### Fix 1: Preserve Self-Organized on Root

**Change:** In `MESH_EVENT_PARENT_CONNECTED`, only disable self-organized for non-root nodes.

```c
case MESH_EVENT_PARENT_CONNECTED: {
    // ... existing code ...
    
    // Only disable self-organized for non-root nodes
    // Root must keep accepting new children
    if (!esp_mesh_is_root()) {
        esp_mesh_set_self_organized(false, false);
        esp_wifi_scan_stop();
        ESP_LOGI(TAG, "Self-organized disabled (RX node, no more parent scans)");
    }
    break;
}
```

**In `MESH_EVENT_CHILD_CONNECTED`**, don't disable self-organized at all—just stop scanning:

```c
case MESH_EVENT_CHILD_CONNECTED: {
    // ... existing code ...
    
    if (is_mesh_root) {
        // Stop scanning but keep accepting children
        esp_wifi_scan_stop();
        esp_wifi_disconnect();  // Stop trying to connect to "MESHNET_DISABLED"
        // Do NOT call esp_mesh_set_self_organized(false, false)
    }
    break;
}
```

### Fix 2: Configure Mesh Queue Size

**Change:** After `esp_mesh_init()` and before `esp_mesh_start()`:

```c
// Increase mesh RX queue for multi-node scenarios (default is 32)
ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(64));

// Set AP association timeout to clean up stale connections
ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(30));  // 30 seconds
```

### Fix 3: Per-Child Flow Control

**Change:** Replace global `backoff_level` with per-child tracking.

```c
typedef struct {
    mesh_addr_t addr;
    uint8_t backoff_level;
    int64_t last_qfull_us;
} child_flow_t;

static child_flow_t child_flow[MESH_ROUTE_TABLE_SIZE];
static int child_flow_count = 0;

// In network_send_audio():
for (int i = 0; i < route_table_size; i++) {
    child_flow_t *flow = find_or_create_flow(&route_table[i]);
    
    // Skip if this specific child is backed off
    if (flow->backoff_level > 0 && should_skip_frame(flow)) {
        continue;  // Skip only for this child
    }
    
    esp_err_t send_err = esp_mesh_send(...);
    update_flow_state(flow, send_err);
}
```

### Fix 4: Stop Root Scanning at Startup

**Change:** In `network_init_mesh()`, after `esp_mesh_start()` for TX/COMBO nodes:

```c
ESP_ERROR_CHECK(esp_mesh_start());

if (my_node_role == NODE_ROLE_TX) {
    // Immediately stop any startup scans
    esp_wifi_scan_stop();
    // Disconnect STA to prevent "MESHNET_DISABLED" connection attempts
    esp_wifi_disconnect();
}
```

### Fix 5: Dynamic Jitter Buffer

**Change:** In `lib/audio/src/adf_pipeline.c`, adjust prefill based on network state:

```c
uint32_t get_dynamic_prefill_frames(void) {
    uint32_t base = JITTER_PREFILL_FRAMES;  // 3 frames = 60ms
    
    uint8_t layer = network_get_layer();
    uint32_t nodes = network_get_connected_nodes();
    
    // Add 20ms per hop beyond layer 1
    if (layer > 1) {
        base += (layer - 1);
    }
    
    // Add 20ms if many nodes (more contention)
    if (nodes >= 3) {
        base += 1;
    }
    
    return (base > JITTER_BUFFER_FRAMES) ? JITTER_BUFFER_FRAMES : base;
}
```

### Fix 6: Reset Accumulated State

**Change:** Add periodic state reset when network is stable:

```c
// In mesh_heartbeat_task or a new cleanup task:
static uint32_t stable_ticks = 0;

if (backoff_level == 0 && total_drops == last_snapshot_drops) {
    stable_ticks++;
    if (stable_ticks >= 15) {  // 30 seconds (2s heartbeat × 15)
        total_drops = 0;
        total_sent = 0;
        stable_ticks = 0;
        ESP_LOGI(TAG, "Network stable: reset TX counters");
    }
} else {
    stable_ticks = 0;
}
```

---

## Implementation Order

### Phase 1: Critical Fixes (Immediate)

| Priority | Fix | Files |
|----------|-----|-------|
| P0 | Fix 1: Preserve self-organized on root | `mesh_net.c` |
| P0 | Fix 2: Configure queue size | `mesh_net.c` |
| P0 | Fix 4: Stop root scanning | `mesh_net.c` |

### Phase 2: Reliability Improvements

| Priority | Fix | Files |
|----------|-----|-------|
| P1 | Fix 3: Per-child flow control | `mesh_net.c` |
| P1 | Fix 5: Dynamic jitter buffer | `adf_pipeline.c`, `build.h` |

### Phase 3: Maintenance

| Priority | Fix | Files |
|----------|-----|-------|
| P2 | Fix 6: Reset accumulated state | `mesh_net.c` |
| P2 | Routing table validation | `mesh_net.c` |

---

## Verification Plan

1. Flash all 4 nodes with fixed firmware
2. Boot SRC first, wait 10 seconds
3. Boot OUT1, verify connection within 15 seconds
4. Boot OUT2, verify connection within 15 seconds
5. Boot OUT3, verify connection within 15 seconds (**currently fails**)
6. Play audio source, verify all 3 OUTs receive simultaneously
7. Monitor serial for `QUEUE_FULL` or backoff messages
8. Run continuous test for 10+ minutes
9. Power cycle one OUT, verify it reconnects and resumes audio
10. Verify no memory leaks (`heap_caps_get_free_size()` stable)

---

## References

1. [ESP-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/esp-wifi-mesh.html)
2. [ESP-IDF mesh internal_communication example](https://github.com/espressif/esp-idf/tree/master/examples/mesh/internal_communication)
3. [esp_mesh.h API reference](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_mesh.h)
4. SoundMesh codebase: `lib/network/src/mesh_net.c`
5. SoundMesh architecture: `docs/planning/mesh-network-architecture.md`
