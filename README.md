# SoundMesh (MeshNet Audio)

Wireless multi-node audio streaming on XIAO ESP32-S3 using ESP-WIFI-MESH and Opus.

`SRC` nodes capture audio, encode Opus, and broadcast into the mesh.  
`OUT` nodes receive, decode, and play through I2S DAC/headphone output.

## What it does

- Streams mono audio at `48 kHz`, `16-bit`, `20 ms` Opus frames.
- Supports one source and multiple output nodes.
- Uses designated-root mesh startup (`SRC` is root, `OUT` joins fixed root).
- Exposes node and stream state to OLED UI, serial dashboard, and portal API.

## Repository layout

- `src/src/main.c` — SRC firmware entrypoint.
- `src/out/main.c` — OUT firmware entrypoint.
- `lib/audio/` — capture, encode/decode, playback pipeline.
- `lib/network/` — mesh init, transport, heartbeat, node metrics.
- `lib/control/` — display, buttons, portal/state rendering.
- `lib/control/portal-ui/` — portal UI source and build scripts.
- `lib/config/` — shared constants and pin mapping.
- `docs/` — roadmap, operations, architecture, and history.

## Build and flash

Use PlatformIO environments:

```bash
pio run -e src
pio run -e out
```

Flash examples:

```bash
pio run -e src -t upload --upload-port /dev/cu.usbmodem101
pio run -e out -t upload --upload-port /dev/cu.usbmodem2101
```

Upload portal web assets (SPIFFS) after portal UI changes:

```bash
pio run -e src -t uploadfs
pio run -e out -t uploadfs
```

## Runtime model

- Audio tasks run on Core 1 (`APP_CPU`) for timing stability.
- Mesh/network tasks run on Core 0 (`PRO_CPU`) with Wi-Fi stack.
- OUT playback uses jitter prefill and bounded concealment to smooth burst loss.
- Pilot control-plane policy is root-managed: SRC handles uplink/OTA orchestration.
- Portal demo mode is explicit only (`?demo=1`), with no silent fallback.

## Core configuration

Primary tuning lives in `lib/config/include/config/build.h`:

- Audio format, frame duration, and Opus settings.
- Mesh transport and queue sizing.
- Jitter/prefill depths and task stack sizes.
- Portal auth/feature flags and heap guard rails.

## Validation workflow

Run tests and both firmware builds after code changes:

```bash
pio test -e native
pio run -e src
pio run -e out
```

Recommended full check:

```bash
pio test -e native && pio run -e src && pio run -e out
```

Required pre-upload gate:

```bash
bash tools/preupload_gate.sh
```

Do not flash hardware unless this gate passes.

Gate highlights:
- validates `src/out` build artifacts and emits `.pio/build/preupload_gate_metrics.tsv`
- enforces role-specific RAM ceilings (SRC 70%, OUT 65%)
- enforces stack/heap budget floors from `build.h`
- validates runtime safety markers from generated sdkconfig headers
- preserves crash-signature checks
- fail-closed on portal flags: if enabled, requires approved runtime evidence in
  `docs/operations/runtime-evidence/portal-enable-evidence.env`

## Control-plane auth

Protected control endpoints require token auth:

- `POST /api/ota`
- `POST /api/uplink`
- `POST /api/mixer`
- control-capable `/ws` sessions

Provide either:
- `Authorization: Bearer <token>`
- `X-SoundMesh-Token: <token>`

Default token is configured in `build.h` as `PORTAL_CONTROL_AUTH_TOKEN`.

## OTA workflow

Portal OTA endpoint:

```json
{"url":"https://your-host/path/firmware.bin"}
```

Notes:
- OTA requires HTTPS URL and trusted certificate chain.
- In portal UI, use `Ctrl/⌘ + Shift + U` to open OTA prompt.
- Keep at least one known-good node/firmware pair before broad rollout.

## Uplink workflow

Portal uplink endpoint:

```json
{"enabled":true,"ssid":"YourNetwork","password":"YourPassword"}
```

Disable/clear uplink:

```json
{"enabled":false}
```

Notes:
- SRC applies router config and syncs descendants.
- OUT nodes request sync after parent connect and surface status in portal.
- Responses redact sensitive fields by design.

## Mixer workflow

Portal mixer endpoint:

```json
{"outGainPct":200}
```

Notes:
- Valid range is `0..400` percent.
- `GET /api/mixer` returns current applied/pending/error status.
- Mixer state is also exposed via `/api/status` and `/ws` as `mixer`.

For operator procedures, see `docs/operations/`.
