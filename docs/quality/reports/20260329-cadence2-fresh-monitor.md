# 2026-03-29 Cadence2 Fresh Monitor

## Scope
Todo: `urgent-cadence2-monitor-fresh`

Analyzed freshest post-flash SRC/OUT logs from the latest run directory and quantified recurring stutter cadence using available markers.

## Freshest evidence set (post-flash)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/src-serial.log` (updated 2026-03-29 02:04)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/out-serial.log` (updated 2026-03-29 02:04)
- Flash evidence: `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/flash-out.log`
- Run metadata: `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/run-info.env`
- Aggregated metrics: `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/transport-metrics.json`

## Marker availability check
- `TX_POLICY` logs: **not present** in freshest SRC/OUT serial logs.
- `RX_POLICY rebuffer_trigger/resume_wait/resume_ready`: **not present** in freshest SRC/OUT serial logs.

Because those mandatory policy markers are absent, policy-interval extraction is blocked for this run.

## Extracted intervals from available events
### SRC `reason:201` cadence
From `src-serial.log`:
- Event count: 106
- Interval mean: **2.852 s**
- Interval median: **2.852 s**
- Interval range: **2.845–2.854 s**

### OUT `buf=0%` cadence
From `out-serial.log`:
- Event count: 83
- Interval mean: **3.610 s**
- Interval median: **3.000 s**
- Interval range: **1.000–7.000 s**

### OUT `underrun` cadence
From `out-serial.log`:
- Event count: 83
- Interval mean: **3.615 s**
- Interval median: **3.217 s**
- Interval range: **1.377–8.433 s**

## Periodicity finding
Periodic behavior persists in the freshest run:
- `reason:201` is highly periodic (~2.85 s).
- `buf=0%` and `underrun` recur quasi-periodically (~3.6 s mean), but with broader jitter.

## Strongest correlation to stutter cadence
Strongest observed alignment is between OUT underruns and the SRC `reason:201` cycle after timeline offset normalization:
- Nearest-event absolute delta (underrun ↔ reason:201): median **0.245 s**, mean **0.344 s**, p95 **0.698 s**.
- `buf=0%` also aligns, but weaker: median **0.443 s**, mean **0.489 s**, p95 **1.136 s**.

Interpretation: recurring roboty/stutter cadence aligns best with **SRC `reason:201` disconnect cycle**, with OUT underruns as the closest audible symptom.

## Blocker status
**Blocked** for full cadence2 requirement due to missing mandatory policy markers in freshest post-flash logs.

### Exact logs required to unblock
For the same run window (fresh post-flash), capture logs that include:
1. SRC serial with `TX_POLICY` mode/probe lines.
2. OUT serial with `RX_POLICY rebuffer_trigger`, `RX_POLICY resume_wait`, `RX_POLICY resume_ready` lines.
3. Concurrent OUT lines containing `buf=0%` and `underrun`.
4. Concurrent SRC lines containing `reason:201`.

Without (1) and (2), policy-specific interval extraction cannot be completed.
