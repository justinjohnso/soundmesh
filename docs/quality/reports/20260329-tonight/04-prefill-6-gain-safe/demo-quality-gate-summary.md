# Demo Quality Gate Summary

- Generated: 2026-03-29T05:10:55.242242Z
- Final verdict: **FAIL**

| Gate | Result | Notes |
| --- | --- | --- |
| HIL soak | PASS | none |
| Transport SLO | FAIL | failed_metrics=join_time_s, underruns_per_min, loss_pct |
| Output quality | FAIL | band=fail; error=output quality failures: dropout_ratio_pct, lag_ms_std, quality_score; output quality band=fail |

## Artifacts

- `hil_soak_summary_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/04-prefill-6-gain-safe/soak-summary.json`
- `output_quality_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/04-prefill-6-gain-safe/output-quality.json`
- `transport_metrics_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/04-prefill-6-gain-safe/transport-metrics.json`
