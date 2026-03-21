# Mesh Reliability Investigation & Fix Progress

**Date**: 2026-03-20  
**Status**: In Progress - Root Cause Analysis Complete

## 2026-03-21 Validation Cycle (Latest)

### 2026-03-21 Additional validation (stutter-focused telemetry + transport/jitter tuning)

#### Big-picture checkpoint (anti-rabbit-hole)
- Re-ran synchronized OUT telemetry before changing code.
- Confirmed stutter signature is still transport-side burst loss/jitter, not local decode failure:
  - OUT1/OUT2 both `connected=1`, both `audio=1`
  - Persistent packet loss around ~18-20%
  - Large ping jitter spikes (tens to hundreds of ms)
  - No dominant `decode failed`/local crash signature
- OUT3 remains join-blocked in repeated `AUTH_EXPIRE`/`NO_AP_FOUND`.

#### Root-cause-aligned change applied
- Reduced mesh packet rate pressure and widened startup cushion for bursty delivery:
  - `MESH_FRAMES_PER_PACKET`: `2 -> 3` (50fps source now ~16.7 mesh pkts/sec per destination instead of 25)
  - `JITTER_PREFILL_FRAMES`: `3 -> 4` (60ms -> 80ms prefill)
- File changed:
  - `lib/config/include/config/build.h`

#### Why this is root-cause aligned
- With 2 active OUTs, observed loss remained high even while both stayed connected.
- That pattern indicates airtime/queue contention on root send path (`P2P` per-destination fanout), not a pure RX state-machine issue.
- Lowering packets/sec directly reduces send-call frequency and contention pressure.
- Slightly higher prefill reduces audible underruns from burst gaps without masking connection failures.

#### Validation/rollout note
- This change affects sender+receiver protocol cadence (`MESH_FRAMES_PER_PACKET` used in TX batching and RX loss accounting).
- Full benefit requires SRC/COMBO root firmware and OUT firmware on the same revision.

#### Post-flash OUT telemetry (30s synchronized pass)
- Rebuilt `tx/rx/combo`.
- Reflashed all three OUT nodes with updated RX firmware.
- Observed:
  - OUT1 connected/audio active, reported loss ~15.5-16.3%, repeated jitter spikes, recurring underruns (`Underrun #80/#100`).
  - OUT2 connected/audio active, reported loss ~16.6-17.8%, recurring underruns (`Underrun #60/#80/#100`).
  - OUT3 still in repeated `AUTH_EXPIRE` with periodic `NO_AP_FOUND` and parent candidate `assoc:2`.
- Interpretation:
  - Improvement is modest vs prior ~18-20% windows, but stutter root cause remains transport burstiness (underruns still frequent).
  - 3rd-node join blocker is unchanged.
  - Because root-side sender cadence must match receiver assumptions, next decisive validation requires flashing SRC/COMBO with the same build.

### 2026-03-21 Additional validation (runtime self-organized toggle removal)

#### Change applied
- In `lib/network/src/mesh_net.c`, removed runtime `esp_mesh_set_self_organized(...)` calls from:
  - `MESH_EVENT_PARENT_CONNECTED`
  - `MESH_EVENT_PARENT_DISCONNECTED`
- Kept startup-time self-organized configuration in `network_init_mesh()`.
- Left root scan-stop behavior (`esp_wifi_scan_stop()`) in place where relevant.

#### Why this change
- The prior synchronized logs showed connected nodes rapidly oscillating:
  - `Parent connected` immediately followed by `Parent disconnected: ASSOC_LEAVE`.
- That pattern aligned with topology mode churn caused by event-time self-organized toggles, not just RF quality.

#### Validation steps
- Rebuilt all environments successfully: `tx`, `rx`, `combo`.
- Reflashed all OUT nodes:
  - `/dev/cu.usbmodem211101`
  - `/dev/cu.usbmodem211201`
  - `/dev/cu.usbmodem211301`
- Ran synchronized parallel monitoring on all 3 OUT serial ports.

