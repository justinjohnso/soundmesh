# MeshNet Audio: Implementation Roadmap (v0.1 - v0.3)

This roadmap tracks the **current implemented state** of the project and the next milestones.
It keeps the v0.1/v0.2/v0.3 framing, separates completed vs in-progress vs planned work, and reflects the code in this repository.

---

## Current Baseline (applies across versions)

### Audio and transport baseline
- Sample rate: **48 kHz**
- Internal PCM format: **16-bit mono** (`lib/config/include/config/build.h`)
- Opus frame duration: **20 ms** (`AUDIO_FRAME_MS=20`)
- Opus bitrate target: **24 kbps** (`OPUS_BITRATE=24000`)
- Mesh packet batching: **2 Opus frames per mesh packet** (`MESH_FRAMES_PER_PACKET=2`)
- TX/COMBO pipeline: capture -> mono -> Opus encode -> `network_send_audio()`
- RX pipeline: mesh receive -> Opus decode -> jitter/prefill -> I2S playback

### Mesh baseline
- Designated-root model is active in `lib/network/src/mesh_net.c`:
  - TX/COMBO sets `esp_mesh_set_type(MESH_ROOT)`
  - all nodes use `esp_mesh_fix_root(true)`
  - RX waits to join the fixed root
- Root fanout uses routing-table aware sends and group delivery flags used by `network_send_audio()`.
- Control and heartbeat traffic use `network_send_control()`.

### Portal baseline
- Portal runs from `lib/control/src/usb_portal_netif.c` + `portal_http.c` + `usb_portal_dns.c`.
- HTTP APIs currently implemented:
  - `GET /api/status`
  - `GET|POST /api/ota`
  - `GET|POST /api/uplink`
- WebSocket endpoint: `GET /ws` (pushes serialized portal state).
- OTA backend uses `esp_https_ota()` (`lib/control/src/portal_ota.c`).
- Uplink config is applied at root using `esp_mesh_set_router()` and synchronized through mesh control messages.

### Build and project path baseline
- Firmware entry points:
  - `src/tx/main.c`
  - `src/rx/main.c`
  - `src/combo/main.c`
- Key modules:
  - `lib/audio/` (pipeline, codec, I2S, ring buffers)
  - `lib/network/` (mesh transport + uplink control)
  - `lib/control/` (display/buttons/portal)
  - `lib/config/` (shared constants)
- Build flow:
  1. Build portal UI (`portal/`) to `portal/dist`.
  2. Prebuild script `tools/pio_prebuild_portal.py` gzip-syncs `portal/dist` assets into `data/`.
  3. Build firmware with PlatformIO (`tx`, `rx`, `combo`).
  4. Upload firmware and SPIFFS image (`uploadfs`) as needed.

---

## Version Overview (current status)

| Version | Focus | Current Status |
|---|---|---|
| **v0.1** | Core mesh audio path | **Implemented** |
| **v0.2** | Compression + observability hardening | **Partially implemented / in progress** |
| **v0.3** | USB portal operations (status/control) | **Partially implemented / in progress** |

---

## v0.1 — Core Mesh + Audio Streaming

### Completed
- Mesh startup and join flow works with designated root behavior.
- Audio streaming over mesh is implemented end-to-end with Opus encode/decode.
- Core FreeRTOS audio tasks and ring buffers run in production paths.
- RX jitter/prefill buffering is implemented in the current pipeline.
- Node role split (TX/COMBO as source class, RX as output class) is reflected in runtime IDs and telemetry.
- OLED and button control layer are integrated and active (`lib/control/src/display_ssd1306.c`, `buttons.c`).

### In progress / partial
- Multi-node reliability and tuning continue (queue sizing, rate control, loss handling).
- Audio quality/performance characterization is still iterative and environment-dependent.

### Planned (remaining under v0.1 scope)
- Continue reliability validation for larger multi-node deployments and varied RF conditions.
- Keep improving packet-loss resilience and underrun behavior under churn.

---

## v0.2 — Compression + Internal Observability

### Completed
- Opus codec integration is active in the running pipeline.
- Telemetry/state serialization exists for portal consumption (`portal_state_serialize_json`).
- Mesh heartbeat ingestion and node cache/state model are implemented in `portal_state.c`.
- FFT-based spectrum bins are computed in the audio layer and exposed for portal views.

### In progress / partial
- Observability is present but still evolving in schema depth and UI presentation.
- Long-session synchronization and drift behavior are handled pragmatically today; additional formalization is still open.
- USB audio input remains stubbed (`lib/audio/src/usb_audio.c`) and is not in active use.

### Planned future work (v0.2 continuation)
- Harden telemetry schema contracts and compatibility guarantees.
- Expand validation for long-session drift/jitter behavior under stress.
- Implement production USB audio input path (currently placeholder).

---

## v0.3 — External Portal + Operations Surface

### Completed
- USB networking portal stack is implemented (TinyUSB net class + esp_netif + DHCP + DNS redirect + HTTP server).
- Static portal assets are served from SPIFFS with gzip support.
- Real-time portal state push exists via `/ws`.
- OTA control API is present and starts HTTPS OTA jobs.
- Uplink control API is present, with root apply and mesh-wide sync propagation.

### In progress / partial
- Portal currently provides operational status/control; broader mixer-centric workflows are not fully realized.
- API surface is focused on status/uplink/OTA and does not yet include a complete multi-stream mixer command plane.
- Cross-platform UX polish and full operational hardening are still ongoing.

### Planned future work (v0.3 continuation)
- Expand portal API/UI around richer stream and routing controls.
- Improve operational safeguards and rollout workflows for OTA.
- Continue portal UX refinement for captive flow and diagnostics.

---

## Explicit Future Milestone (not current baseline)

The following target remains **future work** and is not represented by the current production baseline:

- **24-bit PCM pipeline + 5 ms framing**

Current implemented baseline remains 16-bit PCM with 20 ms Opus frames.

---

## Practical Build / Validation Commands

```bash
# Firmware builds
pio run -e tx
pio run -e rx
pio run -e combo

# Baseline full firmware compile pass
pio run -e tx && pio run -e rx && pio run -e combo

# Host-side tests
pio test -e native

# Portal build + asset sync path
cd portal && npm install && npm run build
# then PlatformIO prebuild syncs portal/dist -> data/*.gz

# Upload filesystem assets (when portal content changes)
pio run -e tx -t uploadfs
pio run -e rx -t uploadfs
pio run -e combo -t uploadfs
```

---

## Notes on scope control

- This document describes what the project **is and does now**, plus explicitly marked next work.
- Timeline estimates and day-by-day durations are intentionally removed because they no longer reflect active execution reality.
