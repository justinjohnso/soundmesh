# ADF Pipeline Split Plan (`adf-refactor-split-plan`)

> Status: superseded — split has been implemented; see `docs/architecture/audio.md` for active architecture reference and `docs/roadmap/implementation-roadmap.md` for current execution priorities.

**Date:** 2026-03-23  
**Status:** Planned (design-only, no behavior changes)  
**Scope:** Split `lib/audio/src/adf_pipeline.c` into focused modules while preserving runtime behavior and current public API.

## Goals

- Reduce `adf_pipeline.c` size and cognitive load.
- Preserve all current runtime semantics (task creation order, event-driven ring-buffer signaling, Opus/FEC/PLC behavior, jitter prefill behavior, and local monitor behavior).
- Keep `lib/audio/include/audio/adf_pipeline.h` public API behavior compatible.
- Make future feature work (USB input completion, FFT tuning, RX concealment improvements) safer.

## Current Behavior Inventory (must remain unchanged)

1. **Public API:**
   - `adf_pipeline_create/start/stop/destroy/is_running`
   - `adf_pipeline_feed_opus`
   - `adf_pipeline_get_stats`, `adf_pipeline_set_input_mode`
   - `adf_pipeline_get_fft_bins`, `adf_pipeline_get_latest_fft_bins`
2. **Task topology and priorities (Core 1):**
   - TX: `adf_cap` prio 4, `adf_enc` prio 3
   - RX: `adf_dec` prio 4, `adf_play` prio 5
3. **Task startup order constraints:**
   - TX: create encode first, set `pcm_buffer` consumer, then create capture.
   - RX: create decode first, create playback, then set `opus_buffer` and `pcm_buffer` consumers.
4. **Runtime data flow:**
   - TX capture -> mono frame -> PCM ring -> encode -> batched mesh send.
   - RX feed_opus -> opus item ring -> decode/FEC/PLC -> PCM ring -> playback.
5. **Timing model:**
   - Event-driven via ring-buffer notifications and `ulTaskNotifyTake`.
   - No conversion to polling loops.
6. **Debug/test toggles:**
   - `TX_TEST_TONE_MODE`, `RX_TEST_TONE_MODE` behavior stays equivalent.

## Target Module Map

### Public entry point (kept stable)

- `lib/audio/src/adf_pipeline.c`
  - Keeps all current exported symbols from `adf_pipeline.h`.
  - Becomes orchestration layer only.
  - Owns lifecycle API wiring and delegates to internal modules.

### New internal modules

1. `lib/audio/src/adf_pipeline_core.c`
   - Pipeline object lifecycle internals:
     - allocation/init of `struct adf_pipeline`
     - ring buffer and mutex creation/destruction
     - opus encoder/decoder init helpers
   - `start/stop` internal helpers for task creation/deletion logic
   - preserves exact startup/teardown order and task pinning.

2. `lib/audio/src/adf_pipeline_tx.c`
   - TX task implementations:
     - `tx_capture_task`
     - `tx_encode_task`
   - TX-specific helpers:
     - peak/activity tracking
     - local output mirroring in COMBO
     - encode batching + mesh header fill/send

3. `lib/audio/src/adf_pipeline_rx.c`
   - RX path implementations:
     - `adf_pipeline_feed_opus` internals
     - `rx_decode_task`
     - `rx_playback_task`
   - sequence tracking + FEC/PLC insertion path
   - jitter prefill / underrun concealment logic

4. `lib/audio/src/adf_pipeline_fft.c`
   - FFT state init and frame processing
   - fft-bin retrieval helpers for portal telemetry
   - keeps COMBO FFT disable behavior intact

5. `lib/audio/src/adf_pipeline_state.h` (new internal header)
   - Full definition of `struct adf_pipeline`.
   - Shared static-buffer declarations as `extern` where needed.
   - Internal helper prototypes shared across split modules.
   - **Not** installed as public include.

6. `lib/audio/src/adf_pipeline_internal.h` (new internal header)
   - Internal function contracts only (module boundaries).
   - Keeps `adf_pipeline_state.h` focused on data layout.

### Optional/Deferred module

- `lib/audio/src/adf_pipeline_test_modes.c` (optional later)
  - Extract `TX_TEST_TONE_MODE` / `RX_TEST_TONE_MODE` paths after primary split.
  - Defer until parity is proven.