#### Observed outcome
- ✅ `OUT1` and `OUT2` no longer show rapid `ASSOC_LEAVE` connect/disconnect storms.
- ✅ Both remain mesh-connected while audio quality remains variable (stream-loss cycling persists due to upstream burstiness/loss).
- ⚠️ `OUT3` still fails to complete join:
  - repeated `AUTH_EXPIRE`
  - occasional `NO_AP_FOUND`
  - never reaches stable `connected=1` state.

#### Root-cause impact assessment
- This fix addressed a **real root-cause contributor** for connected-node churn (not a symptom-only tweak).
- It did **not** resolve the final 3rd-node join blocker, which remains auth/association-path limited.
- Current top blocker remains: root-side association/auth behavior and/or asymmetric RF path quality for the third OUT.

### 2026-03-21 Additional validation (RX stream-loss confirmation window)

#### Change applied
- Added a second-stage confirmation window before RX declares stream loss:
  - `lib/config/include/config/build.h`: `STREAM_SILENCE_CONFIRM_MS=300`
  - `src/rx/main.c`: stream loss now requires:
    1) initial silence timeout (`STREAM_SILENCE_TIMEOUT_MS`)
    2) sustained silence for confirmation window (`STREAM_SILENCE_CONFIRM_MS`)

#### Why this change
- Even after earlier timeout tuning, OUT nodes still showed burst-induced `Streaming -> Stream Lost` flapping.
- This produced avoidable state churn and audio interruptions during brief packet gaps.

#### Validation steps
- Rebuilt `rx` successfully.
- Reflashed all OUT nodes:
  - `/dev/cu.usbmodem211101`
  - `/dev/cu.usbmodem211201`
  - `/dev/cu.usbmodem211301`
- Ran synchronized post-flash monitoring.

#### Observed outcome
- ✅ OUT1 showed long stable streaming windows with loss dropping toward ~5-6% before occasional stream-loss transitions.
- ✅ OUT2 showed improved continuity but still experienced intermittent `Streaming -> Stream Lost` under bursty windows.
- ⚠️ OUT3 remained join-blocked (`AUTH_EXPIRE` / occasional `NO_AP_FOUND`), unaffected by RX stream-state logic.

#### Root-cause impact assessment
- This change is **effective symptom mitigation** for stream flapping and receiver stability.
- It does not address the remaining dominant blocker: third-node join/auth path reliability.

### 2026-03-21 Additional validation (association-expire tuning + rollback)

#### Change tested
- Increased mesh AP association expiry from 30s to 45s (temporary experiment).

#### Observed outcome
- No reliable improvement in third-node join behavior.
- Persistent `AUTH_EXPIRE` loops remained on the non-joining OUT.
- Experiment introduced ambiguous side effects and did not improve reproducibility.

#### Action taken
- Reverted association expiry back to 30s baseline.
- Rebuilt and reflashed all OUT nodes after rollback.

### 2026-03-21 Additional validation (RX mesh-restart recovery experiment + rollback)

#### Change tested
- Added bounded RX-side mesh restart recovery for prolonged join loops.
- Goal was to break persistent `AUTH_EXPIRE` deadlock in OUT-only environment.

#### Observed outcome
- Recovery hook executed, but did not produce reliable break-out to stable join.
- Logs remained dominated by repeated `AUTH_EXPIRE` with no sustained parent connection for the affected node.

#### Action taken
- Removed mesh-restart recovery path and restored native retry baseline.
- Rebuilt and reflashed all OUT nodes after rollback.

### Final OUT-only validation snapshot (current baseline)

- OUT1: `connected=1` heartbeats observed.
- OUT2: `connected=1` heartbeats observed.
- OUT3: continued `AUTH_EXPIRE` loop with repeated candidate selection but no stable join.

### Final conclusion from this cycle

- OUT-side reliability improvements are real and retained:
  - connect/disconnect storm fix
  - stream-loss hysteresis hardening
- Remaining blocker is root-side association/auth path behavior for 3rd OUT joins.
- Further progress now requires SRC/COMBO-side instrumentation and validation during 3-node join attempts.

### What was changed and validated
- Rolled back the last AP-auth tuning experiment in `mesh_net.c` to remove the high-risk auth override path:
  - removed explicit `esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK)`
  - restored `esp_mesh_set_ap_assoc_expire(30)`
