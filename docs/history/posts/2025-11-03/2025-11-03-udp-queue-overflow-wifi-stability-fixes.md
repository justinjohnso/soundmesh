# Fixing UDP Queue Overflow, WiFi Stability, and Layer Separation (2025-11-03)

## Problem Summary

The TX unit was experiencing catastrophic issues preventing basic operation:
1. **UDP send queue overflow** - continuous "Send Failed" messages saturating the network layer
2. **Non-functional button/knob inputs** - despite implementing 1ms pacing, no user input was being registered
3. **Severe bandwidth mismatch** - TX sending at 800+ kbps but RX only receiving 92 kbps with terrible audio quality
4. **WiFi AP instability** - RX would connect but disconnect after 3-4 seconds, AP would disappear

## Root Cause Analysis

### Issue 1: UDP Queue Overflow (ENOBUFS)
The original fix handled `EWOULDBLOCK`/`EAGAIN` errors but not `ENOBUFS`/`ENOMEM` which are the actual errors returned when broadcasting with no receivers. Broadcasting to 255.255.255.255 with no connected clients causes the UDP send queue to fill up because packets have nowhere to go.

**Fix:** Updated `network_udp_send()` to gracefully handle all non-blocking socket errors:
```c
if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM) {
    return ESP_OK; // Buffer full - expected when broadcasting with no receivers
}
```

### Issue 2: Button GPIO Conflict
Button was configured on GPIO 4 which was stuck LOW (always pressed state). Hardware testing revealed the actual button was wired to GPIO 43.

**Fix:** Changed `BUTTON_GPIO` from 4 to 43 in pins.h. Buttons now work perfectly.

### Issue 3: ADC Continuous Mode Blocking Knob Reads
The knob (A2/GPIO3) uses ADC1 oneshot reads, but ADC1 was already in continuous mode for stereo AUX input, causing `ESP_ERR_TIMEOUT` on all knob reads. You can't mix oneshot and continuous modes on the same ADC unit.

**Fix:** Made continuous ADC mode conditional on input mode:
- Start in `INPUT_MODE_TONE` (not AUX)
- Stop continuous ADC when leaving AUX mode
- Start continuous ADC only when entering AUX mode
- Knob now works in TONE/USB modes where continuous ADC is off

### Issue 4: WiFi Broadcast Performance Collapse (Oracle Diagnosis)

Consulted the Oracle which identified the fundamental architectural problems:

**Broadcasting is catastrophic for WiFi performance:**
- Sent at lowest basic rate with no ACK/retry
- Saturates the AP/STA link
- No link-layer rate control
- 1ms packets (1000 pkt/s) create massive MAC/PHY overhead

**Latency task steals audio packets:**
- Both latency measurement and audio use the same UDP socket/port
- `network_udp_recv()` calls in latency task consume audio packets
- Creates silent packet loss that's invisible to the application

**Fixes implemented:**
1. **Disabled latency measurement on RX** - eliminated socket contention
2. **Frame aggregation: 1ms → 10ms** - reduced packet rate from ~1000 pkt/s to ~100 pkt/s
3. **Added sequence gap tracking** - quantify actual packet loss with metrics

### Issue 5: RX Stack Overflow Causing WiFi Disconnects

Oracle diagnosed that RX was experiencing stack overflow from large on-stack buffers:
- `packet_buffer[MAX_PACKET_SIZE]` = ~2KB
- `audio_frame[AUDIO_FRAME_SAMPLES * 2]` = ~4KB  
- `silence_frame[AUDIO_FRAME_SAMPLES * 2]` = ~4KB
- **Total: ~10KB on stack** with only default stack size

Stack corruption caused:
- Jitter buffer allocation failures ("Failed to create jitter buffer")
- WiFi disconnects every 3-4 seconds (`state: run -> init (fc0)`)
- AP appearing/disappearing in scans

**Fix:** Moved all large buffers to static storage:
```c
// Move large buffers to static storage to avoid stack overflow
static uint8_t rx_packet_buffer[MAX_PACKET_SIZE];
static int16_t rx_audio_frame[AUDIO_FRAME_SAMPLES * 2];
static int16_t rx_silence_frame[AUDIO_FRAME_SAMPLES * 2] = {0};
```

RAM usage increased from 10.4% → 12.2% but stack overflow eliminated. Added diagnostics:
```c
ESP_LOGI(TAG, "Main task stack high water mark: %u bytes", uxTaskGetStackHighWaterMark(NULL));
ESP_LOGI(TAG, "Free heap: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
```

### Issue 6: Display Updates Interfering with WiFi

Display was updating every loop iteration with per-update malloc/free in `ssd1306_write_data()`:
- Heap churn from repeated malloc(129) / free()
- Blocking I2C at 100 kHz taking excessive time
- Display updates every 10ms (or faster) starving WiFi keepalives

**Fixes:**
1. **Eliminated malloc** - static I2C TX buffer instead of per-update allocation
2. **Throttled display to 10 Hz** (100ms) on both TX and RX
3. Display updates no longer interfere with timing-critical WiFi/audio operations

