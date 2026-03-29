# Quality Docs

This directory is for active quality and validation policy:

- `benchmarks-and-slos.md`
- `contracts/portal-observability-contract.md`
- `reports/release-candidate-benchmark-template.md`
- `output-quality-method.md`

Keep this focused on current validation standards and release criteria.

## Benchmark workflow

1. Capture SRC and OUT serial logs during a benchmark run.
2. Extract metrics:
   `python tools/benchmarks/extract_metrics.py --src-log <src.log> --out-log <out.log> --duration-seconds <N> --output docs/quality/reports/<candidate>-metrics.json`
3. Render the SLO report:
   `python tools/benchmarks/render_report.py --input docs/quality/reports/<candidate>-metrics.json --output docs/quality/reports/<candidate>-benchmark-report.md`
4. Attach both files to the release-candidate PR.

## Demo-quality gate runner

Run the consolidated gate to evaluate soak stability, transport SLOs, and output quality in one command:

```bash
python tools/quality/run_demo_quality_gate.py \
  --src-port <SRC_PORT> \
  --out-port <OUT_PORT> \
  --src-log <src.log> \
  --out-log <out.log> \
  --capture-wav <captured.wav> \
  --reference-wav <reference.wav> \
  --duration 300 \
  --transport-node-metrics <optional-node-metrics.json> \
  --output-dir docs/quality/reports/<candidate-id> \
  --markdown-output docs/quality/reports/<candidate-id>/demo-quality-gate-summary.md
```

Outputs:

- `demo-quality-gate-summary.json` (machine-readable per-gate + final verdict)
- optional markdown summary (when `--markdown-output` is provided)
- intermediate `hil-soak-summary.json`, `transport-metrics.json`, and `output-quality.json`

Gate behavior:

- The gate hard-fails when required JSON artifacts are missing, empty, or malformed, even when the producer command exits `0`.
- Transport sanity checks validate raw metadata consistency (line counts, join-time source, event-count consistency) when those fields are present.
- If `--transport-node-metrics` is provided, transport verdicts are based on the worst candidate (aggregate + per-node) so multi-node regressions are not masked by aggregate-only metrics.

## Tonight profile matrix workflow

Use `docs/quality/reports/20260329-tonight-profiles.json` as the run plan for the tuning sweep.
Current demo default selection from `20260329-send-mode-ab`:

- transport mode: `GROUP|NONBLOCK`
- settings profile: `baseline-current`
- baseline macro set: `JITTER_PREFILL_FRAMES=4`, `RX_OUTPUT_VOLUME=2.0f`,
  `OUT_OUTPUT_GAIN_DEFAULT_PCT=200`, `OUT_OUTPUT_GAIN_MAX_PCT=400`,
  `OPUS_EXPECTED_LOSS_PCT=10`, `RX_PLC_MAX_FRAMES_PER_GAP=2`,
  `RX_DECODE_MAX_ITEMS_PER_CYCLE=2`, `RX_PCM_HIGH_WATER_FRAMES=PCM_BUFFER_FRAMES-2`,
  `RX_UNDERRUN_CONCEAL_FRAMES=3`, `RX_UNDERRUN_REBUFFER_MISSES=8`

Per profile:

1. Apply `macro_overrides` in `lib/config/include/config/build.h`.
2. Build/flash, then run:
   `python tools/quality/run_demo_quality_gate.py --src-port <SRC_PORT> --out-port <OUT_PORT> --src-log <run-dir>/src-serial.log --out-log <run-dir>/out-serial.log --capture-wav <run-dir>/out-capture.wav --reference-wav docs/quality/reports/calibration-fixtures/reference.wav --duration 300 --output-dir <run-dir> --markdown-output <run-dir>/demo-quality-gate-summary.md`
3. If capture is done separately, use:
   `python tools/quality/capture_out.py --device "<HOST_AUDIO_DEVICE_ID>" --duration 30 --sample-rate 48000 --channels 1 --output <run-dir>/out-capture.wav --write-metadata`
4. Refresh transport metrics when needed:
   `python tools/benchmarks/extract_metrics.py --src-log <run-dir>/src-serial.log --out-log <run-dir>/out-serial.log --duration-seconds 300 --output <run-dir>/transport-metrics.json`

Follow `run_dir` and `per_profile_required_artifacts` in the matrix JSON exactly so results stay comparable across profiles.

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
