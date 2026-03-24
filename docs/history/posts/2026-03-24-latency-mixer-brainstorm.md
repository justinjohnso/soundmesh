# Latency, Mixers, and What's Actually Achievable

*March 24, 2026*

---

## The Latency Question

The question came up: can we get SoundMesh latency down into the 5–20ms range? Dante does sub-millisecond, why can't we at least get to "noticeable" territory?

Short answer: we can get to **30–70ms** with deliberate tuning. We cannot get to 5–20ms on WiFi mesh, and here's exactly why.

### Why the current number is ~340ms

The full latency budget in the current configuration:

| Stage | Time | Notes |
|---|---|---|
| I2S DMA capture chunk | 10ms | DMA fires when chunk fills |
| Opus frame accumulation | 20ms | Must collect full 20ms frame before encoding |
| Opus encode | ~3–5ms | COMPLEXITY=2, fast |
| TX frame batching | +20ms | `MESH_FRAMES_PER_PACKET=2` — waits for second frame |
| ESP-WIFI-MESH OTA | 3–15ms | CSMA/CA contention, no determinism |
| Mesh protocol overhead | 3–10ms | ACK, association, routing |
| RX decode task scheduling | 1–5ms | FreeRTOS 1ms tick jitter |
| Opus decode | ~1–2ms | |
| **RX jitter buffer prefill** | **280ms** | `JITTER_PREFILL_FRAMES=14` — the dominant term |
| I2S playback DMA | 10ms | |
| **Total** | **~340–370ms** | |

The 280ms jitter prefill is doing 80% of the work. It exists because any single missed or late mesh packet causes an audible dropout without it. The stability is purchased with latency.

### What WiFi mesh physically cannot do

- **WiFi is CSMA/CA**, not TDMA. Every packet contends for the medium. Jitter of ±5–15ms per hop is normal and structurally irreducible.
- **ESP-WIFI-MESH adds routing overhead** per hop: ~3–8ms per hop. With `MESH_MAX_LAYER=2` we're capped at 2 hops.
- **Opus minimum frame is 2.5ms** — achievable but means 400 encode/decode calls/second. CPU budget gets tight at COMPLEXITY=2 on one S3 core.
- **FreeRTOS tick = 1ms**. Task notification latency adds 0–1ms at every pipeline stage crossing.

**The physical floor for this hardware/protocol stack: ~25–45ms**, and only achievable with aggressive settings and accepting dropout risk on noisy WiFi channels.

### Why Dante's comparison doesn't hold

Dante uses wired Gigabit Ethernet with PTPv2 hardware timestamping (nanosecond accuracy), deterministic switched networks with AVB/TSN bandwidth reservation, and typically a dedicated DSP processor. It's a fundamentally different medium and hardware class. A better comparison is **Bluetooth LE Audio** (LC3 codec, ISO channels, ~20–40ms) or commercial single-hop WiFi audio products like KLEER (~30ms) — both of which are single-hop, proprietary, no mesh routing.

### The realistic tuning target: 50–80ms

Getting there requires:

1. **Drop `JITTER_PREFILL_FRAMES` to 2–4** (saves 200–240ms). Accept dropout risk.
2. **Drop `AUDIO_FRAME_MS` from 20ms to 10ms** (saves 10ms capture + 10ms encoding wait). Doubles packet rate.
3. **Drop `MESH_FRAMES_PER_PACKET` to 1** (saves 20ms TX batching). Doubles mesh bandwidth usage.
4. **Drop `I2S_DMA_DESC_NUM` from 8 to 3–4** (saves ~30ms DMA buffer).
5. **Confirm `WIFI_PS_NONE`** in sdkconfig (already the case for mesh mode).

With all of these: **40–70ms best-case**, with ~20–40ms jitter events during WiFi congestion.

The key insight is that these should be **user-controlled settings**, not hardcoded constants. A "Latency Target" control in the portal lets users pick their tradeoff based on their RF environment. This feeds directly into the mixer design below.

---

## The Mixer Design

### Why a mixer?

