# Output Quality Method

This document defines host-side audio quality capture and scoring for OUT-node validation.

## Tooling

- `tools/quality/capture_out.py`
- `tools/quality/score_output_quality.py`
- `tools/quality/run_quality_matrix.py`

## 1) Capture OUT audio

Use host audio loopback/interface capture to write a WAV:

```bash
python tools/quality/capture_out.py \
  --device "2" \
  --duration 30 \
  --sample-rate 48000 \
  --channels 1 \
  --output data/quality/captured/out-run.wav \
  --write-metadata
```

Notes:

- Backend priority: `ffmpeg` first, `sox` fallback.
- If neither is available, the command fails with explicit install guidance.
- Metadata JSON is optional:
  - explicit path: `--metadata-json <path>`
  - default sibling file: `--write-metadata`

## 2) Full-reference scoring

Compare a known-good reference WAV with a captured WAV:

```bash
python tools/quality/score_output_quality.py \
  --reference data/quality/reference.wav \
  --captured data/quality/captured/out-run.wav \
  --sample-rate 48000 \
  --output docs/quality/reports/out-run-quality.json
```

### Metrics

#### dropout_ratio_pct

Frame-level dropout ratio using 20 ms windows:

- Compute RMS per window for reference and captured.
- `reference_active` if `rms_ref >= max(1e-4, 0.1 * max_rms_ref)`.
- `captured_silent` if `rms_cap < 2e-3`.
- `dropout_ratio_pct = 100 * count(reference_active AND captured_silent) / count(reference_active)`.

Interpretation:

- Lower is better.
- Near-zero means active reference segments are present in captured output.

#### lag_ms_mean and lag_ms_std

Sliding normalized cross-correlation over active windows (100 ms frame, 50 ms hop, ±120 ms search):

- Per frame, compute lag at peak normalized correlation.
- Report:
  - `lag_ms_mean`: average lag (signed)
  - `lag_ms_std`: jitter in lag estimates

Interpretation:

- Mean near 0 and low std indicate stable timing.
- High std indicates jitter/instability.

#### spectral_deviation_db

Average absolute STFT magnitude distance in dB:

- STFT config: `n_fft=1024`, `hop=256`, Hann window.
- Convert magnitudes to dB (`20*log10(mag + eps)`).
- Compute mean `abs(db_captured - db_reference)` over aligned frames/bins.

Interpretation:

- Lower is better.
- Higher values indicate tonal/timbre distortion or codec/packet artifacts.

#### alignment_offset (optional)

The scorer estimates a global alignment offset by cross-correlation before metric extraction (disabled with `--no-alignment`).

## 3) Calibrated threshold bands (fail / warn / pass)

Threshold definitions are stored in:

- `tools/quality/output_quality_thresholds.json`

The scorer automatically loads this file (or a custom file passed by `--thresholds`) and emits:

- `threshold_evaluation.overall_band` (`fail`, `warn`, `pass`)
- `threshold_evaluation.metric_bands`
- `threshold_evaluation.failed_metrics`
- `threshold_evaluation.warned_metrics`

Current practical demo bands:

| Metric | Pass | Warn | Fail |
|---|---:|---:|---:|
| `dropout_ratio_pct` | `<= 1.0` | `<= 6.0` | `> 6.0` |
| `lag_ms_std` | `<= 12.0` | `<= 25.0` | `> 25.0` |
| `spectral_deviation_db` | `<= 38.0` | `<= 46.0` | `> 46.0` |
| `quality_score` | `>= 78.0` | `>= 68.0` | `< 68.0` |

Band intent:

- **fail**: robotic/stuttery and not demo safe.
- **warn**: borderline; requires reviewer judgment and correlated log evidence.
- **pass**: demo acceptable in two-node pilot conditions.

### Calibration method used for initial bands

1. Generate a clean synthetic mono fixture (`reference.wav`) with mixed tones and slow envelope.
2. Generate three degraded captures:
   - `captured-pass.wav`: light noise only
   - `captured-warn.wav`: moderate 20 ms dropouts + mild noise
   - `captured-fail.wav`: heavy dropouts + stutter/quantization artifacts
3. Score all three with `score_output_quality.py` using threshold evaluation enabled.
4. Tune breakpoints so the synthetic profiles classify as pass/warn/fail respectively.
5. Archive artifacts under `docs/quality/reports/`.

