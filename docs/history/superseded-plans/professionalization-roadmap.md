# SoundMesh Professionalization Roadmap (Pilot-First) — Superseded

> Status: superseded as primary execution plan.  
> Canonical linear roadmap: `docs/roadmap/implementation-roadmap.md`.

Date: 2026-03-23  
Scope inputs synthesized:
- `roadmap-alignment-audit.md`
- `architecture-maintainability-audit.md`
- `runtime-robustness-audit.md`
- `testing-quality-audit.md`
- `security-operations-audit.md`
- `benchmark-gap-analysis.md`

## Objective

Define one coherent professionalization plan that prioritizes a **small internal pilot first** (supervised, trusted operators, controlled physical access), then hardens for broader and less supervised operation.

## Decision principles

1. **Pilot reliability and safety over feature expansion.**
2. **Resolve contract drift before adding new APIs/UI behavior.**
3. **Use measurable go/no-go gates per phase.**
4. **Sequence work to reduce high-blast-radius risks first (security + runtime + operational readiness).**

## Contradictions resolved into a single recommendation set

| Conflict in audits/docs | Final recommendation |
|---|---|
| Portal scope: root-only plan vs RX currently serving portal | **Pilot policy: root-only portal exposure (TX/COMBO).** RX portal serving is disabled for pilot branch/config to reduce control-plane surface and operator confusion. |
| Node role vocabulary mismatch (`SRC/OUT` vs `TX/RX`) | **External contract standard: `SRC/OUT`.** Internal code symbols may remain TX/RX temporarily, but `/api/status`, `/ws`, UI labels, and tests must present/expect `SRC/OUT`. |
| Demo-mode fallback can mask failures | **Pilot policy: no automatic demo fallback.** If demo exists, it is explicit and visibly blocked from control actions. |
| Portal memory threshold docs (64KB vs 30KB runtime) | **Single source for pilot: 30KB runtime threshold** (current implementation), documented with rationale and monitoring requirement. |
| “Planned” refactors already implemented (audio/mesh split) | Treat split architecture as **current baseline**, not future work. Remaining items are boundary-hardening, not initial modularization. |

---

## Phase 0 — Must-have before pilot (go-live gate)

Target: supervised 2-node pilot (1 source root + 1 receiver), repeatable operation, bounded risk.

### Work items

1. **Control-plane minimum security (P0)**
   - Require auth token for `POST /api/ota`, `POST /api/uplink`, and `/ws` control path.
   - Return `401` for missing/invalid auth.
   - Remove or mask sensitive fields (`lastUrl`, SSID) from API responses and logs.

2. **Operational safety and rollout discipline (P0)**
   - Publish one operator runbook: preflight, canary flow, rollback, uplink reset, escalation owner.
   - Define rollout policy: canary one node, observation window, explicit abort criteria.

3. **Runtime hardening for known edge failures (P0)**
   - Validate `xTaskCreate` results for `mesh_rx`/`mesh_hb`; fail safely with explicit log path.
   - Add zero-length frame guard in mesh RX loop before first-byte dispatch.
   - Add capped rejoin circuit-breaker and degraded-state telemetry flag.

4. **Contract and UX consistency for pilot operations (P0)**
   - Enforce root-only portal policy in docs + build behavior.
   - Normalize external role naming to `SRC/OUT` end-to-end.
   - Disable automatic demo fallback (or show hard LIVE/DEMO indicator and disable controls in demo).

5. **Minimum pilot evidence gate (P0)**
   - Keep host and compile gates green.
   - Add repeatable two-node HIL smoke checklist/script for boot-order permutations and stream start.

### Phase 0 acceptance gate (all required)

- **Security gate**
  - 100% of OTA/uplink/control WS mutation paths reject unauthorized requests (`401`).
  - `GET /api/status` and `GET /api/uplink` contain no raw SSID/password; OTA status contains no full URL.
- **Runtime gate**
  - Mesh task startup failure is explicitly detected and surfaced in logs.
  - Zero-length packet test case exists and passes in native suite.
  - Rejoin circuit-breaker triggers degraded state after configured max attempts/window.
