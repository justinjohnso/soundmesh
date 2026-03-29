# Deployment Checklists

## Pre-upload crash-prevention checklist (mandatory)

Run:

```bash
bash tools/preupload_gate.sh
```

Uploads are blocked unless this passes.

The gate enforces:

- `pio test -e native`
- `pio run -e src -e out`
- build-artifact validation for every role (`firmware.elf`, `.map`, generated `sdkconfig.h`)
- generated metrics artifact: `.pio/build/preupload_gate_metrics.tsv`
- role-specific RAM thresholds (conservative fail-closed ceilings):
  - SRC: <= 70% and <= 180000 bytes used
  - OUT: <= 65% and <= 150000 bytes used
- OTA partition and rollback safety:
  - partition table includes `otadata`, `ota_0`, and `ota_1`
  - `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`
  - `CONFIG_APP_ROLLBACK_ENABLE=1`
- stack/heap risk floors from build config:
  - `PORTAL_HTTP_STACK_BYTES >= 6144`
  - `PORTAL_WS_PUSH_STACK_BYTES >= 4096`
  - `PORTAL_DNS_STACK_BYTES >= 3072`
  - `PORTAL_MIN_FREE_HEAP >= 30720`
  - audio/network task stack minimums for capture/encode/decode/playback/mesh_rx/heartbeat (encode floor: 24576 bytes)
- runtime safety config assertions from generated `sdkconfig.h` per env:
  - `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1`
  - `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=1`
- phased portal rollout locks:
  - `ENABLE_SRC_USB_PORTAL_NETWORK` and `ENABLE_OUT_USB_PORTAL_NETWORK` must be `0|1`
  - OUT portal cannot be enabled while SRC portal is disabled
  - SRC portal init must remain compile-gated behind `ENABLE_SRC_USB_PORTAL_NETWORK`
  - OUT must not call `portal_init()` while OUT flag is blocked
- fail-closed portal enable policy:
  - if either portal flag is `1`, gate requires runtime evidence markers in
    `docs/operations/runtime-evidence/portal-enable-evidence.env`
  - evidence must be recent, approved, and include minimum heap/stack/status-smoke metrics
  - evidence must include HIL soak metrics proving no panic and no late reset loops
  - gate requires presence of `tools/hil_soak_check.py` for reproducible soak validation
- historical crash-signature checks are preserved:
  - block `esp_mesh_waive_root(`
  - block `ESP_ERROR_CHECK(es8388_audio_init`

### Runtime evidence file for portal enablement

Template:

`docs/operations/runtime-evidence/portal-enable-evidence.env.example`

To request enabling portal flags, copy template to:

`docs/operations/runtime-evidence/portal-enable-evidence.env`

Then provide measured values from latest HIL smoke run.

Recommended capture command (5 minutes / 300 seconds):

```bash
python tools/hil_soak_check.py \
  --src-port /dev/cu.usbmodem101 \
  --out-port /dev/cu.usbmodem1101 \
  --duration 300
```

Use the SRC monitor port for `--src-port` and verify serial output stays healthy through the full 5-minute soak.

## Current rollout phase

- **Phase:** SRC portal enabled with fail-closed runtime evidence
- `ENABLE_SRC_USB_PORTAL_NETWORK=1`
- `ENABLE_OUT_USB_PORTAL_NETWORK=0`
- Portal enablement/continuation requires approved runtime evidence markers.

## OTA rollback verification checklist

- Trigger OTA to one canary node only.
- After first successful boot on new image, confirm serial includes:
  - `OTA image confirmed valid; rollback canceled`
- Validate node remains stable for at least one short soak window.
- If canary fails boot validation, stop rollout and reflash known-good image.

## Stage 2 soak and fault-injection checklist

- Nightly endurance soak (6h):
  - `python tools/hil_fault_matrix.py --src-port <SRC_PORT> --out-port <OUT_PORT> --duration 21600 --schedule tools/fault_schedules/nightly-6h.json`
- Pre-release endurance soak (24h):
  - `python tools/hil_fault_matrix.py --src-port <SRC_PORT> --out-port <OUT_PORT> --duration 86400 --schedule tools/fault_schedules/prerelease-24h.json`
- Required artifacts:
  - `docs/operations/runtime-evidence/fault-matrix/fault-matrix-summary.json`
  - Per-case summary JSON files in the same directory.
- Pass criteria:
  - Matrix result is `PASS`.
  - No panic hits and no late reset hits in SRC/OUT summaries.
  - Baseline and scheduled-fault cases both report `PASS`.

## Upload targets for current session

- SRC: `/dev/cu.usbmodem101`
- OUT: `/dev/cu.usbmodem1101`