- Rebuilt all environments (`tx`, `rx`, `combo`) successfully.
- Reflashed all three OUT nodes successfully:
  - `/dev/cu.usbmodem211101`
  - `/dev/cu.usbmodem211201`
  - `/dev/cu.usbmodem211301`

### Observed behavior after reflash
- OUT3 still cannot complete join and remains in repeated `AUTH_EXPIRE` loops.
- OUT3 logs show:
  - successful candidate selection and `DONE connect to parent ... rssi:-24 ... [layer:1, assoc:2]`
  - immediate `wifi:state: auth -> init (200)` followed by `Parent disconnected: AUTH_EXPIRE`
- This is consistent over long windows (not transient).

### Root-cause update from this cycle
- The failure is no longer consistent with discovery/RSSI problems alone.
- The strongest current hypothesis is root-side association/auth path limitation in the active path (effective association ceiling pattern around `assoc:2`), not OUT stale state.
- This cycle reinforces that **OUT-side retry tuning is now a diminishing-return path**.

### Practical implication
- Because only OUT nodes are directly accessible here, the remaining critical fix path is on SRC/COMBO root behavior and configuration.
- OUT-side firmware is currently in a stable diagnostic state; next meaningful validation requires SRC/COMBO running the same latest mesh changes.

## Big Picture Analysis

### Original Vision (from docs/planning/mesh-network-architecture.md)
The system was designed as a **self-healing mesh** with:
- Automatic root election
- Multi-hop routing for extended range
- Tree broadcast for 1-to-many audio distribution
- Topology that reconfigures when nodes join/leave

### What Actually Got Implemented
A **fixed-root topology** where:
- TX/COMBO is forced to be `MESH_ROOT` via explicit API calls
- RX nodes wait indefinitely for the designated root
- "Self-organized" mode was partially disabled to reduce scanning pauses

### What Went Wrong
The simplification to fixed-root removed some of ESP-MESH's self-healing capabilities, but created edge cases where:
1. Root doesn't properly advertise to all nodes
2. Packet flow becomes bursty instead of continuous
3. Stream state cycling causes memory churn

## Root Causes Identified

### 1. **TX Not Sending Continuously** (PRIMARY CAUSE)
The TX encode task at `adf_pipeline.c:716` skips encoding when no input signal is detected:
```c
if (pipeline->input_mode != ADF_INPUT_MODE_TONE && !signal_present) {
    batch_count = 0;  // Clear batch
    continue;         // Skip sending
}
```

This creates 100ms+ gaps when:
- No audio input is connected
- Input signal falls below threshold
- TX is in LINE/USB mode without active input

**Evidence**: Stream cycling logs show `Stream Found → 100ms+ silence → Stream Lost` pattern.

### 2. **Stream Timeout Too Aggressive**
The 100ms silence timeout in `rx/main.c:352` triggers state cycling:
- Normal packet interval: ~40ms (25 packets/sec)
- Timeout threshold: 100ms
- Result: Missing just 2-3 packets triggers stream loss

Each stream loss/found cycle:
- Logs state transitions
- Resets receiving_from_src_id
- May allocate/free internal buffers

**Impact**: OUT2 went from 70KB free to 2KB free heap over ~3 minutes.

### 3. **OUT3 Physical/RF Issue**
After 13,675+ scan attempts, OUT3 still hasn't found the mesh. This is NOT a code bug:
- Scans are happening (logs show continuous attempts)
- Root is visible to OUT1 and OUT2
- Likely causes: physical distance, WiFi interference, or antenna orientation

### 4. **Per-Child Rate Limiting Was Overcomplicated**
Phase 2 added per-child flow control that was too aggressive:
- At backoff level 2, dropped 66% of frames
- Recovery required 1 full second without queue-full
- With 3 children × 50fps = 150 sends/sec, queue fills frequently

**Resolution**: Disabled rate limiting entirely (Phase 3). Natural backpressure via `MESH_DATA_NONBLOCK` is sufficient.

