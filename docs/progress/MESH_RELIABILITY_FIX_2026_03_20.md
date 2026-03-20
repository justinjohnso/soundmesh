# Mesh Reliability Investigation & Fix Progress

**Date**: 2026-03-20  
**Status**: In Progress - **USER ACTION REQUIRED**: Reflash SRC Node

## 🚨 Critical Action Required

The rate limiting fix is ready but needs to be applied to the SRC/COMBO node:

```bash
pio run -e combo -t upload --upload-port /dev/cu.usbmodem21401
```

This should significantly reduce packet loss from ~60% to <5%.

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

## Current Measurements (OUT Node 1)

| Metric | Value | Expected | Status |
|--------|-------|----------|--------|
| Packet Loss | 56% | <5% | ❌ Critical |
| Ping Latency | 294-455ms | <50ms | ❌ Critical |
| RSSI | -31 dBm | - | ✅ Good |
| Buffer Fill | 33% | 50-80% | ⚠️ Low |
| RX Bandwidth | 11-18 kbps | ~40 kbps | ❌ Low |

Stream state cycles rapidly: "Stream Found" → "100ms+ silence timeout" every 100-200ms.

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
3. **Pending**: Phase 3 - Rate limiting disabled

## Next Steps

### Immediate (User Action Required)
1. Flash SRC/COMBO node: `pio run -e combo -t upload --upload-port /dev/cu.usbmodem21401`
2. Monitor packet loss after reflash
3. Test audio playback on all 3 OUT nodes

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
