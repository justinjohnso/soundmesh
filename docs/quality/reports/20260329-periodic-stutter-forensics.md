# Periodic stutter forensics (2026-03-29)

## Evidence used (freshest + urgent)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/out-serial.log` (latest non-empty OUT serial)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/src-serial.log` (latest non-empty SRC serial)
- `docs/quality/reports/20260329-urgent-stutter-live-monitor.md` (urgent monitor summary)
- Cross-check: `docs/quality/reports/20260329-tonight/01-baseline-current/out-serial.log` through `04-prefill-6-gain-safe/out-serial.log`
- Note: `20260329-tonight/06-baseline-rx-hardening/{src,out}-serial.log` are empty.

## Measured periodic intervals
From `05-prefill-6-gain-safe-fec10`:
- **Underrun events** (`Underrun #...`): 83 events, median interval **3.22s** (mean 3.62s).
- **Buffer-empty events** (`buf=0%`): 83 events, median interval **3.00s** (mean 3.61s).
- **Heartbeat/churn events** (`Heartbeat #... churn(...)`): 12 events, interval **25.00s**.
- **SRC churn disconnects** (`reason:201`): 106 events, interval **2.852s**.
- **Recovery marker** (`Playback prefilled`): 5 events, interval **66.44s** median; nearest `buf=0%` delta median **0.92s**.
- **Fallback mode transitions**: not present in tonight logs (no explicit fallback-transition markers).

## Correlation findings
Strongest periodic match to audible cycle:
1. **OUT starvation loop**: `buf=0%` and underruns recur at ~**3.0–3.6s** and align closely (underrun↔buf0 nearest delta median 0.74s).
2. **Transport degradation during stutter**: when `buf=0%`, telemetry shows much worse delivery shape:
   - Ping: **142ms** (buf0) vs **82ms** (buf>0)
   - RX bandwidth: **33 kbps** (buf0) vs **55 kbps** (buf>0)
   - Loss remains ~19% in both states (so burst/jitter/throughput collapse matters more than average loss).
3. **Heartbeat/churn mismatch**: 25s heartbeat cadence does **not** match ~3s artifact cycle.
4. **SRC reason:201 cadence (~2.85s)** is near-periodic and may inject burstiness, but timing is not phase-locked enough in these artifacts to prove direct one-to-one causation.

## Most likely root trigger(s)
Primary trigger: **receive-side starvation from periodic throughput/jitter collapse**, causing repeated drain-to-zero and underrun bursts every ~3s.
Contributing trigger: **ongoing SRC/root Wi-Fi churn (`reason:201`)** likely increases burstiness, worsening starvation susceptibility.

## Specific fix hypothesis (next implementation)
Implement a **burst-loss resilient RX control loop** on OUT:
1. Raise dynamic prefill target when consecutive low-throughput/high-latency windows are detected (e.g., if RX kbps <40 or ping >150ms for >=2s, bump prefill from 7→10–12 frames temporarily).
2. Add hysteresis hold before resuming playback after refill (prevent immediate re-drain).
3. Smooth producer/consumer mismatch with adaptive decode pacing tied to RX arrival variance.
4. In parallel, mitigate SRC `reason:201` churn (root Wi-Fi stability) to reduce periodic burst gaps.

Expected outcome to validate next run: elimination (or major reduction) of the ~3s `buf=0%`/underrun recurrence.
