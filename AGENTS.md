# AGENTS.md — SoundMesh (MeshNet Audio)

Wireless audio streaming system for XIAO ESP32-S3 using ESP-WIFI-MESH + Opus codec.
TX node captures audio → Opus encodes → broadcasts via mesh → RX nodes decode → I2S DAC output.

## Quick Reference

### Build & Flash
```bash
pio run -e tx                # Build TX firmware
pio run -e rx                # Build RX firmware
pio run -e combo             # Build COMBO firmware (TX + headphone monitor)

pio run -e tx -t upload --upload-port /dev/cu.usbmodem101     # Flash TX
pio run -e rx -t upload --upload-port /dev/cu.usbmodem2101    # Flash RX
pio run -e combo -t upload --upload-port /dev/cu.usbmodem101  # Flash COMBO

pio run -e tx -t clean       # Clean build artifacts
pio device monitor -b 115200 # Serial monitor
```

### Verify Builds
After any code change, confirm all three environments compile:
```bash
pio run -e tx && pio run -e rx && pio run -e combo
```

## Project Goal

Transmit audio wirelessly from a TX node to one or more RX nodes over ESP-WIFI-MESH.
The two-node prototype (1 TX/COMBO + 1 RX) is the immediate priority.

## Architecture: Three Code Layers

The firmware is organized into three independent layers that run concurrently
using ESP32-S3 dual-core FreeRTOS:

### 1. Audio Layer (`lib/audio/`)
**Core 1 (APP_CPU) — highest priority**

Handles all audio capture, codec, and playback. Never calls network APIs directly.

| Module | Purpose |
|--------|---------|
| `adf_pipeline.c` | Main pipeline orchestrator. TX: capture→encode→mesh. RX: mesh→decode→playback |
| `es8388_audio.c` | ES8388 codec driver (I2C control + I2S audio). Primary input for TX/COMBO |
| `i2s_audio.c` | UDA1334 I2S DAC output driver (RX without ES8388) |
| `opus_codec.c` | Opus encode/decode wrappers |
| `ring_buffer.c` | FreeRTOS ringbuffer wrapper with event-driven consumer notifications |
| `tone_gen.c` | 440Hz sine wave test tone generator |
| `usb_audio.c` | USB audio input (stub — future feature) |

**FreeRTOS Tasks (all pinned to Core 1):**
- `adf_cap` (prio 4): Reads I2S/ES8388 → writes PCM ring buffer
- `adf_enc` (prio 3): Reads PCM buffer → Opus encode → `network_send_audio()`
- `adf_dec` (prio 4): Reads Opus ring buffer → Opus decode → writes PCM buffer
- `adf_play` (prio 5): Reads PCM buffer → I2S write (highest audio priority)

### 2. Network Layer (`lib/network/`)
**Core 0 (PRO_CPU) — ESP-WIFI-MESH runs here**

Handles mesh formation, root election, packet routing, and audio transport.

| Module | Purpose |
|--------|---------|
| `mesh_net.c` | ESP-WIFI-MESH init, event handling, audio send/receive, ping/pong latency |

**FreeRTOS Tasks:**
- `mesh_rx` (prio 6): Blocking `esp_mesh_recv()` loop — parses audio/heartbeat/ping packets
- `mesh_hb` (prio 2): Sends heartbeats every 2s + stream announcements

**Key Design:**
- **User Designated Root** (ESP-IDF official pattern): TX/COMBO call `esp_mesh_set_type(MESH_ROOT)` +
  `esp_mesh_fix_root(true)` before start → immediately become root, no election/scanning delay.
  RX calls `esp_mesh_fix_root(true)` only → waits indefinitely to join the designated root.
- **No fallback timer** — the old 10s timer that caused race conditions has been removed.
- **Root broadcasts explicitly**: root iterates routing table and sends to each descendant
  using `MESH_DATA_P2P` flag (standalone mesh has no DS; FROMDS caused stalls).
- **Startup gating**: `mesh_rx` task and `mesh_hb` task are created before `esp_mesh_start()`
  and wait for readiness notification before operating.
- Adaptive rate limiting on `network_send_audio()` with gradual backoff/recovery (levels 0-2)
- Deduplication cache prevents broadcast loops in multi-hop topologies

### 3. Control Layer (`lib/control/`)
**Runs in main task (app_main loop)**

Handles UI, buttons, status display. Polls at 10Hz for display, 200Hz for buttons.

| Module | Purpose |
|--------|---------|
| `display_ssd1306.c` | SSD1306 OLED (128×32) rendering for TX/RX/COMBO status views |
| `buttons.c` | GPIO button polling with short/long press detection |

### Config (`lib/config/`)
| Header | Purpose |
|--------|---------|
| `build.h` | **All** audio/codec/buffer/task/network constants. Single source of truth |
| `pins.h` | GPIO assignments for I2C, I2S, ES8388, UDA1334, button |

## Audio Format

