# Demo Candidate Freeze — stutter-freeze-demo-candidate

- Timestamp (UTC): 2026-03-29
- Candidate scope: firmware/software freeze for tomorrow demo
- Final soak verdict: **FAIL**
- Freeze decision: **NO-GO for live stutter-free reliability demo**

## Enforced transport/audio profile (currently in `lib/config/include/config/build.h`)

- `AUDIO_SAMPLE_RATE=48000`, `AUDIO_FRAME_MS=20`
- `OPUS_BITRATE=48000`, `OPUS_EXPECTED_LOSS_PCT=10`, `OPUS_ENABLE_INBAND_FEC=1`
- `MESH_FRAMES_PER_PACKET=2`
- `TRANSPORT_SETTINGS_PROFILE_ID="baseline-current"`
- `TRANSPORT_ROOT_FANOUT_MODE="GROUP|NONBLOCK"`
- `TRANSPORT_TO_ROOT_MODE="TODS|NONBLOCK"`
- `JITTER_PREFILL_FRAMES=4`
- `RX_OUTPUT_VOLUME=2.0f`
- `OUT_OUTPUT_GAIN_DEFAULT_PCT=200`, `OUT_OUTPUT_GAIN_MAX_PCT=400`

Reference: `lib/config/include/config/build.h`

Selection context:

- `docs/quality/reports/20260329-tonight/selected-profile.json`
- `docs/quality/reports/20260329-tonight/matrix-aggregate.md`

## Latest validation results (referenced evidence)

### Build validation

- SRC build: **PASS**
  - `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/build-src.log`
- OUT build: **PASS**
  - `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/build-out.log`

### Tests validation

- Latest recorded native test baseline: `pio test -e native` **PASS (84/84)**
  - `docs/audits/2026-03-professionalization/runtime-robustness.md`
- No newer native test artifact is attached in this freeze packet; docs-only freeze uses existing validated evidence.

### Gate/soak validation

- Final soak packet: `docs/quality/reports/20260329-final-multiout-soak/final-multiout-soak-verdict.md`
- Machine-readable verdict: `docs/quality/reports/20260329-final-multiout-soak/final-multiout-soak-verdict.json`
- Integrity check (packet completeness): `docs/quality/reports/20260329-final-multiout-soak/artifact-integrity.json`
- Prior baseline gate summary: `docs/quality/reports/20260329-fresh-multiout-baseline/demo-quality-gate-summary.md`

## Final soak verdict and blocking metrics

From `docs/quality/reports/20260329-final-multiout-soak/final-multiout-soak-verdict.json`:

- `final_verdict.result=FAIL`
- `demo_ready=false`
- Failed gate: `transport_slo`
- Blocking metrics:
  - `join_time_s=null` (treated as fail)
  - `underruns_per_min=16.6` (threshold <= 1.0)
  - `loss_pct=19.9` (threshold <= 2.0)
  - additional instability indicators: `reason201_per_min=21.2`, `buf0_events_per_min=33.2`
- Output-quality gate status in final tuned soak: `not_evaluated` (no fresh capture artifact in that run)

Supporting baseline quality evidence:

- `docs/quality/reports/20260329-fresh-multiout-baseline/demo-quality-gate-summary.md`
- `docs/quality/reports/20260329-fresh-multiout-baseline/output-quality.json`

## Go/No-Go recommendation and operator guidance

### Recommendation

- **NO-GO** for any demo claim of stable/stutter-free mesh audio performance.
- Candidate is frozen for controlled demonstration only, with explicit instability caveat.

### Primary risk notes

- Transport SLO remains out of bounds (join/underrun/loss), so live behavior can regress abruptly.
- Final tuned soak lacks fresh output-capture quality scoring (`output_quality` was `not_evaluated`).
- Existing baseline output-quality evidence is also FAIL (`docs/quality/reports/20260329-fresh-multiout-baseline/output-quality.json`).

### Operator guidance for tomorrow (risk-managed fallback)

1. Treat this as a **known-failing candidate**; do not describe as release-ready.
2. Keep an immediate fallback (recorded audio/video proof) prepared before live run.
3. If audible stutter or stream interruption appears, switch to fallback immediately.
4. If asked for objective status, cite the final verdict packet above and transport fail metrics.
5. Do not reconfigure profile macros on demo day; freeze remains tied to current `build.h` values and referenced evidence.

## Validation command references (for reproducibility)

- `pio test -e native`
- `pio run -e src`
- `pio run -e out`