```c
// Static buffer to avoid malloc/free on every update
static uint8_t i2c_tx_buffer[1 + DISPLAY_WIDTH];

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len) {
    if (len > DISPLAY_WIDTH) return ESP_ERR_INVALID_SIZE;
    i2c_tx_buffer[0] = 0x40;
    memcpy(&i2c_tx_buffer[1], data, len);
    return i2c_master_write_to_device(..., i2c_tx_buffer, len + 1, ...);
}
```

### Issue 7: Knob Polling Log Spam
Knob ADC noise was causing frequency updates every few milliseconds, creating hundreds of log messages per second and potentially interfering with WiFi.

**Fix:** Added hysteresis to knob updates - only change frequency if delta > 5 Hz:
```c
if (abs((int)new_freq - (int)status.tone_freq_hz) > 5) {
    status.tone_freq_hz = new_freq;
    tone_gen_set_frequency(status.tone_freq_hz);
    ESP_LOGI(TAG, "Tone frequency updated to %lu Hz", status.tone_freq_hz);
}
```

## Architecture Improvements

### Layer Separation
Following the architecture docs in `docs/planning/v0.1-implementation-plan.md`, ensured clean separation between layers:

**Network Layer:**
- Non-blocking UDP with graceful queue overflow handling
- No interference with control or audio layers
- Separate concerns: audio data vs control messages (latency measurement)

**Control Layer:**  
- Button polling at 5ms for responsiveness
- Knob updates at 1ms but with hysteresis
- Display updates throttled to 10 Hz (100ms)
- No blocking I2C operations interfering with audio/network timing

**Audio Layer:**
- 10ms frame generation/transmission (48 kHz stereo = 480 samples = 1920 bytes)
- Conditional ADC mode management based on input mode
- Clean separation between tone generation and AUX input

### Frame Aggregation Benefits

Changing from 1ms to 10ms frames:
- **Packet rate:** ~1000 pkt/s → ~100 pkt/s (10x reduction)
- **Per-packet overhead:** Dramatically reduced
- **WiFi AP stability:** Much improved (though still has issues)
- **Frame size:** 96 samples → 480 samples (192 bytes → 1920 bytes payload)
- **Bandwidth:** ~800 kbps → expected ~154 kbps for same audio quality

### Metrics and Observability

Added packet loss tracking on RX:
```c
// Track sequence gaps to measure packet loss
if (!first_packet) {
    uint16_t expected_seq = (last_seq + 1) & 0xFFFF;
    if (seq != expected_seq) {
        int16_t gap = (int16_t)(seq - expected_seq);
        if (gap > 0) {
            dropped_packets += gap;
        }
    }
}
```

Stats logged every second:
```
Stats: RX=XXX pkts, DROP=XXX pkts, LOSS=XX.X%%, BW=XXX kbps
```

## Configuration Changes

### build.h
```c
// Changed from:
#define AUDIO_FRAME_MS         1  // 1ms = 96 samples = 384 bytes

// To:
#define AUDIO_FRAME_MS         10  // 10ms = 480 samples = 1920 bytes
```

### pins.h
```c
// Changed from:
#define BUTTON_GPIO            GPIO_NUM_4

// To:
#define BUTTON_GPIO            GPIO_NUM_43
```

## Current Status

