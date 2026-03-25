# SoundMesh Troubleshooting (SRC/OUT)

Active troubleshooting guide for pilot operation.

## Quick triage order

1. Run `bash tools/preupload_gate.sh`.
2. Check serial logs for panic/reset signatures.
3. Confirm mesh association state.
4. Confirm stream state transitions (`Waiting` → `Streaming`).
5. Validate portal auth behavior on protected endpoints.

## Common symptoms and actions

## OUT never starts streaming

Checks:

- Confirm SRC is running and mesh-ready.
- Confirm OUT reaches `Mesh Ready` then `Waiting`.
- Verify OUT logs receive packet activity (`OUT packet`, playback logs).

Actions:

- Verify both nodes are on matching firmware generation.
- Reboot OUT first, then SRC (and test inverse order).
- If prolonged `Waiting` persists, inspect rejoin behavior and mesh link quality.

## Repeated stream-loss and reconnect churn

Checks:

- Inspect OUT state transitions around `Stream Lost` and rejoin warnings.
- Confirm circuit-breaker cooldown is being honored.

Actions:

- Reduce RF contention and ensure close-range SRC transmit power settings are unchanged.
- Verify no stale constants are bypassing `network_rejoin_allowed()`.
- Re-run native mesh query tests and gate.

## Portal control actions return 401 unexpectedly

Checks:

- Confirm token matches `PORTAL_CONTROL_AUTH_TOKEN` in `build.h`.
- Ensure request uses either:
  - `Authorization: Bearer <token>`
  - `X-SoundMesh-Token: <token>`
- For WS control, confirm `?token=` is present.

Actions:

- Retry with explicit header/query token.
- Verify portal UI query params include `token`.
- Confirm endpoint is one of protected control paths.

## Portal control actions return 429 rate_limited

Checks:

- Poll control telemetry:
  - `curl -s http://10.48.0.1/api/control/metrics`
- Confirm `rateLimit.windowMs` and `rateLimit.maxRequests`.
- Check whether `otaRejects` or `uplinkRejects` counters are rising quickly.
- Check request/error counters:
  - `otaRequests`, `uplinkRequests`
  - `otaApplyFails`, `uplinkApplyFails`
  - `badRequests`

Actions:

- Reduce request burst rate from automation/UI retries.
- Add client-side backoff and retry jitter for control POST calls.
- If persistent under normal usage, adjust limits in `PORTAL_CONTROL_RATE_LIMIT_*` and revalidate.
- If `badRequests` climbs, inspect payload formatting and field types in callers.

## Portal appears live but no device data updates

Checks:

- Confirm not in explicit demo mode (`?demo=1`).
- Check WS connection status in UI and serial.
- Confirm `/api/status` responds with live SRC/OUT state.

Actions:

- Remove demo query flag and reconnect.
- Verify USB network interface is active on SRC.
- Restart portal session and retest WS.

## OTA accepted but no update progress

Checks:

- Confirm OTA URL is HTTPS and reachable by root.
- Confirm `/api/ota` status changes after submission.

Actions:

- Retry with known-good HTTPS URL.
- Check serial for OTA start and error logs.
- If uncertain, rollback to known-good firmware and pause rollout.

## Gate fails on runtime evidence

Checks:

- Open `docs/operations/runtime-evidence/portal-enable-evidence.env`.
- Confirm marker names are `SRC_*` / `OUT_*` and values are numeric where required.
- Confirm flags in evidence match build flags.

Actions:

- Regenerate evidence via current HIL run.
- Update timestamp and commit.
- Re-run gate.

## Panic or reset loop observed

Immediate actions:

1. Stop rollout and preserve logs.
2. Reflash known-good firmware to affected node.
3. Disable portal flags if issue is control-plane correlated.
4. Escalate with evidence packet from runbook.

## Fault matrix run reports failure

Checks:

- Open `docs/operations/runtime-evidence/fault-matrix/fault-matrix-summary.json`.
- Identify which case failed (`baseline` or scheduled profile).
- Inspect per-case `*-summary.json` for `panic_hits` and `late_reset_hits`.

Actions:

- Reproduce with the same schedule and shorter duration first.
- Validate serial stability and USB cable/power integrity before firmware changes.
- If failures reproduce, treat as rollout blocker and escalate with summary artifacts.
