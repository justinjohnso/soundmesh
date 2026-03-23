# SoundMesh Benchmark Gap Analysis (ESP-IDF Mesh/Audio)

## Scope and method

This benchmark compares the current SoundMesh implementation against:

- ESP-IDF mesh example patterns (`examples/mesh/internal_communication`, `examples/mesh/manual_networking`) already referenced in project planning docs.
- Common embedded audio-over-mesh practices for ESP32-S3 class devices (event-driven tasks, bounded buffering, observability, control-plane hardening, and operational test discipline).

Assessment is grounded in current repo code and docs, especially:

- `lib/network/src/mesh/mesh_init.c`, `mesh_events.c`, `mesh_tx.c`, `mesh_rx.c`, `mesh_dedupe.c`
- `lib/audio/src/adf_pipeline_core.c`, `adf_pipeline_rx.c`, `sequence_tracker.c`
- `lib/control/src/portal_http.c`, `json_extract.c`
- `lib/config/include/config/build.h`
- `docs/roadmap/implementation-roadmap.md`, `docs/architecture/network.md`, `docs/history/progress-notes/mesh-reliability-fix.md`

---

## Benchmark dimensions

| Dimension | Benchmark expectation | SoundMesh status |
|---|---|---|
| Mesh bootstrap and role control | Deterministic root behavior, stable join flow, scan discipline | **Lead**: designated root + fixed-root flow implemented and documented |
| Data-plane resilience | Sequence integrity, dedupe, bounded concealment, adaptive buffering | **Lead/Partial**: seq tracking + FEC/PLC + jitter prefill present; tuning still iterative |
| Tasking and real-time model | Core separation, event-driven loops, bounded stack/memory | **Lead**: clear core split, pinned tasks, compile-time sanity checks |
| Control-plane robustness | Structured payload parsing, strict validation, robust error handling | **Lag**: JSON extraction uses string matching; limited parser robustness |
| Observability and SLOs | Measurable latency/loss/underrun KPIs with routine benchmark runs | **Partial**: good counters/logs/tests, but no single repeatable benchmark harness/report cadence |
| Security posture (local mesh ops) | Authenticated control changes, hardened OTA/control interfaces | **Lag**: functional controls exist, but limited request hardening/auth semantics |
| Test depth across layers | Unit + contract + integration/soak for realistic RF/audio conditions | **Partial**: strong native/unit coverage; limited automated RF soak/HIL benchmarking |

---

## Where SoundMesh leads

1. **Topology control and startup determinism**
   - TX/COMBO root designation and fixed-root behavior are explicit (`mesh_init.c`), reducing election ambiguity and boot-order fragility.

2. **Real-time architecture discipline**
   - Audio/network layering is clear, tasks are pinned with explicit priorities and centralized stack constants (`build.h`, pipeline core), aligned with ESP32 dual-core best practice.

3. **Loss handling pragmatism**
   - Sequence-gap handling with bounded PLC injection and FEC recovery (`sequence_tracker.c`, `adf_pipeline_rx.c`) is stronger than many baseline examples.

4. **Contract-level testing foundation**
   - Native tests cover frame codec, dedupe behavior, portal API contracts, and sequence logic; this is above “example-level” maturity.

## Where SoundMesh lags

1. **Portal JSON parsing hardening**
   - `json_extract.c` uses delimiter-based string matching, which is brittle under formatting variance/escaping and harder to extend safely.

2. **Control-plane and OTA guardrails**
   - APIs are functional and tested, but control changes are permissive for a USB-accessible management plane; stronger intent/auth checks are still needed.

3. **Benchmark operationalization**
   - Metrics exist, but there is no single repeatable “benchmark runbook” producing consistent pass/fail artifacts for latency, loss, underrun, and reconnection behavior.

4. **Long-duration mesh reliability automation**
   - Reliability insights are documented, but continuous soak/fault-injection workflows are still mostly manual.

---

## High-leverage adoption opportunities

## Adopt now (1–2 sprints, low/medium risk)

1. **Replace string-based JSON parsing with cJSON for portal endpoints**
   - Apply to `/api/ota` and `/api/uplink` request parsing only (surgical scope).
   - Keep current response schema and existing contract tests; add malformed/escaped-input test cases.
   - Expected impact: fewer parser edge-case failures, easier future endpoint growth.

2. **Add bounded watchdog-aware receive cadence in mesh RX loop**
   - Move from indefinite blocking receive to bounded wait cadence with explicit health tick (same philosophy already used in readiness waits).
   - Expected impact: better fault visibility and safer long-session behavior during rare stack stalls.

3. **Create a single benchmark checklist + acceptance thresholds doc section**
   - Define required measurements for every networking/audio change: packet loss %, underruns/min, reconnect time, end-to-end latency band.
   - Tie to existing commands (`pio run -e tx/rx/combo`, `pio test -e native`) and serial log filters.
   - Expected impact: consistent go/no-go decisions and less regression ambiguity.

4. **Promote key runtime counters into one “health snapshot” payload**
   - Consolidate already-existing counters (drops, underruns, parent disconnects, scan count, route size) into one periodic status object surfaced to portal and logs.
   - Expected impact: faster triage and easier comparison between test runs.

5. **Add negative tests for control payload robustness**
   - Extend native contract tests with fuzzed spacing/ordering/escaping cases and oversize field rejection.
   - Expected impact: hardens control plane with minimal runtime cost.

## Adopt later (after stability milestones, medium/high effort)

1. **Control-plane security hardening profile**
   - Add optional authenticated control actions (token/challenge or signed command envelope) for OTA/uplink mutations.
   - Gate behind compile-time flag to preserve development velocity.

2. **Structured RF soak and fault-injection harness**
   - Scripted long-run scenarios (node joins/leaves, power cycles, induced congestion) with machine-readable output summaries.
   - Enables trend tracking instead of one-off debug sessions.

3. **Evaluate incremental ADF/codec-dev migration boundaries**
   - Not a full rewrite now; pilot only where maintenance pain is highest (codec abstraction boundary) while preserving current working pipeline.

4. **Adaptive QoS loop for mesh send strategy**
   - Dynamically tune batching/backoff/jitter targets from measured link health rather than static defaults.
   - Defer until benchmark baselines are stable and reproducible.

---

## Recommended execution order

1. Parser hardening + tests (fastest risk reduction).
2. Benchmark runbook/thresholds + health snapshot unification.
3. RX loop watchdog cadence improvement.
4. Then pursue later-track security and soak automation.

This sequence keeps changes realistic for current team velocity while increasing reliability confidence per sprint.
