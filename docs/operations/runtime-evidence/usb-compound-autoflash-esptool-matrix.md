# USB Compound Autoflash — Direct `esptool` Feasibility Matrix

## 1) Evidence metadata (required)

- Evidence UTC: `2026-04-11T19:32:21Z`
- Run ID: `usb-compound-autoflash-a2f3`
- Operator: `justin`
- Reviewer/approver: `justin`
- Git commit: `8b5dbcf+workspace-local`
- Firmware env/build: `SRC runtime (USB compound app mode active; exact image tag not reported in this probe)`

## 2) Host matrix (required)

| Host ID | OS + version | Python version | `esptool.py` version | USB topology notes |
| --- | --- | --- | --- | --- |
| `host-1` | `macOS 15.7.3 (Darwin 24.6.0)` | `Python 3.14.4` | `esptool.py v4.5.1` (PlatformIO package) | Direct USB-C to SRC; runtime CDC at `/dev/cu.usbmodem1234561` |

## 3) Device + port mapping (required)

| Device role | Device ID / serial | Runtime CDC port | ROM/download port (if different) | NCM interface | Notes |
| --- | --- | --- | --- | --- | --- |
| SRC | `SER=123456` (runtime CDC from `pio device list`); `80:B5:4E:C3:6D:C4` (hardware serial observed in earlier session logs for ROM path) | `/dev/cu.usbmodem1234561` | `/dev/cu.usbmodem211401` (observed/manual ROM-download upload path when present; inferred from prior session logs, not this runtime-only probe) | `en10` (`169.254.1.90`, observed candidate from session host interfaces) | Runtime CDC mapping is directly captured for this run; ROM-port/hardware-serial/NCM details are carried forward from observed session evidence and may vary with reconnect/order. |
| OUT | `n/a` | `n/a` | `n/a` | `n/a` | Out of scope for this task |

## 4) Command attempt log (required)

Record every direct `esptool` attempt (pass and fail).

| Attempt ID | Host ID | Device role | Port used | Pre-state (`app`/`download`) | Command | Result (`success`/`failure`) | Failure mode (if any) | Evidence snippet |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `A01` | `host-1` | `SRC` | `/dev/cu.usbmodem1234561` | `app` | `bash tools/usb_runtime_esptool_probe.sh --port /dev/cu.usbmodem1234561` | `failure` | `sync failed` | `A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.` |

## 5) Aggregated results (required)

- Total attempts: `1`
- Success count: `0`
- Failure count: `1`
- Success rate: `0%`
- Consecutive success streak (max): `0`

### Failure-mode counts

| Failure mode | Count | Notes |
| --- | --- | --- |
| timeout | `0` | |
| sync failed | `1` | Runtime CDC path accepted port open but no ROM sync response in app mode |
| missing port | `0` | |
| unexpected reset loop | `0` | |
| other (`tool-path precheck fix applied`) | `0` | Probe script updated to auto-resolve PlatformIO esptool path before attempt |

## 6) Final decision (required)

Decision is locked for this run (`usb-compound-autoflash-a2f3`) based on evidence above.
Current decision lock mapping: `8b5dbcf+workspace-local` (immutable base commit + uncommitted workspace review state).

- [ ] **Direct `esptool` viable** for no-manual-reset primary workflow.
- [x] **OTA-over-USB-NCM primary** (direct `esptool` not reliable enough).

Decision rationale (required):

- Runtime direct-esptool probe on the active SRC CDC path failed (`0/1` success).
- Observed failure mode was ROM sync/connect failure (`No serial data received`) while in app mode, matching non-viable no-button direct flashing.
- Therefore OTA over the existing USB-NCM portal path remains the only evidence-supported no-manual-reset primary workflow.

Approval:

- Decision owner: `justin`
- Approved UTC: `2026-04-11T19:32:21Z`
