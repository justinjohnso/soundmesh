# TX WiFi AP Stability Issue - Debugging Session

**Date**: November 3, 2025  
**Status**: 🔴 Unresolved - TX AP dropping RX connections before DHCP completes

## Problem Statement

The RX unit cannot maintain a stable connection to the TX Access Point. The TX AP beacon is visible to external devices (phones, laptops) but drops ESP32 STA connections after ~4 seconds, before DHCP can complete IP assignment.

### Symptoms

- TX AP initializes successfully and broadcasts beacon
- RX finds TX AP in scan (`RSSI: -37 dBm`)
- RX connects to TX AP successfully
- **Connection drops after 4 seconds** (`wifi:state: run -> init (fc0)`)
- RX never receives IP address from DHCP
- Result: `ESP_ERR_INVALID_STATE` on all UDP receive calls

### Timeline of Investigation

#### Initial Hypothesis: I2C Display Blocking WiFi Stack

**Reasoning**: Display I2C operations on TX were suspected to block the WiFi stack, causing beacon timing issues and AP instability.

**Actions Taken**:
1. Verified I2C bus speed configured at maximum (400kHz) via `I2C_MASTER_FREQ_HZ`
2. Confirmed display throttling at 10Hz to minimize I2C overhead
3. Both optimizations were already in place

**Result**: ❌ TX AP still unstable despite I2C optimizations

#### Second Hypothesis: Broadcast Address Issue

**Reasoning**: ESP32 SoftAP may not forward limited broadcasts (`255.255.255.255`) to connected STAs.

**Actions Taken**:
1. Implemented directed broadcast (`192.168.4.255`) instead of `INADDR_BROADCAST`
2. Added subnet broadcast calculation from AP netif: `ip.addr | ~netmask.addr`
3. Added DHCP IP wait on RX before socket creation (40 x 250ms polling)
4. Enhanced logging for IP addresses and broadcast configuration

**Code Changes** ([mesh_net.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/network/src/mesh_net.c)):
```c
// TX AP mode - compute directed broadcast
esp_netif_ip_info_t ip_info;
ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
uint32_t directed_broadcast = ip_info.ip.addr | ~ip_info.netmask.addr;
// ...
broadcast_addr.sin_addr.s_addr = directed_broadcast;  // 192.168.4.255
```

```c
// RX STA mode - wait for DHCP IP before socket creation
esp_netif_ip_info_t ip_info = {0};
for (int i = 0; i < 40; i++) {
    esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (ip_info.ip.addr) break;
    vTaskDelay(pdMS_TO_TICKS(250));
}
```

**Result**: ❌ RX still cannot obtain IP - connection drops before DHCP completes

## Current Status

### TX Unit (Access Point Mode)
- ✅ AP initializes successfully
- ✅ Beacon visible to external devices
- ✅ Configured broadcast: `192.168.4.255`
- ✅ Sending audio packets (`seq=2943`, `seq=3071`, etc.)
- ✅ No crashes or error logs
- ⚠️ **Drops ESP32 STA clients after ~4 seconds**

### RX Unit (Station Mode)
- ✅ Finds TX AP in scan
- ✅ WiFi authentication succeeds
- ✅ Association succeeds (`wifi:state: assoc -> run`)
- ❌ **Connection dropped at 8145ms** (`wifi:state: run -> init (fc0)`)
- ❌ Never receives IP from DHCP
- ❌ All UDP operations return `ESP_ERR_INVALID_STATE`

### Key Log Evidence

**RX Connection Sequence**:
```
I (3395) wifi:new:<6,1>, old:<1,0>, ap:<255,255>, sta:<6,1>, prof:6
I (3405) wifi:state: init -> auth (b0)
I (3775) wifi:state: auth -> assoc (0)
I (3885) network: Connected to AP: MeshNet-Audio (RSSI: -37 dBm)
I (4165) wifi:state: assoc -> run (10)
I (8145) wifi:state: run -> init (fc0)  ← DISCONNECT
I (8145) wifi:new:<6,0>, old:<6,1>, ap:<255,255>, sta:<6,1>, prof:6
E (13885) network: Failed to obtain IP address from DHCP
```

**Disconnect Timing**:
- Connection established: `3.885s`
- Disconnect event: `8.145s`
- **Duration before disconnect: ~4.26 seconds**

## Hypotheses for Next Investigation

### 1. TX Watchdog or Task Starvation
The TX unit may be experiencing:
- Task WDT timeout on WiFi tasks
- CPU starvation from display/audio processing
- Insufficient stack space causing silent failures

**Test**: Disable display completely on TX and retry

### 2. TX AP DHCP Server Issue
The ESP32 DHCP server may be:
- Timing out before responding to DHCP requests
- Conflicting with main task execution
- Silently failing due to memory constraints

**Test**: Monitor TX logs for DHCP-related messages during RX connection attempt

### 3. TX Audio Processing Overload
The tone generation + UDP transmission may be:
- Consuming too much CPU time
- Blocking critical WiFi stack operations
- Preventing timely beacon transmission

**Test**: Disable audio generation/transmission on TX temporarily

### 4. WiFi Power Management Conflict
Despite disabling power-save mode, there may be:
- Default WiFi stack behavior disconnecting inactive clients
- Beacon timeout misconfiguration
- DTIM period issues

**Test**: Review TX WiFi initialization parameters

## Architecture Notes

### Current TX Task Structure
```
Main Loop (10ms):
  ├─ ADC Audio Capture
  ├─ Tone Generation
  ├─ UDP Broadcast (non-blocking)
  ├─ Display Update (10Hz throttle)
  └─ Button Polling

Background:
  ├─ WiFi Stack (FreeRTOS tasks)
  ├─ DHCP Server
  └─ Latency Measurement Task
```

### Resource Usage
- **TX RAM**: 12.6% (41,356 / 327,680 bytes)
- **TX Flash**: 78.4% (821,573 / 1,048,576 bytes)
- **Main Stack**: High water mark not logged for TX

## Next Steps

1. **Disable TX display** - Test if display is still causing issues despite 400kHz I2C
2. **Add TX WiFi event handlers** - Log station connect/disconnect events
3. **Monitor TX DHCP server** - Check if DHCP responses are being sent
4. **Disable latency measurement task** - Reduce TX background load
5. **Increase TX beacon interval** - May improve reliability
6. **Add TX main stack monitoring** - Check for stack overflow issues

## References

- [display_ssd1306.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/control/src/display_ssd1306.c#L168) - I2C config at 400kHz
- [mesh_net.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/network/src/mesh_net.c#L50-L86) - Directed broadcast implementation (TX)
- [mesh_net.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/lib/network/src/mesh_net.c#L172-L213) - DHCP wait + directed broadcast (RX)
- [tx/main.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/src/tx/main.c) - TX main loop
- [rx/main.c](file:///Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/src/rx/main.c#L225-L230) - RX display rendering (10Hz)

## Conclusion

The issue has **shifted** from packet delivery (broadcast addressing) to **connection stability** (TX dropping clients). The TX AP cannot maintain ESP32 STA connections long enough for DHCP to complete, despite being visible to other devices and showing no obvious errors in logs.

**Critical observation**: TX drops the RX connection with reason code `fc0` after exactly ~4 seconds, suggesting a timeout or watchdog issue rather than RF interference or distance problems.
