# SoundMesh (MeshNet Audio)

Wireless multi-node audio streaming on XIAO ESP32-S3 using ESP-WIFI-MESH and Opus.

`SRC` (TX/COMBO) captures audio, encodes Opus, and multicasts frames into the mesh.  
`OUT` nodes receive, decode, and play through I2S DAC/headphone output.

## What it does

- Streams mono audio at `48 kHz`, `16-bit`, `20 ms` Opus frames.
- Supports one source and multiple output nodes.
- Uses designated-root mesh startup (`TX/COMBO` is root, `RX` nodes join).
- Exposes node and stream state to OLED UI and serial dashboard.

## Repository layout

- `src/tx/main.c` — TX firmware entrypoint.
- `src/rx/main.c` — RX firmware entrypoint.
- `src/combo/main.c` — COMBO firmware entrypoint (TX + local monitor).
- `lib/audio/` — capture, encode/decode, playback pipeline.
- `lib/network/` — mesh init, transport, heartbeat, node metrics.
- `lib/control/` — display, buttons, portal/state rendering.
- `lib/config/` — shared constants and pin mapping.
- `docs/` — planning, troubleshooting, and progress notes.

## Build and flash

Use PlatformIO environments:

```bash
pio run -e tx
pio run -e rx
pio run -e combo
```

Flash examples:

```bash
pio run -e combo -t upload --upload-port /dev/cu.usbmodem21401
pio run -e rx -t upload --upload-port /dev/cu.usbmodem211101
pio run -e rx -t upload --upload-port /dev/cu.usbmodem211201
pio run -e rx -t upload --upload-port /dev/cu.usbmodem211301
```

## Runtime model

- Audio tasks run on Core 1 (`APP_CPU`) for timing stability.
- Mesh/network tasks run on Core 0 (`PRO_CPU`) with Wi-Fi stack.
- RX playback uses jitter prefill and bounded concealment to smooth burst loss.

## Core configuration

Primary tuning lives in `lib/config/include/config/build.h`:

- Audio format and frame duration.
- Opus bitrate/complexity/FEC.
- Mesh transport (`MESH_FRAMES_PER_PACKET`, queue sizing, channel).
- Jitter/prefill depths and task stack sizes.
- Close-range RF cap (`WIFI_TX_POWER_QDBM`) for stable multi-node association.

## Validation workflow

After code changes, always build all firmware targets:

```bash
pio run -e tx && pio run -e rx && pio run -e combo
```

Run deterministic host-side logic tests:

```bash
pio test -e native
```

Recommended full validation pass:

```bash
pio test -e native && pio run -e tx && pio run -e rx && pio run -e combo
```

For current architecture and operating conventions, see `AGENTS.md`.
