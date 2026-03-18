# Plan: Stabilize Node Identity & Connection State Visibility

**Created:** March 18, 2026  
**Branch:** `feat/node-identity-connection-state`  
**Status:** Ready for implementation

---

## TL;DR

Build two interconnected systems to fix RX connectivity troubleshooting:

1. **SRC_ID**: Derive stable, display-friendly identity from MAC (format: `SRC_A1B2C3`) that TX exposes in heartbeats & audio frames, and RX reports in logs/OLED
2. **RX State Machine**: Implement explicit connection states (Init → Mesh Joining → Mesh Ready → Waiting for Stream → Streaming) visible on OLED & serial, enabling precise diagnosis of connection failures during playtests

---

## Implementation Phases

### Phase 1: SRC Identity Infrastructure *(Foundation — all later phases depend on this)*
- Add `derive_src_id()` utility in `lib/network/include/network/mesh_net.h` to convert MAC bytes to `"SRC_A1B2C3"` format
- Store as global `char g_src_id[12]` in `lib/network/src/mesh_net.c`, populated at `network_init_mesh()` time
- Update `mesh_heartbeat_t` struct in `lib/network/include/network/mesh_net.h` to include `char src_id[12]` field
- Populate heartbeat SRC_ID in `lib/network/src/mesh_net.c` broadcast loop
- Add `network_get_src_id()` utility for consistent access across TX/RX

### Phase 2: TX-Side Visibility *(depends on Phase 1)*
- Update `src/tx/main.c` to log `"TX STARTED - SRC_ID: SRC_XXXXXX, Root: YES"` after network init succeeds (~line 140)
- Verify heartbeat transmission includes SRC_ID (automatic from Phase 1)
- Mirror changes to `src/combo/main.c`

### Phase 3: RX Connection State Machine *(depends on Phase 1)*
- Create 8-state enum in `src/rx/main.c`:
  - `RX_STATE_INIT`
  - `RX_STATE_MESH_JOINING`
  - `RX_STATE_MESH_READY`
  - `RX_STATE_WAITING_FOR_STREAM`
  - `RX_STATE_STREAM_FOUND`
  - `RX_STATE_STREAMING`
  - `RX_STATE_STREAM_LOST`
  - `RX_STATE_ERROR_NO_MESH`
- Add globals: `current_state`, `receiving_from_src_id[12]` (stores SRC_ID of active stream), `state_change_time`
- Implement `rx_set_connection_state()` to log all transitions with reason & timestamp
- Hook into existing callbacks:
  - app boot → `INIT`
  - parent connect event → `MESH_READY`
  - first audio frame → `STREAM_FOUND` & store SRC_ID
  - 100ms+ silence timeout → `STREAM_LOST`
  - mesh disconnect → `ERROR_NO_MESH`

### Phase 4: RX-Side Display & Logging *(depends on Phase 3)*
- Create `rx_state_to_string()` utility mapping enum to human-readable names
- **OLED enhancement**: Add connection state line on display (e.g., `"State: [Streaming]"`, `"From: SRC_A1B2C3"` if receiving)
- **Serial logging**: Log every state transition with timestamp; every 5s in non-streaming states show `"Still waiting for stream (State: X, elapsed: Ys)"`
- (Optional) Update portal if it reads serial telemetry

### Phase 5: Validation & Testing *(iterative, verifies all phases)*
1. **SRC_ID stability**: Boot TX 3× → SRC_ID should never change (MAC-derived)
2. **State transitions**: Boot RX→TX → verify serial shows correct state chain with timestamps
3. **Multi-RX verification**: Boot 2 RX + 1 TX → both RX show same SRC_ID
4. **Failure recovery**: Kill TX → RX shows `STREAM_LOST` → `NO_MESH`; restart TX → auto-recovery
5. **OLED readability**: State + SRC_ID visible on 128x64 display without clutter

---

## Key Files to Modify

### Network Layer (Phase 1)
- `lib/network/include/network/mesh_net.h` — Add SRC_ID derivation function + update heartbeat struct
- `lib/network/src/mesh_net.c` — Implement SRC_ID logic, populate heartbeat, expose getter utility

