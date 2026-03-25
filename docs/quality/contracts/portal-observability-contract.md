# Portal Observability Contract

This document defines compatibility expectations for portal observability payloads used by UI and operator tooling.

## Scope

- `GET /api/status`
- `GET /api/control/metrics`
- `GET /api/uplink`
- `GET /api/mixer`
- `GET /api/ota`
- `GET /ws` payload shape (same core structure as `/api/status`)

## Schema versioning policy

`/api/status` includes `schemaVersion` from `PORTAL_STATUS_SCHEMA_VERSION`.
`/api/control/metrics` includes `schemaVersion` from `PORTAL_CONTROL_METRICS_SCHEMA_VERSION`.

Rules:

1. Additive fields (new optional keys, new nested objects) are allowed without schema version bump.
2. Breaking changes require bumping `PORTAL_STATUS_SCHEMA_VERSION`, including:
   - field removal
   - type changes
   - semantic reinterpretation of existing keys
3. During migration, firmware and UI must support at least one previous schema where feasible.

## Compatibility guarantees

- Existing keys validated by native contract tests remain stable within the same `schemaVersion`.
- Sensitive values remain redacted:
  - uplink `ssid` must be `"<configured>"` when configured.
  - OTA `lastUrl` must be `"<configured>"` when present.
- Control metrics include auth/rate-limit/retry counters for incident triage:
  - `schemaVersion`
  - `authRejects`, `otaRejects`, `uplinkRejects`, `mixerRejects`
  - `otaRequests`, `uplinkRequests`, `mixerRequests`
  - `otaApplyFails`, `uplinkApplyFails`, `mixerApplyFails`
  - `badRequests`
  - `rateLimit.windowMs`, `rateLimit.maxRequests`
- Mixer state is additive in status/WS payloads:
  - `mixer.outGainPct` (0-400)
  - `mixer.applied`, `mixer.pendingApply`
  - `mixer.lastError`, `mixer.updatedMs`

## Validation requirements

Before merge, run:

- `pio test -e native`
- `pio run -e src && pio run -e out`
- `bash tools/preupload_gate.sh`

Contract changes must include:

- updates to `test/native/test_portal_api_contract/main.c` for any schema delta
- docs update in this file when compatibility rules or endpoint behavior changes