| Parameter | Value |
|-----------|-------|
| Sample Rate | 48,000 Hz |
| Bit Depth | 16-bit PCM (internal pipeline) |
| Channels | Mono (stereo I2S on hardware) |
| Codec | Opus, 64 kbps VBR |
| Frame Duration | 20ms (960 samples per frame) |
| Packet Size | ~14 byte header + ~160 byte Opus payload |

## Hardware

- **Board**: Seeed XIAO ESP32-S3
- **Audio Input (TX/COMBO)**: PCBArtists ES8388 module (I2C addr 0x10, LIN2/RIN2 line in)
- **Audio Output (RX)**: UDA1334 I2S DAC **or** ES8388 headphone out
- **Display**: SSD1306 OLED 128×32 (I2C addr 0x3C)
- **Button**: GPIO43 with internal pull-up

### Pin Map
```
I2C:  SDA=GPIO5, SCL=GPIO6 (shared: OLED + ES8388)
I2S:  MCLK=GPIO1, BCLK=GPIO7, WS=GPIO8, DOUT=GPIO9, DIN=GPIO2
Button: GPIO43
```

## Build System

- **Framework**: ESP-IDF via PlatformIO (espressif32@~6.6.0)
- **Build environments**: `tx`, `rx`, `combo` (selected via `platformio.ini`)
- **Conditional compilation**: `CONFIG_TX_BUILD`, `CONFIG_RX_BUILD`, `CONFIG_COMBO_BUILD`
- **ES8388 toggle**: `CONFIG_USE_ES8388` (enabled for TX and COMBO; RX uses UDA1334 I2S DAC)
- **Extra script**: `extra_script.py` generates `src/CMakeLists.txt` per environment
- **Opus**: `78/esp-opus` component (via `idf_component.yml`)
- **Partitions**: Custom `partitions.csv` with ~2MB app partition

### SDKconfig Hierarchy
```
sdkconfig.shared.defaults  → Common settings (WiFi, mesh, FreeRTOS, I2C)
sdkconfig.tx.defaults      → TX-specific (TinyUSB, USB OTG)
sdkconfig.rx.defaults      → RX-specific (I2S TX channel)
sdkconfig.combo.defaults   → COMBO (TX + RX settings combined)
```

## Code Conventions

- **Language**: C (ESP-IDF style)
- **Naming**: `camelCase` for variables/functions, `UPPER_CASE` for constants/macros, `PascalCase` for types
- **Includes**: Use `"module/header.h"` style for project headers
- **Memory**: Prefer static/global buffers over stack allocation for audio data (ESP32 stack is limited)
- **Error handling**: Use `ESP_ERROR_CHECK()` for fatal init errors, return `esp_err_t` for recoverable
- **Logging**: Use `ESP_LOGI/W/E` with module-specific TAG strings
- **Task creation**: Always use `xTaskCreatePinnedToCore()` for audio tasks (Core 1)
- **Constants**: ALL tunable values go in `lib/config/include/config/build.h` — never duplicate
- **Ring buffers**: BYTEBUF mode for PCM streams, NOSPLIT/item mode for Opus frames
- **Event-driven**: Tasks block on `ulTaskNotifyTake()` — no busy-wait polling loops

## Known Issues & Bugs (as of Feb 2025)

### Fixed (Feb 2025)

1. ~~**Mesh Discovery Deadlock**~~: **FIXED** — Implemented User Designated Root pattern.
   TX/COMBO are forced to `MESH_ROOT` before start; RX waits indefinitely. Removed 10s
   fallback timer that caused race conditions. Boot order no longer matters.

4. ~~**Watchdog Triggers During Mesh Scan**~~: **FIXED** — All nodes (TX, RX, COMBO) now use
   chunked `ulTaskNotifyTake()` with 1s timeout + `esp_task_wdt_reset()` during network wait.

5. ~~**Flash Size Warning**~~: **FIXED** — Deleted stale `sdkconfig.tx/rx/combo` cache files.
   `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` in `sdkconfig.defaults` now takes effect correctly.

### Remaining — Critical

2. **RX Audio Output — UDA1334 Power**: UDA1334 DAC needs 5V power for audible output.
   At 3.3V, output is ~900mV RMS (barely audible). Confirmed in multiple threads.
   - *Fix*: Wire UDA1334 VIN to 5V supply, not 3.3V.

3. **I2C/MCLK EMI Conflict**: ES8388 I2C control (GPIO5/6) conflicts with MCLK output (GPIO1).
   Once I2S starts driving MCLK, I2C writes may fail due to electromagnetic interference.
   - *Mitigation*: All codec register writes happen BEFORE I2S init. Post-init I2C
     (volume changes) may silently fail.

### Non-Critical

6. **Opus Decode Errors**: Occasional `Opus decode failed` warnings when RX receives
   corrupted or partial packets. The pipeline handles this gracefully (skips frame).