### 5. **Node-Specific Link/Auth Instability (Current Dominant Blocker)**
Latest field runs show asymmetric behavior:
- `OUT2`: long stable streaming windows, loss trending down to ~10-16%
- `OUT1`: unstable streaming with repeated stream loss and ~30% loss windows
- `OUT3`: repeated `AUTH_EXPIRE` join loops, never reaches parent-connected

This asymmetry persisted after full `erase + reflash` on OUT1 and OUT3, which strongly indicates root causes are now mostly **join path reliability and RF/topology quality**, not stale flash state.

---

## Latest Changes Applied (This Pass)

1. **TX airtime reduction**
   - `OPUS_BITRATE` changed `64000 -> 32000` in `build.h`
   - Goal: reduce per-child airtime contention for 1→many streaming

2. **RX stream-loss hysteresis increase**
   - `STREAM_SILENCE_TIMEOUT_MS` changed `300 -> 500` in `build.h`
   - Goal: reduce false stream-lost transitions during burst gaps

3. **Root startup churn reduction**
   - Removed `esp_wifi_disconnect()` call at TX startup in `network_init_mesh()`
   - Goal: avoid destabilizing root STA/AP state (also aligns with portal stability concerns)

4. **RX forced rejoin loop disabled**
   - Removed explicit forced rejoin call path on repeated `AUTH_EXPIRE`
   - Retained native ESP-MESH retry path with logging
   - Goal: avoid self-inflicted join churn

5. **Validation erase/reflash**
   - Full flash erase + reflash executed on OUT1 and OUT3
   - Result: OUT3 auth loop persisted, OUT1 still weaker than OUT2

---

## Latest Monitoring Snapshot

| Node | Join State | Audio State | Loss Trend | Key Signals |
|------|------------|-------------|------------|-------------|
| OUT1 | Connected | Flappy | ~30-32% | Stream lost/recovered cycles, occasional weak RSSI |
| OUT2 | Connected | Mostly stable | ~10-16% | Sustained RX windows, stable heap |
| OUT3 | Not connected | None | N/A | Persistent `AUTH_EXPIRE`, no stable parent connect |

## What I've Implemented

| Phase | Change | Root Cause? | Status |
|-------|--------|-------------|--------|
| 1 | Keep self-organized on root | ✅ Yes | Fixed |
| 1 | Increase queue to 64 | Partial | Fixed |
| 1 | Stop root scanning | ✅ Yes | Fixed |
| 2 | Per-child rate limiting | ❌ No (symptom) | Reverted |
| 2 | Dynamic jitter buffer | Partial | Kept |
| 3 | Disable rate limiting | ✅ Yes | Fixed |
| - | Fix seq number calculation | ✅ Yes (measurement) | Fixed |

## What Still Needs Fixing

### Immediate (Code Changes)
1. **Increase stream timeout** from 100ms to 300ms
   - Reduces state cycling
   - Reduces memory churn
   - Better tolerance for bursty TX

2. **Force TX to TONE mode** for testing
   - Eliminates input signal detection as variable
   - Verifies mesh path works end-to-end

### Physical/Operational
1. **Move OUT3 closer to SRC** or other OUT nodes
   - It should be able to join via multi-hop
   - May need line-of-sight to at least one node

2. **Verify SRC has audio input connected**
   - Or switch to TONE mode via button

## Current Measurements

| Node | Connected | Heap | Loss | Issue |
|------|-----------|------|------|-------|
| OUT1 | ✅ | 71KB | 17-20% | Stream cycling |
| OUT2 | ✅ | **2KB** | 20% | Memory exhausted |
| OUT3 | ❌ | ? | N/A | Can't find mesh |

## Problem Statement

User reported three critical issues with the mesh audio streaming:
1. Only 1-2 of 3 OUT nodes connect reliably
2. Connected nodes don't consistently receive audio
3. Adding multiple connected nodes causes stuttering degradation

## Investigation Summary

### Phase 1: Connection Reliability (COMPLETE)

**Root Cause Identified**: `esp_mesh_set_self_organized(false, false)` was called in `MESH_EVENT_PARENT_CONNECTED` for ALL nodes, including ROOT. This caused the ROOT to stop accepting new children after the first one connected.

