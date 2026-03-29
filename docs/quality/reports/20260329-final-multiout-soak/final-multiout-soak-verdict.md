# Final Multi-OUT Soak Verdict

- Report ID: `stutter-run-final-multiout-soak`
- Mode: `artifact-replay-nonflash`
- Profile: `prefill-6-gain-safe-fec10` (`20260329-tonight-05-prefill-6-gain-safe-fec10`)
- Final verdict: **FAIL**
- Demo readiness: **FAIL**

## Gate Summary

- HIL soak: PASS
- Transport SLO: FAIL
- Output quality: NOT_EVALUATED (output-quality.json absent for tuned profile run; no fresh capture artifact available without hardware interaction.)

## Key Numbers

- join_time_s: None
- underruns_per_min: 16.6
- loss_pct: 19.9
- reason201_per_min: 21.2
- buf0_events_per_min: 33.2
- SRC/OUT panic_hits: 0 / 0
- multi-OUT descendants max: 3

## Failed Criteria

- transport failed metrics: join_time_s, underruns_per_min, loss_pct

## Confidence & Limitations

- Confidence: **medium**
- No fresh OUT capture WAV for tuned profile run, so output-quality gate could not be re-evaluated.
- Transport join_time_s is null (no join markers in captured window), which is treated as criteria failure.