Artifacts produced:

- `docs/quality/reports/output-quality-threshold-calibration-synthetic.json`
- `docs/quality/reports/calibration-pass-quality.json`
- `docs/quality/reports/calibration-warn-quality.json`
- `docs/quality/reports/calibration-fail-quality.json`
- `docs/quality/reports/calibration-fixtures/*.wav`

This is an initial host-side calibration and must be refined with real hardware captures before release sign-off.

## 4) Aggregate quality score (0-100)

`quality_score` uses weighted components:

- Dropout component (45%): `max(0, 100 - 2.5 * dropout_ratio_pct)`
- Lag component (35%): `max(0, 100 - 2.0 * (abs(lag_ms_mean) + lag_ms_std))`
- Spectral component (20%): `max(0, 100 - 5.0 * spectral_deviation_db)`

Final score:

```text
quality_score =
  0.45 * dropout_component +
  0.35 * lag_component +
  0.20 * spectral_component
```

Score is clamped to `[0, 100]`.

## 5) Matrix orchestration

Run profile matrix with built-in defaults or custom profile JSON:

```bash
python tools/quality/run_quality_matrix.py --skip-hooks
```

To enforce calibrated bands in matrix execution:

```bash
python tools/quality/run_quality_matrix.py \
  --skip-hooks \
  --thresholds tools/quality/output_quality_thresholds.json \
  --fail-on-band fail
```

`--fail-on-band` behavior:

- `fail` (default): non-zero exit if any profile is `fail`
- `warn`: non-zero exit if any profile is `warn` or `fail`
- `off`: disable threshold band gating

Custom profile file:

```json
{
  "profiles": [
    {
      "name": "baseline",
      "reference_wav": "data/quality/reference.wav",
      "captured_wav": "data/quality/captured-baseline.wav",
      "hooks": {
        "pre": ["echo pre"],
        "capture": ["echo capture"],
        "post": ["echo post"]
      }
    }
  ]
}
```

Run with profile JSON:

```bash
python tools/quality/run_quality_matrix.py \
  --profiles docs/quality/reports/example-quality-profiles.json
```

Outputs:

- `docs/quality/reports/quality-matrix-results.json`
- `docs/quality/reports/quality-matrix-results.csv`
- Per-profile `*-quality.json`

### Real hardware matrix run (exact sequence)

Use this sequence when USB devices and host audio capture are available:

```bash
# 1) Capture OUT audio for each candidate profile
python tools/quality/capture_out.py \
  --device "<HOST_AUDIO_DEVICE_ID>" \
  --duration 30 \
  --sample-rate 48000 \
  --channels 1 \
  --output data/quality/captured/<profile-name>.wav \
  --write-metadata

# 2) Create/update profile matrix JSON with captured WAVs
#    (example path)
#    docs/quality/reports/<run-id>-profiles.json

# 3) Run matrix with calibrated thresholds and fail gating
python tools/quality/run_quality_matrix.py \
  --profiles docs/quality/reports/<run-id>-profiles.json \
  --thresholds tools/quality/output_quality_thresholds.json \
  --fail-on-band fail \
  --output-dir docs/quality/reports/<run-id>
```

Recommended release gate policy:

- Keep `--fail-on-band fail` for normal RC gating.
- Use `--fail-on-band warn` only when policy requires zero-warning acceptance.
- Reserve `--fail-on-band off` for dry-runs/tooling validation only.

## Integration with benchmark extraction + SLO gate

Use both host and mesh metrics in candidate gates:

1. Existing network/continuity extraction:
   - `python tools/benchmarks/extract_metrics.py ... --output <mesh-metrics.json>`
2. Existing SLO rendering/check workflow:
   - `python tools/benchmarks/render_report.py ...`
3. New output-quality scoring:
   - `python tools/quality/score_output_quality.py ... --output <quality-metrics.json>`
4. Gate recommendation for release candidate:
   - Require **all benchmark SLOs** from `extract_metrics.py` to pass.
   - Require **quality threshold band** to be `pass` (or explicitly accept `warn` with rationale).
   - Archive both JSON artifacts under `docs/quality/reports/` for PR review.

This keeps transport health and audible output quality coupled in one release decision path.