**Fix Applied** (commit c822707):
- Only disable self-organized for non-root (RX) nodes
- Added `esp_mesh_set_xon_qsize(64)` to increase queue capacity
- Added `esp_mesh_set_ap_assoc_expire(30)` for stale association cleanup
- Stopped root WiFi scanning at startup

**Result**: All 3 OUT nodes now connect successfully.

### Phase 2: Audio Distribution (COMPLETE but PROBLEMATIC)

**Root Cause Identified**: Global backoff rate limiting was penalizing all children when any single queue filled up.

**Fix Applied** (commit bd3a79e):
- Implemented per-child flow control with independent rate limiting
- Added dynamic jitter buffer calculation based on network topology

**Problem Discovered**: The per-child rate limiting was TOO AGGRESSIVE:
- At backoff level 2, skips 2 of every 3 frames (66% drop rate)
- Recovery requires 1 full second with no queue-full events (too slow)
- With 3 children × 50fps audio = 150 sends/sec, queue fills frequently
- Result: System enters permanent backoff, causing ~60% measured packet loss

### Phase 3: Rate Limiting Disabled (CURRENT)

**Action**: Disabled per-child rate limiting entirely (`RATE_LIMIT_ENABLED=0`)
- With `MESH_DATA_NONBLOCK`, queue-full returns immediately and we try next frame
- This provides natural congestion handling without accumulated backoff state

**Code Change** (mesh_net.c):
```c
// Rate limiting DISABLED - was causing 60% packet loss by being too aggressive
#define RATE_LIMIT_ENABLED 0
```

**Status**: Change compiled into firmware but needs SRC/COMBO reflash to take effect.

## Critical Discovery: Console Configuration

**Issue**: XIAO ESP32-S3 requires USB Serial JTAG for console output, NOT UART.

**Symptom**: Nodes appeared to hang after "W (355) i2c:" warning - no output from app_main.

