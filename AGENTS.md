# AGENTS.md — SoundMesh (MeshNet Audio)

Wireless audio streaming for XIAO ESP32-S3 using ESP-WIFI-MESH + Opus.
SRC captures/encodes → Mesh broadcast → OUT decodes → I2S DAC.

## Quick Reference
```bash
pio run -e src && pio run -e out  # Build both
pio test -e native                # Run unit tests
bash tools/preupload_gate.sh      # Mandatory pre-flash gate
pio run -e src -t uploadfs        # Upload Portal UI
```

## Architecture: Core Layers
- **Audio (Core 1)**: `adf_pipeline.c` orchestrator. SRC: capture→encode. OUT: decode→playback.
- **Network (Core 0)**: `mesh_net.c`. User Designated Root (SRC root, OUT waits).
- **Control (Core 0)**: Portal UI (Astro), HTTP API, and status polling.

## Hardware & Pin Map (XIAO ESP32-S3)
- **I2C**: SDA=5, SCL=6 (OLED + ES8388)
- **I2S**: MCLK=1, BCLK=7, WS=8, DOUT=9, DIN=2
- **Audio**: ES8388 (SRC/OUT), UDA1334 (OUT-only, needs 5V)
- **Button**: 43 (Internal pull-up)

## Audio Pipeline
- 48kHz / 16-bit Mono / Opus 64kbps / 20ms frames
- **Buffers**: Static bytebufs for PCM, item-mode for Opus.

## Known Issues
- **UDA1334 Power**: Needs 5V VIN for audible output (3.3V is too low).
- **EMI Conflict**: I2S MCLK (GPIO1) can interfere with I2C (GPIO5/6). Write registers BEFORE I2S init.
- **Underruns**: Mitigated by 40-60ms jitter buffer prefill.

## Development Principles
1. **Linear Priority**: Hardening first, then features. See `docs/roadmap/implementation-roadmap.md`.
2. **Batch Commits**: Propose logical batches with direct, lowercase messages. No prefixes.
3. **No Direct `main`**: Use worktrees/branches for features.
4. **Validation**: Must pass `pio test` + `pio run` (both envs) before merge.
5. **Guideline**: Keep `AGENTS.md` ~150 lines (+/- 20).

## Threading Model
- **Core 0**: WiFi/Mesh, `mesh_rx` (prio 6), `mesh_hb` (prio 2), `app_main` (control).
- **Core 1**: `adf_cap` (prio 4), `adf_enc` (prio 3), `adf_dec` (prio 4), `adf_play` (prio 5).
