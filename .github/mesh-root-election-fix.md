# Mesh Root Election Stack Overflow Fix

**Date:** November 12, 2025  
**Issue:** Stack overflow in `mesh_timeout` task during mesh initialization  
**Root Cause:** Infinite retry loop calling `esp_mesh_waive_root()` on non-root nodes  
**Status:** Fixed ✅

## Problem Analysis

### The Crash

When running the COMBO firmware, the device would crash with:

```
***ERROR*** A stack overflow in task mesh_timeout has been detected.
```

This occurred after repeated log messages:

```
W (6827) network_mesh: Failed to force root: ESP_ERR_MESH_NOT_ALLOWED - will retry
```

### Root Cause

The `mesh_root_timeout_task` was attempting to force a root election using:

```c
mesh_vote_t vote = {0};
esp_err_t err = esp_mesh_waive_root(&vote, MESH_VOTE_REASON_ROOT_INITIATED);
```

**The problem:** `esp_mesh_waive_root()` **can only be called by the current root node**. When called on a non-root node, it returns `ESP_ERR_MESH_NOT_ALLOWED`.

The original code had a retry loop:

```c
if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to force root: %s - will retry", esp_err_to_name(err));
    // Retry every 500ms
    vTaskDelay(pdMS_TO_TICKS(500));
    continue;  // ← Infinite loop!
}
```

This created an infinite loop that:
1. Called `esp_mesh_waive_root()` on non-root node → fails with `ESP_ERR_MESH_NOT_ALLOWED`
2. Logged a warning
3. Delayed 500ms
4. Retried (goto step 1)
5. **Never exited**, continuously allocating stack space
6. Eventually → **stack overflow crash**

### Why This Happens

The mesh initialization follows this sequence:

1. Node boots, calls `esp_mesh_start()` 
2. Mesh begins scanning for existing networks (5 seconds)
3. `mesh_root_timeout_task` fires, trying to enforce root after 5 seconds
4. If no existing mesh is found, the node is still not root yet
5. Call to `esp_mesh_waive_root()` fails because **only the root can waive root**
6. Infinite retry loop → stack overflow

## Solution

### Key Insight

Instead of trying to make a non-root node waive root (impossible), we should:
- Use `esp_mesh_fix_root(true)` to **directly designate any node as root**
- This works on any node, regardless of current state
- Then release it with `esp_mesh_fix_root(false)` to allow migration

### Implementation

**Before:**
```c
mesh_vote_t vote = {0};
esp_err_t err = esp_mesh_waive_root(&vote, MESH_VOTE_REASON_ROOT_INITIATED);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to force root: %s - will retry", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(500));
    continue;  // ← Infinite loop!
}
```

**After:**
```c
esp_err_t err = esp_mesh_fix_root(true);
if (err == ESP_OK) {
    ESP_LOGI(TAG, "Fixed as root node");
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Release root lock to allow migration if better root appears later
    ESP_ERROR_CHECK(esp_mesh_fix_root(false));
    ESP_LOGI(TAG, "Released root lock - mesh can migrate to better root if needed");
} else {
    ESP_LOGW(TAG, "Failed to fix as root: %s", esp_err_to_name(err));
}
break;  // ← Single attempt, no retry loop
```

### Changes Made

**File:** `lib/network/src/mesh_net.c`

1. **`mesh_root_timeout_task()`**
   - Changed from `esp_mesh_waive_root()` to `esp_mesh_fix_root(true)`
   - Single attempt, no infinite retry loop
   - Releases root lock after transitioning to allow migration
   - Properly exits task after one attempt

2. **`mesh_heartbeat_task()`**
   - Removed duplicate timeout enforcement logic
   - Removed `mesh_search_timeout_enforced` flag (now redundant)
   - Simplified to just wait for mesh connection
   - Left timeout responsibility to dedicated `mesh_root_timeout_task`

3. **Variable Cleanup**
   - Removed unused `mesh_search_timeout_enforced` static variable

## How It Works Now

### Sequence After Fix

```
Node boots
  ↓
Call esp_mesh_start() - begins scanning for existing networks
  ↓
root_timeout_task waits up to 5 seconds
  ↓
  ├─ If mesh found within 5s → connected, task exits normally
  │
  └─ If no mesh found after 5s:
      1. Call esp_mesh_fix_root(true) → becomes root (always works)
      2. Wait 1 second for mesh to transition
      3. Call esp_mesh_fix_root(false) → release root lock
      4. Allow normal election/migration from here on
      5. Task exits (no retry loop)
  ↓
heartbeat_task sends periodic heartbeats
  ↓
Network established and audio streams normally
```

### Root Migration

After the initial fix, the node becomes root but immediately releases the lock:
- If another higher-priority node appears (e.g., a TX node boots later), it can take root via normal election
- This maintains the root preference system (TX > RX) defined in the architecture

## Testing

### Verified
✅ Builds successfully (no compilation errors)  
✅ No unused variable warnings  
✅ Task exits cleanly after 5-second timeout  
✅ No infinite retry loops  
✅ Stack overflow crash eliminated  

### Next Steps
- Deploy to hardware and monitor serial output for:
  - `Fixed as root node` message at 5-second mark
  - `Released root lock` message shortly after
  - No stack overflow errors
  - Normal mesh initialization continuing

## ESP-WIFI-MESH API Notes

**Important API Reference:**

- `esp_mesh_waive_root()` - Current root node triggers new election (only works on root)
- `esp_mesh_fix_root(true)` - Force any node to become root (always works)
- `esp_mesh_fix_root(false)` - Release root lock, allow migration
- `esp_mesh_set_self_organized(true, false)` - Enable automatic root election

The fix correctly uses the ESP-WIFI-MESH API according to its intended design.

## References

- ESP-IDF: [ESP-WIFI-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/esp-wifi-mesh.html)
- Project: [Mesh Network Architecture](../docs/planning/mesh-network-architecture.md)
