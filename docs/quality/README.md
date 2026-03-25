# Quality Docs

This directory is for active quality and validation policy:

- `benchmarks-and-slos.md`
- `contracts/portal-observability-contract.md`
- `reports/release-candidate-benchmark-template.md`

Keep this focused on current validation standards and release criteria.

## Benchmark workflow

1. Capture SRC and OUT serial logs during a benchmark run.
2. Extract metrics:
   `python tools/benchmarks/extract_metrics.py --src-log <src.log> --out-log <out.log> --duration-seconds <N> --output docs/quality/reports/<candidate>-metrics.json`
3. Render the SLO report:
   `python tools/benchmarks/render_report.py --input docs/quality/reports/<candidate>-metrics.json --output docs/quality/reports/<candidate>-benchmark-report.md`
4. Attach both files to the release-candidate PR.

## Stage 2 soak and fault matrix workflow

Run baseline and scheduled-fault endurance checks with machine-readable summaries:

```bash
python tools/hil_fault_matrix.py \
  --src-port <SRC_PORT> \
  --out-port <OUT_PORT> \
  --duration 21600 \
  --schedule tools/fault_schedules/nightly-6h.json
```

```bash
python tools/hil_fault_matrix.py \
  --src-port <SRC_PORT> \
  --out-port <OUT_PORT> \
  --duration 86400 \
  --schedule tools/fault_schedules/prerelease-24h.json
```

Artifacts are written to:

- `docs/operations/runtime-evidence/fault-matrix/fault-matrix-summary.json`
- per-case `*-summary.json` files in the same directory.
