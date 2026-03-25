# Benchmarks and SLOs

This document defines the release benchmark procedure and thresholds for the SRC/OUT two-node pilot topology.

## Scope

- Topology: `1x SRC (root)` + `1x OUT`
- Firmware: current branch candidate for both roles
- Duration:
  - smoke: 10 minutes
  - release candidate: 60 minutes minimum
- Transport: mesh audio stream with Opus pipeline enabled

## Metrics

- `join_time_s`: seconds from boot to first `Network ready`
- `rejoin_time_s`: seconds from forced disconnect to recovered `Network ready`
- `stream_continuity_pct`: `(stream_active_seconds / total_seconds) * 100`
- `underruns_per_min`: playback underruns divided by run minutes
- `decode_failures_per_min`: Opus decode failures divided by run minutes
- `loss_pct`: packet loss percentage from OUT runtime stats

## SLO thresholds

- `join_time_s` <= 20
- `rejoin_time_s` <= 30
- `stream_continuity_pct` >= 99.0
- `underruns_per_min` <= 1.0
- `decode_failures_per_min` <= 0.5
- `loss_pct` <= 2.0

## Capture procedure

1. Build and flash candidate firmware to SRC and OUT.
2. Start serial logs for both nodes.
3. Run `python tools/benchmarks/extract_metrics.py --src-log <src.log> --out-log <out.log> --duration-seconds <N> --output docs/quality/reports/<candidate>-metrics.json`.
4. Save resulting JSON under `docs/quality/reports/`.
5. Generate markdown summary with `python tools/benchmarks/render_report.py --input docs/quality/reports/<candidate>-metrics.json --output docs/quality/reports/<candidate>-benchmark-report.md`.
6. Attach both artifacts to the release candidate PR.

### Timestamp parsing assumptions

- `extract_metrics.py` expects ESP-IDF-style log prefixes (for example, `I (12345) ...`).
- `join_time_s` uses the earliest timestamp where `Network ready` appears in each role log and takes the larger of SRC/OUT first-ready values.
- `rejoin_time_s` uses the first OUT rejoin trigger line followed by the next OUT `Network ready` line. If no complete rejoin/recovery pair is found, the metric remains `null`.

## Pass/fail policy

A release candidate passes when all SLO thresholds are met. Any threshold breach requires investigation, issue tracking, and a new candidate run.
