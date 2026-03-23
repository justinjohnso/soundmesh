# Runtime Robustness Audit

Date: 2026-03-23  
Scope: Runtime error handling, watchdog/task lifecycle safety, memory/buffer safety, disruption recovery, and long-session resilience.

## Audit Method

- Reviewed runtime-critical code paths in:
  - `lib/network/src/mesh/*`
  - `lib/audio/src/adf_pipeline_*.c`
  - `lib/audio/src/ring_buffer.c`
  - `src/tx/main.c`, `src/rx/main.c`, `src/combo/main.c`
- Cross-checked field validation evidence in:
  - `docs/history/progress-notes/MESH_RELIABILITY_FIX_2026_03_20.md`
  - `docs/history/progress-notes/mesh-reliability-fix.md`
- Verified current build/test baseline:
  - `pio run -e tx && pio run -e rx && pio run -e combo` (all pass)
  - `pio test -e native` (84/84 tests pass)

## Executive Summary

The system is materially more robust than earlier revisions (event-driven startup, fixed-root stabilization, stream-loss hysteresis, and rejoin support are present). The most important residual risks are concentrated in: (1) network task startup/error-path hardening, (2) edge-case packet validation, and (3) long-session behavior under persistent mesh churn.

## What Is Strong Today

- **Event-driven startup and decoupled layers** reduce race-prone polling (`network_register_startup_notification`, task notifications).
- **Non-blocking mesh send with explicit drop accounting** limits hard stalls (`MESH_DATA_NONBLOCK`, `total_drops`).
- **Audio pipeline allocates static frame buffers** and uses bounded ring buffers with explicit overflow handling.
- **RX recovery behavior exists** (`STREAM_SILENCE_TIMEOUT_MS`, `STREAM_SILENCE_CONFIRM_MS`, `network_trigger_rejoin()`).
- **Field evidence shows multi-node stability gains** in recent soak tests with reduced RF/auth churn (`docs/history/progress-notes/MESH_RELIABILITY_FIX_2026_03_20.md`).

## Risk Register

| # | Failure mode | Likelihood | Impact | Evidence | Current mitigation | Recommended mitigation |
|---|---|---|---|---|---|---|
| R1 | Mesh runtime tasks created without return-code checks can fail silently (`mesh_rx`, `mesh_hb`) | Medium | High | `lib/network/src/mesh/mesh_init.c:110-111` uses `xTaskCreate(...)` with no result validation | None for task-create failure path | Check `xTaskCreate` return values and `TaskHandle_t != NULL`; fail fast with clear log + safe fallback/reboot path |
| R2 | Zero-length mesh frame can cause out-of-bounds read (`data.data[0]`) in RX loop | Low-Medium | High | `lib/network/src/mesh/mesh_rx.c:60` reads first byte without `data.size > 0` guard | Downstream checks exist for many packet types | Add immediate `if (data.size == 0) continue;` before first-byte dispatch |
| R3 | Startup notification registry is fixed at two tasks; additional consumers fail with `ESP_ERR_NO_MEM` | Medium (as system grows) | Medium | `lib/network/src/mesh/mesh_state.c:52-53, 75-83` (`waiting_task_handles[2]`) | Current design fits present callsites | Increase capacity or make registration idempotent/set-based; log explicit warning on overflow |
| R4 | Task shutdown relies on forced `vTaskDelete()` after delay; cooperative cleanup is partial | Medium | Medium | `lib/audio/src/adf_pipeline_core.c:247-268`; tasks terminate into infinite sleep loops (`adf_pipeline_tx.c`, `adf_pipeline_rx.c`) | Stop path notifies tasks first | Replace terminal `while(1)` parking with `vTaskDelete(NULL)` and explicit resource release checkpoints |
| R5 | Persistent mesh churn can keep RX in repeated rejoin cycles and prolonged starvation windows | Medium | High (user-visible dropouts) | `src/rx/main.c:435-451`, field logs in `docs/history/progress-notes/MESH_RELIABILITY_FIX_2026_03_20.md` show branch instability episodes | Stream-loss confirmation + bounded rejoin backoff | Add capped rejoin budget/time-window circuit breaker + telemetry flag for operator intervention |
| R6 | Dedupe cache is O(N) linear scan per frame; CPU pressure rises with packet rate/topology scale | Medium | Medium | `lib/network/src/mesh/mesh_dedupe.c:14-20` scans `DEDUPE_CACHE_SIZE` entries each frame | Cache size bounded to 256 | Move to hash/indexed lookup (or bloom + ring) while preserving bounded memory |
| R7 | Long-session counters can wrap and degrade observability/alerting quality | Medium | Low-Medium | Numerous 32-bit counters in `src/rx/main.c`, `lib/network/src/mesh/mesh_state.c` | Wrap behavior tolerated implicitly | Normalize with rollover-safe delta math and periodic counter snapshot/reset semantics |
| R8 | Build-time warning indicates flash-size mismatch; deployment/runtime partition assumptions may drift | Medium | Medium | Build output warns: “Expected 8MB, found 2MB” during `pio run -e tx/rx/combo` | Warning acknowledged in logs | Align board/SDK flash config across environments and verify on-device flash probe in CI/build checks |

