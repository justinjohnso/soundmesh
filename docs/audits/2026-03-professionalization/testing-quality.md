# Testing Quality Audit — MeshNet Audio

## Scope and method

This audit evaluates current testing maturity across:
- native/unit
- integration
- hardware-in-the-loop (HIL)
- soak/endurance
- gate quality and release confidence

Evidence reviewed:
- `platformio.ini` (`env:native` Unity host tests)
- `test/native/*` test suites
- `tools/test_all.sh`
- `README.md` validation workflow
- current repository automation state (no CI workflow present)
- current build/test execution (`pio test -e native && pio run -e tx && pio run -e rx && pio run -e combo`)

## Current maturity snapshot

### What is strong now

1. **Deterministic host-side unit coverage exists and is healthy**
   - 7 native Unity suites, 84 passing test cases.
   - Strong pure-logic coverage in critical areas:
     - frame parsing/batching (`test_frame_codec`)
     - sequence loss/FEC/PLC rules (`test_sequence_tracker`)
     - dedupe cache behavior (`test_mesh_dedupe`)
     - mesh query logic and reconnect guards (`test_mesh_queries`)
     - uplink wire encode/decode (`test_uplink_control`)
     - JSON extraction and API contract validation (`test_portal_json_extract`, `test_portal_api_contract`)

2. **Cross-target compile gate exists**
   - `tx`, `rx`, and `combo` all build successfully from current baseline.
   - This catches compile drift across role-specific code paths.

3. **Contract-thinking is already present**
   - Portal API contract tests validate required fields and invariants, including forward-compatibility behavior.

### Maturity rating (small-pilot readiness)

- **Unit/native:** Medium-High
- **Integration (firmware + transport + tasks):** Low
- **HIL:** Low
- **Soak/reliability:** Low
- **Automation/gating discipline:** Low-Medium
- **Overall confidence for professional small pilot:** **Insufficient today without manual expert supervision**

## High-risk gaps (priority order)

## P0 — Must close for credible pilot confidence

1. **No automated HIL acceptance suite**
   - Missing repeatable two-node tests (TX/COMBO + RX) that assert real outcomes: join time, stream start, sustained audio delivery, reconnect behavior.
   - Current confidence depends on ad hoc manual observation.

2. **No integration tests for RTOS/audio/network boundary behavior**
   - Native tests isolate pure logic, but there is little automated validation of task timing, ring buffer pressure, queue backoff, and decode/playback under live packet jitter/loss.

3. **No soak/endurance gate**
   - No scripted 4–24h run to measure drift, memory stability, drop/underrun trends, reconnect resilience, or watchdog behavior.

4. **No CI/branch-protection style enforcement**
   - Tests/builds are documented but not enforced by workflow automation.
   - Regressions can merge if humans skip manual validation.

## P1 — Important next-level hardening

5. **No fault-injection matrix**
   - No systematic packet-loss, burst-loss, parent disconnect, root restart, or noisy-RF scenario automation with quantified pass/fail thresholds.

6. **No performance/quality SLO gates**
   - Lacking explicit thresholds (e.g., join ≤ X s, underrun rate ≤ Y/min, reconnect ≤ Z s, CPU/heap floors).

7. **No artifacted test evidence**
   - Serial logs, metrics summaries, and run verdicts are not consistently archived per run.

## P2 — Useful but not blocking first pilot

8. **Limited negative/security-style API testing on device path**
   - Contract checks exist in native tests, but device-level HTTP fuzz/abuse and malformed payload stress are not automated.

9. **No coverage visibility trendline**
   - No coverage measurement or trend tracking for host test suites.

## Gate quality assessment

Current gate quality is **good for syntax/logic regressions in tested helpers**, but **weak for system behavior** because the highest-risk failures in this product are emergent (timing, RF conditions, buffer interactions, long-run degradation).

A professional small-pilot bar requires reproducible system-level evidence, not just passing host tests.

## Phased test matrix (concrete)

## 1) Pre-merge matrix (target runtime: 8–15 min)

