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
- `loss_avg_pct`: average packet loss percentage across OUT runtime samples
- `loss_last_pct`: last packet loss percentage sample from OUT runtime stats
- `loss_p95_pct`: p95 packet loss percentage across OUT runtime samples
- `reason201_per_min`: SRC `reason:201` disconnect events per minute
- `buf0_events_per_min`: OUT `buf=0%` starvation events per minute
- `raw.tx_obs_send_failures`: max observed TX send failure count from `TX OBS` summaries
- `raw.tx_obs_queue_full`: max observed TX queue-full count from `TX OBS` summaries
- `raw.tx_obs_backpressure_level_max`: max observed TX backpressure level from `TX OBS` summaries
- `raw.rx_obs_gap_events`: max observed RX sequence-gap event count from `RX OBS` summaries
- `raw.rx_obs_gap_frames`: max observed RX sequence-gap dropped-frame count from `RX OBS` summaries
- `raw.rx_obs_decode_errors`: max observed RX decode-error count from `RX OBS` summaries
- `raw.rx_obs_underrun_rebuffers`: max observed RX rebuffer count from `RX OBS` summaries
- `raw.rx_obs_prefill_wait_ms`: max observed cumulative prefill wait (ms) from `RX OBS` summaries
- `raw.rx_obs_buffer_peak_pct`: max observed RX jitter-buffer fill percent peak from `RX OBS` summaries
- `raw.rx_net_duplicates`: max observed duplicate-frame count from `RX NET` summaries
- `raw.rx_net_ttl_expired`: max observed TTL-expired frame count from `RX NET` summaries
- `raw.rx_net_mesh_recv_errors`: max observed mesh receive error count from `RX NET` summaries
- `raw.rx_net_mesh_recv_empty`: max observed empty mesh packet count from `RX NET` summaries

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

- `extract_metrics.py` expects ESP-IDF-style log prefixes (for example, `I (12345) ...`) and strips ANSI control sequences before parsing.
- `extract_metrics.py` also recognizes periodic `TX OBS`, `RX OBS`, and `RX NET` summary lines and captures the highest observed counter values into `raw.*` additive fields.
- `join_time_s` uses the earliest timestamp from recognized join-ready lines (`Network ready`, `Designated root ready`, `Root ready`, `Parent connected, layer`, and related stream-ready variants) and takes the larger of SRC/OUT first-ready values when both exist.
- If one side has no join-ready line, `join_time_s` falls back to the side that does have one; if neither side has a parsed join-ready line, it remains `null`.
- `rejoin_time_s` uses the first OUT rejoin trigger line followed by the next OUT join-ready line. If no complete rejoin/recovery pair is found, the metric remains `null`.

## Pass/fail policy

A release candidate passes when all SLO thresholds are met. Any threshold breach requires investigation, issue tracking, and a new candidate run.
