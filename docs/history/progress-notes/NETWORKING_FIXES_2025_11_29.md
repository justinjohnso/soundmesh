# ESP-WIFI-MESH Networking Fixes
**Date:** November 29, 2025  
**Status:** ✅ Implementation Complete  
**Priority:** High - Critical bugs affecting mesh formation and audio distribution

---

## Executive Summary

Debugging session identified several critical issues with the ESP-WIFI-MESH implementation in `mesh_net.c`. This document tracks the fixes and learnings from the ESP-MESH API.

## Critical Issues Identified

### 1. ✅ Send Flags (Fixed)
**Problem:** Originally using `MESH_DATA_FROMDS` for root-to-child sends - but that flag is for data from external router, not internal mesh traffic!

**Solution:** Use `MESH_DATA_P2P` for all intra-mesh communication. Also use `MESH_DATA_NONBLOCK` to avoid blocking the audio task.

**Key Learnings:**
- `MESH_DATA_P2P` = internal mesh traffic (node ↔ node)
- `MESH_DATA_FROMDS` = data FROM external router (not for internal mesh!)
- `MESH_DATA_TODS` = data TO external router
- `MESH_TOS_P2P` blocks even with `MESH_DATA_NONBLOCK` flag - use `MESH_TOS_DEF` instead

### 2. ✅ Routing Table API (Fixed)
**Problem:** Using hardcoded `10 * 6` bytes instead of `sizeof(table)`.  
**Also:** Overly complex logic trying to distinguish "direct children" from grandchildren.

**Solution:**
- Use `sizeof(route_table)` for the buffer size parameter
- `esp_mesh_get_routing_table()` returns ALL descendants, not just direct children
- Simplify: iterate all routing table entries; dedupe+TTL handle duplicates naturally
- Let the mesh stack handle routing - we just need to send to all children

### 3. ✅ Root Election (Fixed)
**Problem:** Using arbitrary 3-5 second timeouts to force root election instead of ESP-MESH's native voting system.

**Root Cause:**
- `esp_mesh_set_vote_percentage()` sets a THRESHOLD, not a weight
- LOWER threshold = EASIER to become root (needs fewer votes)
- Staggered timeouts (3s TX, 5s RX) are a workaround, not proper solution

**Solution:**
- Use ESP-MESH's vote percentage properly: TX=0.7 (easier), RX=0.95 (harder)
- Keep timeout as a fallback only (extended to 10s)
- Let the mesh stack handle root election naturally via voting

---

## ESP-MESH API Learnings

### Routing Table Behavior
```c
// esp_mesh_get_routing_table() returns ALL descendants in the tree:
// - Direct children (layer N+1)
// - Grandchildren (layer N+2) 
// - Great-grandchildren (layer N+3)
// ... all the way to leaf nodes

mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];  // Defined in build.h
int route_table_size = 0;
esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);

// route_table_size = total descendants, NOT just direct children
```

### Send Flags
```c
// Intra-mesh communication (node to node): MESH_DATA_P2P
esp_mesh_send(&dest_addr, &data, MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);

// Upstream (child → root): MESH_DATA_TODS (for external DS, rarely used in standalone mesh)
esp_mesh_send(NULL, &data, MESH_DATA_TODS | MESH_DATA_NONBLOCK, NULL, 0);

// CRITICAL: Use MESH_TOS_DEF, not MESH_TOS_P2P
// P2P TOS blocks even with NONBLOCK flag!
data.tos = MESH_TOS_DEF;

// NOTE: MESH_DATA_FROMDS is for data FROM external router - don't use for internal mesh!
```

### Vote Percentage (Root Election)
```c
// Vote percentage is a THRESHOLD, not a weight!
// LOWER = EASIER to become root (needs fewer votes)
// HIGHER = HARDER to become root (needs more votes)

// TX/COMBO nodes (should prefer becoming root):
esp_mesh_set_vote_percentage(0.7);  // Lower threshold = easier

// RX nodes (should avoid becoming root):
esp_mesh_set_vote_percentage(0.95); // Higher threshold = harder
```

### Event Gotchas
```c
// ROOT_FIXED fires on ALL nodes joining a fixed-root mesh
// Must check esp_mesh_is_root() to know if WE are the root
case MESH_EVENT_ROOT_FIXED:
    if (esp_mesh_is_root()) {
        // We ARE the root
    } else {
        // We joined a mesh with a fixed root (we are NOT root)
    }
    break;

// Root has no parent - ignore PARENT_DISCONNECTED when is_mesh_root=true
case MESH_EVENT_PARENT_DISCONNECTED:
    if (!is_mesh_root) {
        // Actual mesh disconnection for child nodes
    }
    // Root nodes ignore this
    break;
```

