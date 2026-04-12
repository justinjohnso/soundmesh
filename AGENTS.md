# AGENTS.md — SoundMesh (MeshNet Audio)

Wireless audio streaming system for XIAO ESP32-S3 using ESP-WIFI-MESH + Opus codec.
SRC node captures audio → Opus encodes → broadcasts via mesh → OUT nodes decode → I2S DAC output.

## Quick Reference

### Build & Flash
```bash
pio run -e src               # Build SRC firmware
pio run -e out               # Build OUT firmware

pio run -e src -t upload --upload-port /dev/cu.usbmodem101    # Flash SRC
pio run -e out -t upload --upload-port /dev/cu.usbmodem2101   # Flash OUT

pio run -e src -t clean      # Clean build artifacts
pio run -e out -t clean
pio device monitor -b 115200 # Serial monitor
```

### Portal Assets (SPIFFS)
```bash
pio run -e src -t uploadfs
pio run -e out -t uploadfs
```

### Portal UI (local)
```bash
cd lib/control/portal-ui
pnpm install
pnpm run dev --demo       # Dev server with mock data
pnpm run build            # Build static assets
pnpm run export:spiffs    # Copy dist/ to data/ for SPIFFS upload
```

### Verify Builds
After any code change, confirm both active environments compile:
```bash
pio run -e src && pio run -e out
```

### Run Tests
Run native Unity unit tests. PlatformIO auto-discovers every `test/native/test_*` suite:
```bash
pio test -e native
```

Current test suites (12): `test_frame_codec`, `test_sequence_tracker`, `test_mesh_dedupe`,
`test_mesh_queries`, `test_mesh_uplink_runtime`, `test_mixer_control`, `test_portal_api_contract`,
`test_portal_json_extract`, `test_control_metrics_serialization`, `test_rx_underrun_concealment`,
`test_uplink_control`.

Recommended full validation:
```bash
pio test -e native && pio run -e src && pio run -e out
```

### Portal + OTA Notes
- USB portal rollout is phased: SRC enabled first, OUT disabled until dedicated HIL validation passes.
- Portal monitor stream is available in UI under "Monitor Output".
- OTA endpoint: `POST /api/ota` with `{"url":"https://.../firmware.bin"}`.
- OTA status endpoint: `GET /api/ota`.
- Use `Ctrl/⌘ + Shift + U` in the portal UI to trigger OTA prompt.
- Uplink endpoint: `POST /api/uplink` with `{"enabled":true,"ssid":"...","password":"..."}`.
- Uplink status endpoint: `GET /api/uplink` (also included in `/api/status` as `uplink`).

## Project Goal

Transmit audio wirelessly from a SRC node to one or more OUT nodes over ESP-WIFI-MESH.
The two-node prototype (1 SRC + 1 OUT) is the immediate priority.

## Architecture: Three Code Layers

The firmware is organized into three independent layers that run concurrently
using ESP32-S3 dual-core FreeRTOS:

### 1. Audio Layer (`lib/audio/`)
**Core 1 (APP_CPU) — highest priority**

Handles all audio capture, codec, and playback. Never calls network APIs directly.

| Module | Purpose |
|--------|---------|
| `adf_pipeline.c` | Main pipeline orchestrator. SRC: capture→encode→mesh. OUT: mesh→decode→playback |
| `es8388_audio.c` | ES8388 codec driver (I2C control + I2S audio). Primary input for SRC |
| `i2s_audio.c` | UDA1334 I2S DAC output driver (OUT without ES8388) |
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
- **User Designated Root** (ESP-IDF official pattern): SRC calls `esp_mesh_set_type(MESH_ROOT)` +
  `esp_mesh_fix_root(true)` before start → immediately become root, no election/scanning delay.
  OUT calls `esp_mesh_fix_root(true)` only → waits indefinitely to join the designated root.
- **No fallback timer** — the old 10s timer that caused race conditions has been removed.
- **Root broadcasts explicitly**: root iterates routing table and sends to each descendant
  using `MESH_DATA_P2P` flag (standalone mesh has no DS; FROMDS caused stalls).
