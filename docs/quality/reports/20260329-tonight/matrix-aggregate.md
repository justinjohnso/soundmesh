# 20260329 Tonight Matrix Aggregate

- Generated: 2026-03-29T05:11:00.342136Z
- Profiles attempted: 5
- Profiles completed: 4
- Blocked: SRC flash required for prefill-6-gain-safe-fec10 (OPUS_EXPECTED_LOSS_PCT=10) but autonomous SRC flashing is disallowed by policy. Next command: pio run -e src -t upload --upload-port /dev/cu.usbmodem1234561

| order | profile_id | status | transport | failed_metrics | output_band | quality_score | lag_ms_std | verdict | ranking |
| ---: | --- | --- | --- | --- | --- | ---: | ---: | --- | ---: |
| 1 | baseline-current | completed | False | join_time_s, underruns_per_min, loss_pct | fail | 43.02 | 32.969 | FAIL | 1 |
| 2 | prefill-5 | completed | False | join_time_s, underruns_per_min, loss_pct | fail | 23.96 | 29.569 | FAIL | 2 |
| 3 | prefill-6 | completed | False | join_time_s, underruns_per_min, loss_pct | fail | 10.0 | 38.891 | FAIL | 4 |
| 4 | prefill-6-gain-safe | completed | False | join_time_s, underruns_per_min, loss_pct | fail | 20.45 | 29.095 | FAIL | 3 |
| 5 | prefill-6-gain-safe-fec10 | blocked | None |  | None | None | None | BLOCKED |  |
