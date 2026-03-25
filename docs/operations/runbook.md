# SoundMesh Pilot Operations Runbook

This runbook is the active operator reference for two-node pilot operation (`SRC` + `OUT`).

## Scope and ownership

- Scope: preflight, canary rollout, abort criteria, rollback/reset, escalation.
- Roles:
  - Operator on shift: executes checklist and captures evidence.
  - Firmware owner: approves gate exceptions and recovery decisions.

## Preflight checklist (must pass before any upload)

1. Confirm working tree is clean or intentionally staged.
2. Run validation:
   - `pio test -e native`
   - `pio run -e src`
   - `pio run -e out`
   - `bash tools/preupload_gate.sh`
3. Confirm gate output includes:
   - `✓ ALL PRE-UPLOAD CRASH GATES PASSED`
   - no `[gate][fail]` lines
4. Confirm portal policy flags in `lib/config/include/config/build.h`:
   - `ENABLE_SRC_USB_PORTAL_NETWORK` set to intended rollout value
   - `ENABLE_OUT_USB_PORTAL_NETWORK` remains `0` unless explicitly approved
5. If either portal flag is `1`, confirm runtime evidence file is current:
   - `docs/operations/runtime-evidence/portal-enable-evidence.env`
   - `PORTAL_ENABLE_APPROVED=YES`
   - `PORTAL_SRC_FLAG` and `PORTAL_OUT_FLAG` match build flags

## Canary rollout flow (two-node pilot)

1. Flash SRC only:
   - `pio run -e src -t upload --upload-port <SRC_PORT>`
2. Boot SRC and verify:
   - no panic/reset loop on serial
   - mesh starts and waits for child
3. Flash OUT:
   - `pio run -e out -t upload --upload-port <OUT_PORT>`
4. Verify association and streaming:
   - SRC-first boot scenario
   - OUT-first boot scenario
   - simultaneous boot scenario
5. Run short soak:
   - `python tools/hil_soak_check.py --src-port <SRC_PORT> --out-port <OUT_PORT> --duration 120`
6. Run Stage 2 fault matrix when validating endurance:
   - nightly profile: `python tools/hil_fault_matrix.py --src-port <SRC_PORT> --out-port <OUT_PORT> --duration 21600 --schedule tools/fault_schedules/nightly-6h.json`
   - pre-release profile: `python tools/hil_fault_matrix.py --src-port <SRC_PORT> --out-port <OUT_PORT> --duration 86400 --schedule tools/fault_schedules/prerelease-24h.json`
   - confirm `docs/operations/runtime-evidence/fault-matrix/fault-matrix-summary.json` reports `PASS`
7. Record evidence artifacts under `docs/operations/runtime-evidence/`:
   - src/out serial logs
   - status smoke output
   - updated `portal-enable-evidence.env` values if portal is enabled
   - fault matrix summary JSON artifacts for endurance runs
8. For OTA canary, verify rollback confirmation log on first boot:
   - `OTA image confirmed valid; rollback canceled`

## Abort criteria (stop rollout immediately)

Abort and block further uploads if any of the following occur:

- `tools/preupload_gate.sh` fails.
- HIL soak result is `SOAK_RESULT=FAIL`.
- Fault matrix summary reports `FAIL` for baseline or scheduled-fault scenarios.
- Any panic signature appears (`Guru Meditation`, `Backtrace`, heap corruption, stack overflow).
- Reboot loop persists beyond initial USB reset window.
- Protected portal endpoints accept unauthenticated control actions.
- Stream continuity is unstable (repeated stream-loss/rejoin churn).

## Rollback and reset procedure

1. Stop deployment and keep one known-good device online.
2. Reflash known-good firmware to failing node:
   - `pio run -e src -t upload --upload-port <SRC_PORT>` or
   - `pio run -e out -t upload --upload-port <OUT_PORT>`
3. If portal-related issue is suspected:
   - set `ENABLE_SRC_USB_PORTAL_NETWORK=0`
   - set `ENABLE_OUT_USB_PORTAL_NETWORK=0`
   - rebuild and reflash both nodes
4. Verify recovery with:
   - `pio test -e native`
   - `pio run -e src && pio run -e out`
   - `bash tools/preupload_gate.sh`
5. Capture failure evidence before reopening rollout:
   - failing serial logs
   - gate output
   - exact commit hash

## OTA bad-image recovery verification

1. Keep one node on known-good firmware.
2. Attempt OTA canary on a single node.
3. If node fails validation or boot stability, treat as bad-image scenario:
   - stop rollout
   - reflash known-good firmware using USB
   - confirm mesh and stream recovery
4. Record outcome and gate logs before resuming OTA.

## Escalation ownership and handoff

Escalate to firmware owner when:

- abort criteria are met and root cause is not obvious,
- gate requires exception,
- repeated failures occur across more than one device.

Minimum escalation packet:

- commit SHA tested
- exact commands run
- gate output excerpt
- HIL soak summary
- fault matrix summary artifact (if endurance profile was run)
- SRC/OUT serial tail snippets with timestamps

## Operational notes

- Demo mode is explicit-only (`?demo=1`) and must not be used as proof of live readiness.
- Treat portal tokens and uplink credentials as sensitive; avoid copying raw values into logs.
- Upload SPIFFS assets after portal UI changes:
  - `pio run -e src -t uploadfs`
  - `pio run -e out -t uploadfs`