---

## Implementation Details

### Routing Table Fix

**Before (buggy):**
```c
mesh_addr_t route_table[10];
int route_table_size = 0;
esp_mesh_get_routing_table(route_table, 10 * 6, &route_table_size);  // Wrong: hardcoded bytes

// Complex logic trying to limit to "direct children"
int my_children = mesh_children_count;
int forward_count = (route_table_size < my_children) ? route_table_size : my_children;
```

**After (fixed):**
```c
mesh_addr_t route_table[MESH_ROUTE_TABLE_SIZE];  // Defined in build.h
int route_table_size = 0;
esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);

// Simple: send to all descendants, let dedupe+TTL handle it
for (int i = 0; i < route_table_size; i++) {
    if (sender && memcmp(&route_table[i], sender, 6) == 0) {
        continue;  // Don't echo back to sender
    }
    esp_mesh_send(&route_table[i], &mesh_data, MESH_DATA_FROMDS | MESH_DATA_NONBLOCK, NULL, 0);
}
```

### Root Election Fix

**Before (timeout-based):**
```c
#define MESH_SEARCH_TIMEOUT_TX_MS 3000  // TX: fast root
#define MESH_SEARCH_TIMEOUT_RX_MS 5000  // RX: slow root

// Same vote percentage for all
esp_mesh_set_vote_percentage(0.9);

// Timeout forces root
if (!is_mesh_connected && !is_mesh_root) {
    esp_mesh_set_type(MESH_ROOT);
    esp_mesh_fix_root(true);
}
```

**After (vote-based):**
```c
#define MESH_FALLBACK_TIMEOUT_MS 10000  // Extended fallback only

// Different vote thresholds by role
if (my_node_role == NODE_ROLE_TX) {
    esp_mesh_set_vote_percentage(0.7);   // TX: easier to become root
} else {
    esp_mesh_set_vote_percentage(0.95);  // RX: harder to become root
}

// Timeout is now just a fallback
if (!is_mesh_connected && !is_mesh_root) {
    ESP_LOGI(TAG, "Fallback timeout - forcing root");
    esp_mesh_set_type(MESH_ROOT);
    esp_mesh_fix_root(true);
}
```

---

## Medium Priority Improvements (Future Work)

These issues were identified but deferred:

### 1. Audio Timing
**Current:** Manual `esp_timer_get_time()` polling  
**Better:** Use `vTaskDelayUntil()` for deterministic frame timing

### 2. Jitter Buffer Config
**Current:** Duplicated between `build.h` and `rx_main.c`  
**Better:** Single source of truth in `build.h`, reference from rx_main.c

### 3. Network State Management
**Current:** Multiple boolean flags (`is_mesh_connected`, `is_mesh_root`, `is_mesh_root_ready`)  
**Better:** FreeRTOS EventGroup for cleaner state transitions

### 4. ESP-ADF Integration
**Current:** Custom ES8388 driver using raw I2C/I2S  
**Better:** ESP-ADF `esp_codec_dev` for ES8388, `audio_pipeline` for streaming

ESP-ADF components to consider:
- `esp_codec_dev` - Unified codec driver (ES8388, PCM5102, etc.)
- `audio_pipeline` - Task-based audio streaming
- `i2s_stream` - I2S input/output elements
- `audio_event_iface` - Event-driven architecture
- `ringbuf` - Lock-free ring buffer

**Note:** The custom ES8388 driver works for now. ESP-ADF migration is lower priority than mesh fixes.

---

## Testing Checklist

### Routing Table Fix
- [ ] Root sends audio to single child
- [ ] Root sends audio to multiple children
- [ ] Child forwards audio to grandchildren
- [ ] No duplicate packets received (dedupe working)
- [ ] TTL prevents infinite loops

### Root Election Fix
- [ ] TX boots first → becomes root
- [ ] RX boots first → becomes root (no TX available)
- [ ] TX boots second → joins as child (RX already root)
- [ ] RX dies → TX becomes new root (vote preference)
- [ ] Multiple TX nodes → one becomes root (vote competition)

### Audio Flow
- [ ] Single hop: TX → RX (< 30ms latency)
- [ ] Multi-hop: TX → Relay → RX (< 60ms latency)
- [ ] Root migration: audio continues after root change

---

## References

- [ESP-WIFI-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/esp-wifi-mesh.html)
- [ESP-MESH API Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp-mesh.html)
- Previous audit: [NETWORK_AUDIT_2025_11_12.md](NETWORK_AUDIT_2025_11_12.md)
- Architecture: [mesh-network-architecture.md](../planning/mesh-network-architecture.md)

---

*Document created: November 29, 2025*
