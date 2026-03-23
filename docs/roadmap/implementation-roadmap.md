# MeshNet Audio Roadmap (Single Linear Track)

This is the canonical “what’s next” roadmap for SoundMesh.

The sequence is linear and prioritized for **small internal pilot readiness first**.  
Feature expansion happens only after pilot hardening gates pass.

---

## Current baseline (implemented)

- Audio: 48 kHz, 16-bit mono internal PCM, 20 ms Opus frames.
- Mesh: designated root behavior (TX/COMBO root, RX joins fixed root).
- Portal firmware APIs: `/api/status`, `/api/uplink`, `/api/ota`, `/ws`.
- Portal UI source path: `lib/control/portal-ui/` (SPIFFS assets synced to `data/`).
- Architecture refactors completed:
  - Network split under `lib/network/src/mesh/`
  - Audio pipeline split under `lib/audio/src/adf_pipeline_*`
- Tests:
  - Native test suite includes transport, sequence, dedupe, mesh query, JSON extraction, and portal API contract checks.

---

## Linear execution track

## Stage 0 — Pilot blocking hardening (do first)

### 0.1 Control-plane security minimum
- Add auth token enforcement for:
  - `POST /api/ota`
  - `POST /api/uplink`
  - control-capable WS paths
- Return deterministic `401` on missing/invalid auth.
- Redact sensitive fields from API/log outputs (SSID details, OTA URL traces where not strictly required).

### 0.2 Runtime safety fixes for known failure paths
- Validate `xTaskCreate` results for mesh tasks and fail safely with explicit logs.
- Guard zero-length RX packets before packet-type dispatch.
- Add rejoin circuit-breaker/degraded-state behavior to avoid endless churn loops.

### 0.3 Contract and operator consistency
- Choose and enforce one portal scope for pilot operation (root-only policy recommended).
- Normalize external role vocabulary to `SRC/OUT` across API + UI + tests.
- Ensure demo mode is explicit only (no silent fallback that can mask live failures).

### 0.4 Pilot operations runbook
- Publish a single runbook:
  - preflight checklist
  - canary rollout flow
  - abort criteria
  - rollback/reset procedure
  - escalation ownership

### 0.5 Minimum pilot validation gate
- Keep:
  - `pio test -e native`
  - `pio run -e tx && pio run -e rx && pio run -e combo`
- Add repeatable two-node HIL smoke:
  - TX-first boot
  - RX-first boot
  - simultaneous boot
  - reconnect and stream-resume checks

### Stage 0 acceptance criteria (must all pass)
- Unauthorized control requests are rejected (`401`) for protected endpoints.
- Sensitive fields are absent or redacted in defined responses/logs.
- Mesh task startup failures are surfaced; zero-length frame path is safely handled.
- Demo mode cannot be mistaken for live control mode.
- All automated gates pass and HIL smoke passes for 2-node pilot topology.

---

## Stage 1 — Post-pilot stabilization

### 1.1 OTA resilience
- Move to dual-slot OTA partition strategy (`otadata`, `ota_0`, `ota_1`) with rollback confirmation.
- Add explicit bad-image recovery verification step.

### 1.2 CI and quality gate enforcement
- Enforce native + tx/rx/combo build matrix in CI/branch protection.
- Add negative/fuzz-style portal payload tests for malformed inputs and boundary cases.

### 1.3 Benchmarks and SLO discipline
- Define and automate benchmark captures for:
  - join/rejoin time
  - stream continuity
  - underruns/min
  - decode failures
  - loss trends
- Produce release-candidate benchmark report as a required artifact.

### 1.4 Maintainability hardening
- Encapsulate mutable mesh shared state behind access APIs (reduce direct global mutation).
- Reduce private-header leakage (avoid exposing network `src/` internals to unrelated modules).
- Introduce an audio transport adapter boundary to decouple audio pipeline from network framing details.

### Stage 1 acceptance criteria
- OTA rollback behavior proven via test procedure.
- CI required checks block merges on failing test/build gates.
- Benchmark report exists for release candidates with agreed thresholds.
- Reduced direct cross-module coupling in critical paths.

---

## Stage 2 — Scale-readiness and endurance

### 2.1 Reliability at longer durations and larger topologies
- Add periodic soak automation (nightly 6h, pre-release 24h).
- Add controlled fault-injection scenarios (disconnect/reconnect/loss bursts/root restart).

### 2.2 Security and operations maturity
- Strengthen auth model and audit events for control-plane operations.
- Add rate limiting and incident-friendly telemetry for rejected/failed control actions.

### 2.3 Performance and architecture refinements
- Optimize dedupe/data-plane hot paths if benchmarks show bottlenecks.
- Continue splitting oversized control modules where needed.
- Tighten observability contract versioning and compatibility policy.

### Stage 2 acceptance criteria
- 24h soak passes with no crashes and acceptable continuity metrics.
- Fault-injection matrix meets recovery targets.
- Security/ops telemetry supports incident triage.
- Multi-node stability goals (>=3 nodes) are met consistently.

---

## Feature-expansion lane (after Stage 0 gates pass)

These are intentionally sequenced after pilot blockers:

1. Multi-stream mixer command plane and richer portal controls.
2. USB audio input production implementation (currently stubbed).
3. 24-bit + 5 ms pipeline target (explicit future milestone, not current baseline).

---

## Immediate “next 5” checklist

1. Implement token auth for OTA/uplink/control WS paths.
2. Add runtime guards (`xTaskCreate` checks, zero-length frame guard, rejoin circuit breaker).
3. Finalize `SRC/OUT` contract consistency + explicit demo-mode policy.
4. Publish pilot runbook and canary/abort workflow.
5. Add and run two-node HIL smoke checklist as release gate.

---

## Notes

- Previous `professionalization-roadmap.md` is now considered a supporting reference input.
- This file is the single source of truth for execution order.