**Fix Applied**:
```ini
# sdkconfig.shared.defaults
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

Note: COMBO uses TinyUSB NCM so it correctly has `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n`.

## Current Measurements (After All Fixes + SRC Reflash)

### Per-Node Status

| Node | Connected | Heap Free | Packet Loss | Issue |
|------|-----------|-----------|-------------|-------|
| OUT1 | ✅ Yes | 71KB (76%) | 17-20% | Stream cycling (100ms timeouts) |
| OUT2 | ✅ Yes | **2KB (99%)** | 20% | ⚠️ **Memory exhausted!** |
| OUT3 | ❌ **NO** | Unknown | N/A | Stuck scanning (13,675+ attempts) |

### Overall Metrics

| Metric | Before | After | Status |
|--------|--------|-------|--------|
| Packet Loss | 56% | 17-20% | ⚠️ Better but still high |
| Ping Latency | 294-455ms | 30-440ms | ⚠️ Variable |
| RSSI | -31 dBm | -18 to -36 dBm | ✅ Good |
| Buffer Fill | 0-33% | 0-100% | ⚠️ Cycling |
| RX Bandwidth | 11-18 kbps | 8-61 kbps | ⚠️ Bursty |

### Critical New Issues Discovered

1. **OUT3 Cannot Join Mesh**: Stuck in `MESH_NWK_LOOK_FOR_NETWORK` after 13,675+ scan attempts. Cannot find root.

2. **OUT2 Memory Leak**: Only 2KB heap remaining (started with ~70KB). Likely caused by rapid stream state cycling allocating internal WiFi/mesh buffers.

Stream state still cycles rapidly: "Stream Found" → "100ms+ silence timeout"

## Bug Discovered: Sequence Number Mismatch

**Issue**: TX sends sequence numbers in increments of `MESH_FRAMES_PER_PACKET` (2), not 1.

- TX sends: seq 0, 2, 4, 6, 8...
- RX expected: seq 0, 1, 2, 3, 4...
- Result: RX counted every packet as having a gap of 1 → phantom 50% loss

**Fix Applied** (commit b4fb387):
```c
// Now expects seq += MESH_FRAMES_PER_PACKET between packets
uint16_t expected_seq = (last_seq + MESH_FRAMES_PER_PACKET) & 0xFFFF;
// And divides gap by frames-per-packet to count actual packets lost
dropped_packets += (gap / MESH_FRAMES_PER_PACKET);
```

**Result**: Reported loss dropped from ~56% to ~44%, revealing ~45% real loss remains.

## Files Modified

| File | Changes |
|------|---------|
| `lib/network/src/mesh_net.c` | Phase 1 fixes, Phase 2 per-child flow, Phase 3 rate limiting disable |
| `lib/network/include/network/mesh_net.h` | Added `network_get_jitter_prefill_frames()` |
| `lib/audio/src/adf_pipeline.c` | Dynamic jitter buffer calculation |
| `sdkconfig.shared.defaults` | USB Serial JTAG console, increased log level |
| `sdkconfig.rx.defaults` | USB Serial JTAG console |

## Commits

1. **c822707**: Phase 1 - Self-organized fix, queue config, scan stop
2. **bd3a79e**: Phase 2 - Per-child flow control, dynamic jitter buffer
3. **db1c2a5**: Phase 3 - Rate limiting disabled
4. **b4fb387**: Fix sequence number mismatch in packet loss calculation

## Remaining Investigation

The ~45% real packet loss suggests the TX is not sending consistently. Possible causes:

1. **Input Signal Detection**: TX only sends when `input_signal_present` is true. If no audio input is connected or signal detection is flaky, TX will send intermittently.

2. **WiFi Channel Congestion**: Even with good RSSI (-17dBm), WiFi interference could cause drops.

3. **Mesh Routing**: The mesh may be re-routing or having internal congestion.

**To verify**: Check if SRC is in TONE mode (always sends) or LINE/USB mode (only sends with signal). Toggle to TONE mode via button to test.

## Next Steps

### Immediate - Address Critical Issues

1. **OUT3 Not Connecting**: 
   - Physical placement issue? Move OUT3 closer to SRC
   - May need fresh flash (though we just did erase+flash)
   - Power cycle all nodes and observe boot order effects

2. **OUT2 Memory Leak**:
   - Likely caused by rapid stream cycling allocating ESP-IDF internal buffers
   - May need to increase stream silence timeout from 100ms to 250-500ms
   - Or add explicit cleanup of pending mesh data on stream loss

3. **Stream Cycling Problem**:
   - The 100ms silence timeout is too aggressive
   - TX may not be sending continuously (input signal detection?)
   - Consider increasing `STREAM_TIMEOUT_MS` in rx/main.c

### If Rate Limiting Fix Works
- Commit the change
- Consider re-enabling rate limiting with MUCH less aggressive parameters:
  - Lower max backoff level (1 instead of 2)
  - Faster recovery (100ms instead of 1000ms)
  - Only trigger after N consecutive queue-fulls

### If Rate Limiting Fix Doesn't Work
- Problem is elsewhere (SRC node CPU/memory, mesh congestion, WiFi interference)
- Need to investigate SRC node directly
- Consider:
  - Reducing audio frame rate (larger Opus frames = fewer packets)
  - Channel hopping to avoid WiFi congestion
  - Investigating portal (USB-NCM) stability issue

## Architecture Notes

The rate limiting runs on ROOT node during `network_send_audio()`:

```
TX Path: ES8388 → I2S → adf_enc task → network_send_audio() → esp_mesh_send() per child
```

The per-child loop iterates routing table and sends P2P to each descendant. If queue is full, that send fails with `ESP_ERR_MESH_QUEUE_FULL`.

Previous behavior: Queue-full triggered backoff level increase, causing frame skipping.
New behavior: Queue-full just loses that one frame, next frame tries again normally.

## Related Issues

**Portal Instability**: User reported SRC portal (USB-NCM) cycling between self-assigned IP and DHCP-assigned IP. This could indicate:
- WiFi stack contention
- USB power issues
- NCM interface competing with mesh WiFi

This may be related to the audio issues if the WiFi stack is unstable on SRC.
