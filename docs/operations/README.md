# Operations Docs

This directory is for active operator-facing documentation:

- `runbook.md`
- `deployment-checklists.md`
- `troubleshooting.md`

If a document is no longer an active operator reference, move it to `docs/history/`.

Current active sequence:

1. `deployment-checklists.md` for pre-upload gate criteria.
2. `runbook.md` for rollout/abort/rollback/escalation flow.
3. `troubleshooting.md` for incident triage and common failure paths.

Runtime evidence artifacts:

- `runtime-evidence/portal-enable-evidence.env` for portal gate approvals.
- `runtime-evidence/fault-matrix/` for Stage 2 baseline and scheduled fault-run summaries.