7. **Buffer Underruns**: RX playback task tracks underruns. These occur during mesh
   reconnection or when TX rate-limits frames due to queue pressure. Jitter buffer
   with 40ms prefill (2×20ms frames) mitigates this.

8. **USB Audio**: Stubbed out — `usb_audio_init()` is a no-op. TinyUSB UAC integration
   is a future feature.

## Debugging Tips

### Serial Monitor Filters
```bash
# Audio pipeline only
pio device monitor -b 115200 | grep -E "adf_pipeline|Playback|decode|encode|Capture"

# Network only
pio device monitor -b 115200 | grep -E "network_mesh|Mesh|mesh_rx|Parent|Root|Child"

# Errors and warnings only
pio device monitor -b 115200 | grep -E "^E |^W "
```

### Test Modes
- `RX_TEST_TONE_MODE` in `adf_pipeline.c` line 719: Set to 1 to bypass entire RX
  pipeline and output 440Hz tone directly to I2S. Tests hardware independently.
- `TX_TEST_TONE_MODE` in `adf_pipeline.c` line 395: Set to 1 to bypass ES8388
  capture and send pure tone through encoder. Tests Opus + network path.

### Memory Monitoring
```c
ESP_LOGI(TAG, "Free heap: %u", heap_caps_get_free_size(MALLOC_CAP_8BIT));
ESP_LOGI(TAG, "Stack HWM: %u", uxTaskGetStackHighWaterMark(NULL));
```

## Threading Model (ESP32-S3 Dual Core)

```
Core 0 (PRO_CPU):                    Core 1 (APP_CPU):
├── WiFi/LWIP (system)               ├── adf_cap  (capture, prio 4)
├── mesh_rx   (receive, prio 6)      ├── adf_enc  (encode,  prio 3)
├── mesh_hb   (heartbeat, prio 2)    ├── adf_dec  (decode,  prio 4)
└── app_main  (control loop)         └── adf_play (playback, prio 5)
```

Audio tasks run on Core 1 to avoid contention with WiFi/mesh stack on Core 0.
The main task (control/UI) runs on Core 0 alongside networking.

## Data Flow

### TX Path
```
ES8388 ADC → I2S RX → [capture task] → PCM ring buffer → [encode task] → Opus encode
→ net_frame_header + opus_payload → network_send_audio() → esp_mesh_send()
```

### RX Path
```
esp_mesh_recv() → [mesh_rx task] → parse header → audio_rx_callback()
→ adf_pipeline_feed_opus() → Opus ring buffer → [decode task] → Opus decode
→ PCM ring buffer → [playback task] → I2S write → UDA1334/ES8388 DAC
```

## File Map

```
soundmesh/
├── src/
│   ├── tx/main.c              # TX entry point (ES8388 input → mesh broadcast)
│   ├── rx/main.c              # RX entry point (mesh receive → I2S output)
│   └── combo/main.c           # COMBO entry point (TX + headphone monitor)
├── lib/
│   ├── audio/
│   │   ├── include/audio/     # Headers: adf_pipeline.h, es8388_audio.h, etc.
│   │   └── src/               # Implementations
│   ├── config/
│   │   └── include/config/    # build.h (all constants), pins.h (GPIO map)
│   ├── control/
│   │   ├── include/control/   # display.h, buttons.h, status.h
│   │   └── src/               # display_ssd1306.c, buttons.c
│   └── network/
│       ├── include/network/   # mesh_net.h (API + packet types)
│       └── src/               # mesh_net.c
├── components/
│   └── esp-opus/              # Opus codec component (git submodule)
├── docs/
│   ├── planning/              # Architecture docs (audio, network, control, roadmap)
│   ├── posts/                 # Development blog posts
│   └── progress/              # Fix documentation
├── platformio.ini             # Build environments (tx, rx, combo)
├── partitions.csv             # Flash partition table
├── sdkconfig.shared.defaults  # Common ESP-IDF config
├── sdkconfig.tx.defaults      # TX-specific config
├── sdkconfig.rx.defaults      # RX-specific config
├── sdkconfig.combo.defaults   # COMBO-specific config
├── extra_script.py            # PlatformIO pre-build script
└── idf_component.yml          # ESP component dependencies (tinyusb, esp-opus)
```

## Development Principles

1. **Use working examples** as the basis for new code rather than writing from scratch.
   ESP-IDF examples, ESP-ADF examples, and the thaaraak/ESP32-ES8388 repo are key references.
2. **All configuration in `build.h`** — never hardcode audio/buffer/network constants elsewhere.
3. **Event-driven, not polling** — tasks block on notifications, ring buffers notify consumers.
4. **Static buffers for audio** — declared at file scope, not on task stacks.
5. **Three-layer separation** — audio never calls network; network pushes data via callbacks.
6. **Test with tone first** — use `RX_TEST_TONE_MODE` / `TX_TEST_TONE_MODE` to isolate issues.
7. **Always verify all three environments build** after any change.