Purpose: fast regression guard for every PR.

| Lane | Test | Target | Pass criteria |
|---|---|---:|---|
| Static build gate | `pio run -e tx`, `-e rx`, `-e combo` | every PR | all compile, no new warnings promoted to errors |
| Native logic gate | `pio test -e native` | every PR | 100% pass |
| Simulated integration (host harness) | new deterministic harness for packet reorder/loss + ringbuffer pressure logic | every PR touching audio/network | fixed scenario set passes |
| API contract gate | existing portal contract suite + added malformed-body cases | every PR touching portal | all required status/error mappings pass |

Required additions to enable this lane:
- add a small host integration harness around packet stream + decode scheduling decisions
- add malformed/oversized/edge JSON payload tests mapped to expected HTTP behavior

## 2) Nightly matrix (target runtime: 60–180 min)

Purpose: catch timing/reliability regressions not visible pre-merge.

| Lane | Test | Duration | Pass criteria |
|---|---|---:|---|
| Two-node HIL smoke | TX/COMBO + RX boot-order permutations (TX-first, RX-first, simultaneous) | 15 min | association + stream start in all permutations |
| Fault-injection HIL | scripted RF disturbance / parent disconnect / root reboot / packet-drop profile | 20–40 min | recovery within threshold, no deadlock/watchdog |
| Audio continuity | continuous stream with counters (drops, underruns, PLC/FEC usage) | 20–60 min | counters below defined thresholds |
| Resource stability | heap/stack watermark sampling | full nightly | no monotonic leak beyond budget |

Required additions to enable this lane:
- scripted serial orchestration (reuse/extend `tools/multi_monitor.py`)
- machine-readable metrics lines from firmware (`key=value` style)
- nightly thresholds file committed to repo

## 3) Pre-release matrix (target runtime: 1–3 days)

Purpose: pilot readiness sign-off.

| Lane | Test | Duration | Pass criteria |
|---|---|---:|---|
| 24h soak | representative 2-node setup with realistic audio | 24h | stable stream, no crash/reset, bounded drop/underrun rates |
| Reboot/rejoin endurance | repeated power/network cycling | 4–8h | deterministic recovery each cycle |
| Environmental realism | near/far placement + interference windows | 4–8h | acceptable audio continuity and recovery |
| OTA/uplink operational safety | OTA start/fail/retry + uplink apply/sync under load | 2–4h | expected state transitions and safe failure handling |

Release evidence package required:
- test run manifest
- logs + summarized metrics
- explicit go/no-go against thresholds

## Prioritized gap-closure plan

## Phase A (1 week) — establish enforceable baseline

1. Add CI workflow to run:
   - `pio test -e native`
   - `pio run -e tx && pio run -e rx && pio run -e combo`
2. Fail PR on any lane failure.
3. Standardize output summary artifact (test/build logs).

**Outcome:** objective pre-merge enforcement.

## Phase B (1–2 weeks) — integration and HIL smoke

4. Build host integration harness for packet/jitter/ordering + pipeline decision logic.
5. Add automated two-node HIL smoke script with pass/fail parser.
6. Define first SLO thresholds:
   - join/startup time
   - reconnect time
   - underruns/min
   - decode failure rate

**Outcome:** catches regressions in real system behavior.

## Phase C (2–3 weeks) — nightly reliability and soak

7. Add nightly fault-injection scenarios and metric trend output.
8. Add 6–24h soak jobs (staged: 6h nightly, 24h pre-release).
9. Add release checklist requiring passing soak + endurance artifacts.

**Outcome:** credible small-pilot reliability confidence.

## Minimal professional small-pilot confidence bar

Before pilot rollout, require all of the following:
- enforced pre-merge CI build + native tests
- automated two-node HIL smoke pass in nightly
- documented SLO thresholds and nightly trend report
- one successful 24h soak and one reboot/rejoin endurance campaign per release candidate

Without this bar, risk of field-visible instability remains high despite strong unit test health.