- **Startup gating**: `mesh_rx` task and `mesh_hb` task are created before `esp_mesh_start()`
  and wait for readiness notification before operating.
- **Root-managed uplink**: root can apply upstream Wi-Fi credentials via portal/API. Root updates router config with
  `esp_mesh_set_router()` and broadcasts sync; non-root nodes request sync after parent connect.
- Adaptive rate limiting on `network_send_audio()` with gradual backoff/recovery (levels 0-2)
- Deduplication cache prevents broadcast loops in multi-hop topologies

### 3. Control Layer (`lib/control/`)
**Runs in main task (app_main loop)**

Handles UI, buttons, status display. Polls at 10Hz for display, 200Hz for buttons.

| Module | Purpose |
|--------|---------|
| `display_ssd1306.c` | SSD1306 OLED (128×32) rendering for SRC/OUT status views |
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
- **Audio Input (SRC)**: PCBArtists ES8388 module (I2C addr 0x10, LIN2/RIN2 line in)
- **Audio Output (OUT)**: UDA1334 I2S DAC **or** ES8388 headphone out
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
- **Build environments**: `src`, `out` (selected via `platformio.ini`)
- **Conditional compilation**: `CONFIG_SRC_BUILD`, `CONFIG_OUT_BUILD`
- **ES8388 toggle**: `CONFIG_USE_ES8388` (enabled on SRC by default)
- **Extra script**: `extra_script.py` generates `src/CMakeLists.txt` per environment
- **Opus**: `78/esp-opus` component (via `idf_component.yml`)
- **Partitions**: Custom `partitions.csv` with ~2MB app partition

### SDKconfig Hierarchy
```
sdkconfig.shared.defaults  → Common settings (WiFi, mesh, FreeRTOS, I2C)
sdkconfig.src.defaults     → SRC-specific (TinyUSB, USB OTG)
sdkconfig.out.defaults     → OUT-specific (I2S TX channel)
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

## Known Issues & Bugs

### Fixed (Feb 2025)

1. ~~**Mesh Discovery Deadlock**~~: **FIXED** — Implemented User Designated Root pattern.
   SRC is forced to `MESH_ROOT` before start; OUT waits indefinitely. Removed 10s
   fallback timer that caused race conditions. Boot order no longer matters.

4. ~~**Watchdog Triggers During Mesh Scan**~~: **FIXED** — All nodes (SRC, OUT) now use
   chunked `ulTaskNotifyTake()` with 1s timeout + `esp_task_wdt_reset()` during network wait.

5. ~~**Flash Size Warning**~~: **FIXED** — Deleted stale sdkconfig cache files.
   `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` in `sdkconfig.defaults` now takes effect correctly.

### Remaining — Critical

2. **OUT Audio Output — UDA1334 Power**: UDA1334 DAC needs 5V power for audible output.
   At 3.3V, output is ~900mV RMS (barely audible). Confirmed in multiple threads.
   - *Fix*: Wire UDA1334 VIN to 5V supply, not 3.3V.

3. **I2C/MCLK EMI Conflict**: ES8388 I2C control (GPIO5/6) conflicts with MCLK output (GPIO1).
   Once I2S starts driving MCLK, I2C writes may fail due to electromagnetic interference.
   - *Mitigation*: All codec register writes happen BEFORE I2S init. Post-init I2C
     (volume changes) may silently fail.

### Non-Critical

6. **Opus Decode Errors**: Occasional `Opus decode failed` warnings when OUT receives
   corrupted or partial packets. The pipeline handles this gracefully (skips frame).

7. **Buffer Underruns**: OUT playback task tracks underruns. These occur during mesh
   reconnection or when SRC rate-limits frames due to queue pressure. Jitter buffer
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
- `RX_TEST_TONE_MODE` in `adf_pipeline.c` line 719: Set to 1 to bypass entire OUT
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

### SRC Path
```
ES8388 ADC → I2S RX → [capture task] → PCM ring buffer → [encode task] → Opus encode
→ net_frame_header + opus_payload → network_send_audio() → esp_mesh_send()
```

### OUT Path
```
esp_mesh_recv() → [mesh_rx task] → parse header → audio_rx_callback()
→ adf_pipeline_feed_opus() → Opus ring buffer → [decode task] → Opus decode
→ PCM ring buffer → [playback task] → I2S write → UDA1334/ES8388 DAC
```

## File Map

```
soundmesh/
├── src/
│   ├── src/main.c             # SRC entry point (ES8388 input → mesh broadcast)
│   └── out/main.c             # OUT entry point (mesh receive → I2S output)
├── lib/
│   ├── audio/
│   │   ├── include/audio/     # Headers: adf_pipeline.h, es8388_audio.h, etc.
│   │   └── src/               # Implementations
│   ├── config/
│   │   └── include/config/    # build.h (all constants), pins.h (GPIO map)
│   ├── control/
│   │   ├── include/control/   # display.h, buttons.h, status.h
│   │   ├── src/               # display_ssd1306.c, buttons.c, portal_http.c, portal_state.c
│   │   └── portal-ui/         # Astro portal UI source (builds to dist/, exported to data/)
│   └── network/
│       ├── include/network/   # mesh_net.h (API + packet types)
│       └── src/               # mesh_net.c
├── components/
│   └── esp-opus/              # Opus codec component (git submodule)
├── docs/
│   ├── roadmap/               # Canonical execution roadmap
│   ├── architecture/          # Active architecture references
│   ├── audits/                # Time-bounded audit packets
│   ├── operations/            # Runbooks/checklists
│   ├── quality/               # Testing/SLO/security quality docs
│   └── history/               # Archived posts/progress/superseded plans
├── platformio.ini             # Build environments (src, out)
├── partitions.csv             # Flash partition table
├── sdkconfig.shared.defaults  # Common ESP-IDF config
├── sdkconfig.src.defaults     # SRC-specific config
├── sdkconfig.out.defaults     # OUT-specific config
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
7. **Always verify both active environments build** after any change.
8. **Never add `Co-authored-by` trailers** to git commit messages.
9. **No hardware upload before gate pass:** run `bash tools/preupload_gate.sh` and block uploads on any failure.