## Domain Findings

### 1) Error Handling Robustness

- **Strong:** extensive `ESP_ERROR_CHECK()` on critical init paths (`network_init_mesh`, audio init).
- **Gap:** some runtime paths intentionally continue on warning (good for resilience), but mesh task creation lacks hard failure checks (R1).
- **Gap:** packet pre-dispatch guard for empty frame is missing (R2).

### 2) Watchdog and Task Lifecycle Safety

- **Strong:** TX/COMBO main tasks initialize/feed task WDT during blocking startup and main loop (`src/tx/main.c`, `src/combo/main.c`).
- **Gap:** RX does not use task WDT in main loop; acceptable today but weaker safety parity under future complexity.
- **Gap:** audio worker tasks park in infinite delay loops on exit and are externally deleted (R4), which is workable but brittle for future refactors.

### 3) Memory and Buffer Safety

- **Strong:** bounded static audio buffers, explicit ring-buffer capacity checks, compile-time sanity asserts in `build.h`.
- **Strong:** ring-buffer API avoids BYTEBUF over-consumption via `xRingbufferReceiveUpTo`.
- **Gap:** dedupe algorithm is compute-inefficient under scale (R6), which can become a runtime quality issue (jitter/underruns) rather than a crash issue.

### 4) Recovery Under Mesh/Audio Disruption

- **Strong:** hysteresis before stream-loss declaration and explicit rejoin hook improve resilience.
- **Strong:** field docs show meaningful stability improvements after RF/power/topology tuning.
- **Gap:** no explicit rejoin circuit breaker/health state when repeated recovery attempts fail (R5).

### 5) Long-Session Risks

- **Strong:** no obvious unbounded heap allocation loop in audited paths.
- **Gap:** observability counters and fixed-size registries can become maintenance hazards in long soak or expanded feature sets (R3, R7).

## Immediate Mitigations (Priority Order)

1. **Harden mesh task startup failures** (`mesh_init.c`): validate `xTaskCreate` results and fail safely.  
2. **Add zero-length packet guard** (`mesh_rx.c`) before reading `data.data[0]`.  
3. **Refactor audio task exit path** to cooperative self-delete (`vTaskDelete(NULL)`) rather than infinite parked loops.  
4. **Add rejoin circuit-breaker policy** (max attempts/time window + degraded-state telemetry).  
5. **Scale-safe startup notification registry** (idempotent + configurable capacity).  

## Validation Evidence Used

- **Build validation:** `tx`, `rx`, `combo` all compile successfully on this audit pass.
- **Unit validation:** `pio test -e native` passes all 84 tests, including:
  - `test_sequence_tracker` (loss/FEC/PLC behavior)
  - `test_mesh_dedupe` (dedupe cache behavior)
  - `test_mesh_queries` (rejoin and stream-ready logic)
  - `test_frame_codec` (framing bounds/compat logic)
- **Field/runtime evidence:** recent progress logs document multi-node soak outcomes and remaining branch-specific instability windows.

## Audit Verdict

Runtime robustness is **good but not yet hardened for all edge conditions**. The system is stable enough for iterative field deployment, but R1/R2/R5 should be treated as near-term reliability work to reduce hard-failure probability and long-session degradation.