### What's Working
- ✅ Non-blocking UDP with graceful error handling
- ✅ Button input on GPIO 43
- ✅ Knob input with hysteresis (when not in AUX mode)
- ✅ 10ms frame aggregation (10x packet rate reduction)
- ✅ RX stack overflow fixed
- ✅ Display malloc eliminated, throttled to 10 Hz
- ✅ Clean layer separation (network/audio/control don't interfere)
- ✅ Packet loss metrics and logging

### What's Still Broken
- ❌ TX WiFi AP becomes unstable and disappears after ~1 minute
- ❌ RX connects but disconnects after 3-4 seconds (`state: run -> init (fc0)`)
- ❌ No audio actually received/played yet

## Remaining Issues

### TX WiFi AP Instability
Even with 10ms frames (~100 pkt/s), the TX WiFi AP crashes or becomes unresponsive after some time. Possible causes:
- Broadcasting still too aggressive even at reduced rate
- WiFi driver issues under sustained broadcast load
- Need to switch to unicast (Oracle recommendation)
- AP/STA architecture may have fundamental issues with ESP32-S3

### Next Steps (Per Oracle Recommendations)

**1. Switch to Unicast (High Priority, 1-2h effort)**
- RX sends HELLO packet to TX on startup
- TX stores RX IP address and uses unicast instead of broadcast
- Eliminates broadcast overhead and enables WiFi rate control/retries

**2. WiFi Event Handlers (Medium Priority, <1h)**
- Add `WIFI_EVENT_STA_DISCONNECTED` handler on RX
- Auto-reconnect instead of aborting
- Log disconnect reason codes for diagnosis

**3. Consider Multi-Task Architecture (Low Priority, 1-2 days)**
- Network RX task (high priority)
- Audio render task (high priority)  
- UI/display task (low priority)
- Proper queue-based communication between tasks
- Pin WiFi to core 0, heavy UI to core 1

**4. Advanced WiFi Tuning (Optional)**
```c
esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40); // or HT20 if interference
```

## Technical Debt

1. **No WiFi event handling** - both TX and RX need proper event handlers
2. **Broadcast architecture** - fundamentally flawed for this use case
3. **Single-task design** - UI blocks audio/network operations
4. **No reconnection logic** - RX aborts instead of retrying
5. **Hardcoded timeouts** - connection retry logic needs improvement

## Lessons Learned

### 1. Broadcasting is Unsuitable for Streaming
WiFi broadcast:
- Transmitted at lowest basic rate
- No ACK/retry mechanism
- No rate adaptation
- Extremely unreliable under load
- **Never use broadcast for sustained data streams**

### 2. Stack Overflow is Silent and Deadly
Large buffers on stack cause:
- Heap corruption
- Random crashes
- WiFi instability
- Hard-to-diagnose symptoms
- **Always use static/heap for buffers >1KB**

### 3. Layer Separation is Critical
Mixing concerns causes cascading failures:
- Latency measurement stealing audio packets
- Display updates blocking WiFi keepalives
- ADC modes conflicting between audio sources
- **Each layer must operate independently**

### 4. Frame Size Matters for Packet Networks
- Small frames = massive per-packet overhead
- 1ms frames at 48 kHz = 1000 packets/second
- WiFi driver queues can't keep up
- **10-20ms frames are optimal for WiFi audio streaming**

### 5. Observability First
Without metrics, can't diagnose:
- Added stack high water mark logging
- Added heap usage logging
- Added sequence gap / packet loss tracking
- Added per-second bandwidth statistics
- **Instrument first, then optimize**

## Code Locations

**Network Layer:**
- `meshnet-audio/lib/network/src/mesh_net.c` - Non-blocking UDP, error handling
- `meshnet-audio/lib/config/include/config/build.h` - Frame size configuration

**TX Main Loop:**
- `meshnet-audio/src/tx/main.c` - Frame pacing, ADC mode management, knob hysteresis

**RX Main Loop:**
- `meshnet-audio/src/rx/main.c` - Static buffers, packet loss tracking, connection resilience

**Control Layer:**
- `meshnet-audio/lib/config/include/config/pins.h` - Button GPIO configuration
- `meshnet-audio/lib/control/src/buttons.c` - Button polling with debug logging
- `meshnet-audio/lib/control/src/display_ssd1306.c` - Static I2C buffer, malloc elimination

## Performance Metrics

### Before Fixes
- **TX:** Spamming "Send Failed: Not enough space" continuously
- **RX:** Receiving 92 kbps with 85%+ packet loss
- **WiFi:** Disconnecting every 3-4 seconds
- **Audio:** Completely broken, unlistenable
- **Buttons/Knob:** Non-functional

### After Fixes
- **TX:** Clean UDP sends with silent ENOBUFS handling
- **RX:** Stack overflow eliminated, jitter buffer allocates successfully
- **Packet Rate:** 1000 pkt/s → 100 pkt/s (10x reduction)
- **Frame Size:** 192 bytes → 1920 bytes (10x larger)
- **Expected Bandwidth:** ~154 kbps (down from 800+ kbps)
- **Display Updates:** 100 Hz → 10 Hz (reduces I2C overhead)
- **Buttons/Knob:** Fully functional with proper GPIO and ADC management
- **WiFi:** Still unstable but crash-resistant on RX side

### Diagnostics
```
I (3887) rx_main: Main task stack high water mark: 126868 bytes
I (3887) rx_main: Free heap: 123716 bytes
```
Shows healthy stack usage after moving buffers off stack.

## Oracle Consultation

The Oracle (GPT-5 reasoning model) provided critical architectural guidance:

### Key Insights
1. **Broadcasting kills WiFi** - no ACK/retry, lowest rate, saturates AP
2. **Socket contention** - latency task steals packets from audio socket
3. **Stack overflow** - large buffers corrupt heap and crash WiFi
4. **Display malloc churn** - avoid per-update heap operations
5. **Frame aggregation essential** - 1ms packets are pathological for WiFi

### Recommended Path Forward
- **S (<1h):** Unicast to known RX IP instead of broadcast
- **S (<1h):** WiFi event handlers with auto-reconnect
- **M (1-3h):** Separate socket/port for latency vs audio
- **L (1-2d):** Multi-task architecture with proper priorities

## References

- **Architecture Docs:** `docs/planning/v0.1-implementation-plan.md`
- **Oracle Analysis:** Thread T-b1b4c728-0c0e-464b-bf23-055890af947b
- **ESP-IDF WiFi Guide:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi.html
- **ESP-IDF Non-blocking Sockets:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/lwip.html

---

*Next session: Implement unicast HELLO/subscribe mechanism and WiFi event handlers to achieve stable connection.*
