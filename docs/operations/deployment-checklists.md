# Deployment Checklists

## Pre-upload crash-prevention checklist (mandatory)

Run:

```bash
bash tools/preupload_gate.sh
```

Uploads are blocked unless this passes.

The gate enforces:

- `pio test -e native`
- `pio run -e tx -e rx -e combo`
- build-artifact validation for every role (`firmware.elf`, `.map`, generated `sdkconfig.h`)
- generated metrics artifact: `.pio/build/preupload_gate_metrics.tsv`
- role-specific RAM thresholds (conservative fail-closed ceilings):
  - TX: <= 70% and <= 180000 bytes used
  - RX: <= 65% and <= 150000 bytes used
  - COMBO: <= 70% and <= 180000 bytes used
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
  - TX/COMBO portal init must remain compile-gated behind `ENABLE_SRC_USB_PORTAL_NETWORK`
  - RX must not call `portal_init()` while OUT flag is blocked
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

Recommended capture command:

```bash
python tools/hil_soak_check.py \
  --src-port /dev/cu.usbmodem101 \
  --out-port /dev/cu.usbmodem1101 \
  --duration 120
```

## Current rollout phase

- **Phase:** SRC/COMBO portal enabled with fail-closed runtime evidence
- `ENABLE_SRC_USB_PORTAL_NETWORK=1`
- `ENABLE_OUT_USB_PORTAL_NETWORK=0`
- Portal enablement/continuation requires approved runtime evidence markers.

## Upload targets for current session

- SRC: `/dev/cu.usbmodem101`
- OUT: `/dev/cu.usbmodem1101`