## Roadmap & Docs Governance (Mandatory)

1. **Single canonical roadmap:** `docs/roadmap/implementation-roadmap.md` is the only source of truth for "what's next".
   - `docs/history/superseded-plans/professionalization-roadmap.md` is supporting reference, not execution authority.
2. **Linear priority policy:** execute Stage 0 pilot hardening items before feature expansion.
3. **Docs relevance policy:** new docs must be clearly classified as one of:
   - canonical active
   - supporting reference
   - historical archive
   - superseded
4. **Superseded handling:** if a doc is replaced, mark it as superseded at the top and point to the canonical replacement.
5. **Anti-clutter rule:** avoid creating one-off planning docs when updating an existing canonical doc is sufficient.
6. **Restructure direction:** keep active docs easy to scan; aggressively archive non-canonical materials during docs cleanup passes.

## Git Workflow & Branch Hygiene (Mandatory)

1. **No direct feature work on `main`:** start every implementation in a dedicated branch.
2. **Use worktrees for active efforts:** each significant task gets its own `git worktree` + branch pair.
3. **Fleet/parallel rule:** if running multiple workflows in parallel ("fleet"), each workflow must run in a separate worktree/branch.
4. **Commit as you go:** create small, logical commits at each stable milestone (working build/tests for that slice), not one large end-of-session commit.
5. **Keep working trees clean:** before context-switching, either commit, or explicitly stash with a clear label.
6. **Merge readiness gate:** merge to `main` only after relevant checks pass (`pio test -e native` and `pio run -e src && pio run -e out` unless docs-only change).
7. **PR-first integration:** prefer merge via PR (even solo) to preserve review history and rollback clarity.

## Context Management Note

This AGENTS.md is comprehensive (~360 lines) because it serves as the single architecture reference.
Path-scoped rules in `.claude/rules/` handle file-type-specific conventions (C/ESP-IDF patterns,
portal-ui patterns) and only load when working on matching files, keeping context lean for focused tasks.