### TX (Phase 2)
- `src/tx/main.c` — Add startup logging after network init succeeds
- `src/combo/main.c` — Mirror TX logging

### RX (Phases 3–4)
- `src/rx/main.c` — State machine enum, globals, transition function, callback hooks, OLED display update, serial logging

### Audio (Phase 1)
- `components/audio/include/audio_frame.h` OR `lib/audio/src/` — Add `char src_id[12]` to audio frame metadata

---

## Implementation Notes

### SRC_ID Format
- Derived from MAC address's first 3 octets in hex: `SRC_A1B2C3`
- Stable across reboots (MAC-based, no persistent storage needed)
- 12-byte buffer: `"SRC_XXXXXX\0"`

### RX State Transitions
- **On mesh join**: `MESH_JOINING` → `MESH_READY` (fires on `MESH_EVENT_PARENT_CONNECTED`)
- **On first audio frame**: Extract SRC_ID from frame header, transition `WAITING_FOR_STREAM` → `STREAM_FOUND` → `STREAMING`; store SRC_ID in `receiving_from_src_id`
- **On stream silence (100ms+)**: `STREAMING` → `STREAM_LOST` (existing jitter buffer provides tolerance)
- **On mesh disconnect**: `STREAMING` → `ERROR_NO_MESH` (or from any state if parent lost)

### OLED Display Changes
- Keep existing metrics (latency, RSSI, buffer %)
- Add state line at top: `"State: [Streaming]"` with state name
- If streaming, show: `"From: SRC_A1B2C3"` (the MAC-derived source ID)
- If not streaming, show elapsed time: `"State: [Waiting 5s...]"`

---

## Scope & Decisions

**Included:**
- MAC-based SRC_ID (no collision risk for single TX scenario)
- 8-state RX machine covering all connection phases
- OLED + serial visibility of states
- Single TX + multiple RX support

**Excluded (Future Work):**
- Multi-TX support (design is ready, implementation deferred)
- Custom node names (requires NVS storage; future enhancement)
- Automatic failover to backup TX

**Assumptions:**
- MAC address stable across reboots
- OLED display (SSD1306) present on RX nodes
- Serial debug channels available on both TX/RX
- Single TX + multiple RX deployment scenario

---

## Testing Strategy

### Unit-Level
- Verify `derive_src_id()` produces consistent output for same MAC
- Verify state transition function logs correctly with timestamps

### Integration-Level
- Boot TX alone → verify SRC_ID in logs + heartbeat
- Boot RX before TX → verify `MESH_JOINING` state on OLED
- When TX boots → RX transitions to `MESH_READY`
- When audio flows → RX shows `STREAMING + From: SRC_XXXXXX`

### Failure Scenarios
- Kill TX while RX streaming → verify `STREAM_LOST` → `ERROR_NO_MESH`
- Restart TX → verify RX auto-recovers to `STREAMING`
- Multi-RX (2 receivers, 1 TX) → verify both show same SRC_ID
- Silence for 100ms+ → verify `STREAMING` → `STREAM_LOST` transition

---

## Success Criteria

✅ SRC_ID visible on TX startup log + every heartbeat (10s interval)  
✅ RX displays connection state on OLED (current state + elapsed time)  
✅ RX displays "From: SRC_XXXXXX" when streaming  
✅ All state transitions logged to serial with timestamps  
✅ Multi-RX nodes correctly identify same SRC_ID  
✅ Connection failures diagnosable from serial log + OLED display  

---

## Further Considerations

### State Transition Debouncing
Current implementation uses 100ms silence detection. Consider testing with 1–2s threshold to reduce false `STREAM_LOST` transitions during normal mesh latency spikes. Adjust based on observed real-world mesh stability.

### Future: SRC_ID Pairing
Once multi-TX support is added, RX should support config like `"connect to SRC_A1B2C3"`. This plan lays groundwork (SRC_ID in headers); selective pairing deferred to v0.3+.

### Portal Integration
Web dashboard should display RX state + current SRC_ID when portal v0.2+ launches.