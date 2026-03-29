# Demo Quality Gate Summary

- Generated: 2026-03-29T04:47:38.003964Z
- Final verdict: **FAIL**

| Gate | Result | Notes |
| --- | --- | --- |
| HIL soak | PASS | none |
| Transport SLO | FAIL | failed_metrics=join_time_s, underruns_per_min, loss_pct |
| Output quality | FAIL | band=fail; error=output quality failures: dropout_ratio_pct, lag_ms_std, quality_score; output quality band=fail |

## Artifacts

- `hil_soak_summary_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/01-baseline-current/soak-summary.json`
- `output_quality_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/01-baseline-current/output-quality.json`
- `transport_metrics_json`: `/Users/justin/Library/CloudStorage/Dropbox/NYU/semester-3-2025-fall/project-development-studio/meshnet-audio/docs/quality/reports/20260329-tonight/01-baseline-current/transport-metrics.json`
