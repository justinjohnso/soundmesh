# Send-Mode A/B Comparison (Artifact-Based)

- Generated: 2026-03-29T12:05:54.201919+00:00
- Todo: `stutter-run-send-mode-ab-tests`
- Method: Hardened artifact comparison for current GROUP mode + historical fallback-mode evidence where no new hardware A/B capture is available.

## Mode A: GROUP|NONBLOCK (current)

| profile | loss_pct | underruns/min | reason201/min | buf0/min | continuity_% | quality_score |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline-current | 22.4 | 10.0 | 21.0 | 27.6 | 100.0 | 43.02 |
| prefill-5 | 22.9 | 9.8 | 21.0 | 26.4 | 100.0 | 23.96 |
| prefill-6 | 30.0 | 9.8 | 21.0 | 32.4 | 100.0 | 10.00 |
| prefill-6-gain-safe | 24.1 | 9.0 | 21.0 | 30.0 | 100.0 | 20.45 |
| **aggregate** | **24.85** (min 22.4, max 30.0) | **9.65** | **21.00** | **29.10** | **100.0** | n/a |

New observability counters: TX OBS / RX OBS / RX NET markers are not present in these logs, so those counters are `null` (coverage gap, not true zero).

## Mode B: P2P|NONBLOCK (historical fallback evidence only)

| metric | value | confidence | source |
| --- | --- | --- | --- |
| loss_pct | best-node ~2–3%, worst-node >20% | low | `docs/history/progress-notes/mesh-reliability-fix.md` lines ~409-413 |
| underruns/min | n/a (not reported in comparable form) | low | historical note only |
| reason201/min | n/a (qualitative persistence only) | low | lines ~397, 404-417 |
| buf0/min | n/a (qualitative starvation only) | low | lines ~396, 411-412 |
| continuity_% | n/a | low | historical note only |

## Partial blocked variant (not a mode A/B datapoint)

- `05-prefill-6-gain-safe-fec10` log-derived only: loss=19.9, underruns/min=16.6, reason201/min=21.2, buf0/min=33.2.
- Excluded from recommendation because required artifacts are incomplete (no transport-metrics/output-quality/demo summary/capture).

## Recommendation for `stutter-apply-best-transport-mode`

- **Recommended mode:** `GROUP|NONBLOCK`.
- **Recommended settings profile:** `baseline-current` ({'JITTER_PREFILL_FRAMES': 4, 'RX_OUTPUT_VOLUME': '2.0f', 'OUT_OUTPUT_GAIN_DEFAULT_PCT': 200, 'OUT_OUTPUT_GAIN_MAX_PCT': 400, 'OPUS_EXPECTED_LOSS_PCT': 8}).
- **Why:** only mode with hardened artifact set in this environment; fallback-mode evidence is historical/qualitative and cannot support strict apples-to-apples promotion.
- **Confidence:** medium for current-mode characterization, low for true cross-mode A/B.
