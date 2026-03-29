# Quality Matrix Dry-Run (Synthetic Fixtures)

This folder contains a **dry-run** of the quality profile matrix workflow using archived calibration fixtures (not live hardware capture).

## Command used

```bash
python tools/quality/run_quality_matrix.py \
  --profiles docs/quality/reports/quality-matrix-dry-run-profiles.json \
  --skip-hooks \
  --thresholds tools/quality/output_quality_thresholds.json \
  --fail-on-band off \
  --output-dir docs/quality/reports/quality-matrix-dry-run
```

## Ranked result

1. `calibration-pass-fixture` — score `80.00`, band `pass`
2. `calibration-warn-fixture` — score `74.95`, band `warn`
3. `calibration-fail-fixture` — score `61.89`, band `fail`

## Provisional best profile logic

Selection is based on SLO-style gating:

- Prefer `overall_band = pass`
- Then rank by `quality_score` descending
- Break ties by lower dropout ratio, lower lag jitter, then lower spectral deviation

Under this policy, `calibration-pass-fixture` is the provisional best profile.

## Sign-off caveat

Hardware validation is still required for release sign-off. This dry-run only validates matrix orchestration and threshold classification behavior.