## State Sharing Rules

1. **Single owning state type:**
   - `struct adf_pipeline` is declared exactly once in `adf_pipeline_state.h`.
   - No duplicate struct fragments.

2. **Public/private boundary:**
   - `adf_pipeline.h` remains opaque handle only.
   - All new internal fields stay private.

3. **Global/static data policy:**
   - Keep current static frame buffers in one C file as the owner (recommended: `adf_pipeline_core.c`), export via `extern` in internal header.
   - Keep FFT globals (`s_fft_*`) owned by `adf_pipeline_fft.c` only.
   - Keep `s_latest_pipeline` owned by core module with accessor helpers.

4. **Concurrency policy:**
   - Fields touched across tasks remain guarded as today:
     - `mutex` for stats/fft bin copies and lifecycle transitions
     - `volatile` semantics retained for run flags / input mode
   - No new lock layering that can invert current lock order.

5. **Network/audio boundary:**
   - TX continues to call `network_send_audio()` only from encode path.
   - RX ingest API remains `adf_pipeline_feed_opus()`; network layer contract unchanged.

## Extraction Sequence (safe, incremental)

1. **Step 1 — Internal headers introduced (no logic move):**
   - Add `adf_pipeline_state.h` and `adf_pipeline_internal.h`.
   - Compile with existing monolithic C file unchanged.

2. **Step 2 — Move FFT block first:**
   - Move `fft_init_once`, `fft_process_frame`, fft getters to `adf_pipeline_fft.c`.
   - Keep exported API symbols in `adf_pipeline.c` as wrappers if needed.
   - Validate FFT bins unchanged on RX/TX and COMBO disable path still active.

3. **Step 3 — Move TX tasks:**
   - Move `tx_capture_task`, `tx_encode_task`, and TX helpers into `adf_pipeline_tx.c`.
   - Keep task names (`"adf_cap"`, `"adf_enc"`) and priority constants unchanged.
   - Verify no change to batching and input-activity gating.

4. **Step 4 — Move RX ingest/decode/playback:**
   - Move `adf_pipeline_feed_opus` internals + `rx_decode_task` + `rx_playback_task` into `adf_pipeline_rx.c`.
   - Preserve sequence-tracker/FEC/PLC logic and underrun concealment behavior.

5. **Step 5 — Move lifecycle core:**
   - Move create/start/stop/destroy internals and opus init into `adf_pipeline_core.c`.
   - Leave public API entry points in `adf_pipeline.c` delegating to core internals.

6. **Step 6 — Cleanup and parity checks:**
   - Remove dead static prototypes from `adf_pipeline.c`.
   - Confirm symbol visibility and include graph cleanliness.

## Runtime-Behavior Risk Controls

1. **Task identity parity:** do not change task names, priorities, stack sizes, core pinning.
2. **Startup sequencing parity:** preserve exact order and consumer registration timing.
3. **Ring-buffer contract parity:** keep PCM as stream mode and Opus as item mode.
4. **Packet semantics parity:** keep batch framing/header fields and TX seq progression exactly as-is.
5. **Loss-concealment parity:** preserve FEC-on-single-gap and bounded PLC injection paths.
6. **Underrun behavior parity:** keep prefill logic, grace delay, and last-good-frame replay.
7. **Instrumentation parity:** preserve key logs and counters (`frames_processed`, `frames_dropped`, underruns, encode/decode averages).
8. **Build gate parity:** each extraction step requires successful `pio run -e tx && pio run -e rx && pio run -e combo`.

## Migration Checklist

- [ ] Add internal headers and compile cleanly.
- [ ] Extract FFT module and verify portal FFT APIs unchanged.
- [ ] Extract TX module and verify tone/AUX/local-output behavior unchanged.
- [ ] Extract RX module and verify feed/decode/playback + concealment unchanged.
- [ ] Extract core lifecycle module and keep public API stable.
- [ ] Confirm all three firmware environments build after each step.
- [ ] Run targeted runtime sanity checks (TX->RX stream, COMBO local monitor, underrun logs).

## Compatibility Statement

- Public API compatibility is maintained by design.
- No algorithmic changes are planned in this design phase.
- If an unavoidable signature change appears during extraction, it must be documented in this file before implementation proceeds.