The current portal has audio analysis (FFT) and node telemetry, but no way to control audio levels. For a distributed audio system this is a foundational missing capability. Every pro audio system — Dante, AVB, AES67 — has per-node gain control.

The design distinguishes two roles:

- **SRC/TX/COMBO nodes** have a **Global Mixer**: they control the *source* signal going into the mesh. They own the input trim, the master broadcast level, and can influence system-wide behavior.
- **OUT/RX nodes** have a **Local Mixer**: they control what comes out of their own speaker/DAC. Independent per-node volume.

This maps cleanly to physical reality: the SRC is the "console" (gain structure, broadcast level), and each OUT node is a "powered monitor" with its own local volume.

### Global Mixer (SRC/TX/COMBO)

Controls:
- **Input trim**: –18 to +18 dB applied before Opus encode. Used to compensate for line-level variation from the source device.
- **Input mute**: silences captured audio before encoding (sends silence across mesh).
- **Master output level**: –60 to +12 dB applied before I2S write (for COMBO local monitoring).
- **Input source**: Line In / Test Tone. Test tone is 440Hz for commissioning without a source device.
- **Latency target**: Stable (280ms) / Balanced (80ms) / Low Latency (40ms). Changes jitter buffer prefill depth.

### Local Mixer (OUT/RX)

Controls:
- **Output volume**: –60 to +12 dB applied after Opus decode, before I2S write. Maps to a linear scale applied sample-by-sample.
- **Output mute**: zeroes the PCM frame before I2S write.
- **Latency target**: same three-tier selector, changes how long the RX node waits before it begins playback.

### Industry-standard config options added

Beyond the basic mixer:
- **Latency target** maps to jitter buffer depth (the single biggest UX knob from the latency analysis)
- **Input source** (line in vs test tone) is standard for commissioning
- dB-calibrated gain display (not raw 0–100 percentages)
- Mute is per-stage (input mute vs output mute), consistent with pro audio console conventions

### What's intentionally out of scope

- **EQ**: explicitly excluded per design intent (per-band EQ requires DSP budget that is better spent on lower latency or better codec quality)
- **Per-remote-node volume from SRC**: this would require mesh control messages to each RX node, and each RX node would need to honor them — a larger protocol extension, not part of this iteration
- **NVS persistence**: mixer state is in-memory and resets on reboot. Adding NVS persistence is a natural next step but deferred

---

## Implementation Summary

### Firmware changes

**Audio pipeline gain** (`adf_pipeline_state.h`, `adf_pipeline_core.c`, `adf_pipeline_rx.c`, `adf_pipeline_tx.c`):
- Add `volatile float output_gain_linear` and `volatile bool output_mute` to the pipeline struct
- Add `volatile float input_gain_linear` and `volatile bool input_mute`
- Apply gain in `rx_playback_task` (replaces hardcoded `RX_OUTPUT_VOLUME = 2.0f`)
- Apply gain/mute in `tx_capture_task` before writing to PCM ring buffer
- Expose setters/getters in `adf_pipeline.h`

**Jitter buffer control** (`mesh_queries.c`, `mesh_net.h`):
- Add `network_set_jitter_override(int frames)` (–1 = auto, 1–16 = fixed depth)
- `network_get_jitter_prefill_frames()` checks override first, then falls back to adaptive logic

**Portal API** (`portal_http.c`, `portal_state.c`):
- New `GET/POST /api/mixer` endpoint
- Mixer state included in `/api/status` JSON for UI initialization
- Registers alongside existing `/api/uplink` and `/api/ota` routes

### Frontend changes

**New `MixerPane.astro`**:
- Replaces the AudioAnalysis pane in the left column of the 3-pane grid
- Role-adaptive: SRC shows global controls, OUT shows local controls
- FFT visualization folded in below mixer controls as signal meter

**Updated `app.js`**:
- `mixerState` object synchronized with `/api/mixer` on connect and after every change
- Debounced PATCH-style POST (only changed fields)
- Slider `input` events update the dB readout in real-time before API call
- Demo mode includes mock mixer state

**Updated `app.css`**:
- Range input styling (horizontal faders, green thumb matching existing palette)
- Segmented button group for latency mode
- Mute button with active/inactive color states