- **Contract/UX gate**
  - API contract tests pass with `SRC/OUT` role values.
  - Pilot UI cannot silently enter demo mode while indicating live control availability.
  - Root-only portal behavior documented and validated in pilot setup checklist.
- **Validation gate**
  - `pio test -e native` passes.
  - `pio run -e tx && pio run -e rx && pio run -e combo` passes.
  - Two-node HIL smoke pass for TX-first, RX-first, simultaneous boot.

---

## Phase 1 — Should-have shortly after pilot (stabilization)

Target: reduce operator burden, improve maintainability, and make releases safer/repeatable.

### Work items

1. **OTA resilience and safer update controls**
   - Migrate partitioning to include `otadata`, `ota_0`, `ota_1` (+ rescue strategy).
   - Implement rollback confirmation (`esp_ota_mark_app_valid_cancel_rollback`).
   - Add OTA source allowlist and signed manifest hash checks.

2. **Testing and release automation uplift**
   - Add CI gate for native tests + tx/rx/combo compile matrix.
   - Add malformed/fuzzed portal payload negative tests.
   - Produce artifacted test summaries (logs + metrics + verdict).

3. **Benchmark operationalization**
   - Create one benchmark checklist with thresholds (join time, reconnect time, underruns/min, decode failures, drop rate).
   - Add periodic health snapshot payload for consistent triage.

4. **Architecture boundary hardening (highest ROI maintainability)**
   - Encapsulate mesh mutable globals behind access APIs.
   - Hide private network headers from external include paths.
   - Introduce audio transport adapter to remove direct audio→network coupling.

### Phase 1 acceptance gate

- OTA rollback validated by one forced-bad-image test with automatic recovery.
- CI required status checks enforce build+native pass on protected branch.
- Negative portal API payload suite passes (malformed JSON, escaped edge cases, oversize rejection).
- Benchmark report generated for each release candidate with all SLO thresholds met.
- No direct external mutation of mesh global state outside approved accessor API.

---

## Phase 2 — Later-scale items (multi-node and less supervised operation)

Target: confidently scale beyond small internal pilot and reduce long-term maintenance drag.

### Work items

1. **Reliability at scale and long-session assurance**
   - Automated nightly fault-injection (loss bursts, parent disconnect, root reboot, congestion).
   - Regular soak regime (6h nightly, 24h pre-release).
   - Counter rollover-safe semantics and trend reporting.

2. **Control-plane and operations maturity**
   - Stronger control auth model (token lifecycle or signed command envelope).
   - Route throttling/rate-limit and security event audit trail.
   - Expanded operator dashboards and incident drill cadence.

3. **Performance and architecture optimization**
   - Replace O(N) dedupe lookup with bounded indexed/hybrid strategy.
   - Split large control files (`display_ssd1306.c`, `portal_state.c`) by responsibility.
   - Move control/audio FFT coupling to publish-subscribe snapshot model.

4. **Extended quality bar for scale releases**
   - Pre-release endurance campaigns across environment variants/interference patterns.
   - Defined go/no-go policy tied to SLO trends, not single-run success.

### Phase 2 acceptance gate

- 24h soak passes on release candidate with no crash/reset and metrics within thresholds.
- Fault-injection matrix passes at defined recovery targets.
- Security telemetry includes failed auth, rejected OTA, and rate-limit counters with alerting.
- Multi-node test topology (>=3 nodes) passes stability and continuity thresholds.

---

## Prioritized execution order

1. **Phase 0 security + runtime + contract consistency** (pilot blocker removal)
2. **Phase 0 HIL smoke + runbook rollout** (operator-ready pilot)
3. **Phase 1 OTA rollback + CI + benchmark gates** (stabilization)
4. **Phase 1 boundary hardening** (maintainability acceleration)
5. **Phase 2 scale, endurance, and optimization**

## Pilot release readiness summary

A pilot is considered ready only when **all Phase 0 acceptance gates pass**.  
Phase 1 items are scheduled immediately after pilot start to prevent temporary controls from becoming permanent operational debt.
