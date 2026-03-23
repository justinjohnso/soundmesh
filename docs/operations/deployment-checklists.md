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
- no flash-size mismatch warning in build output
- phased portal rollout locks:
  - `ENABLE_SRC_USB_PORTAL_NETWORK` must be set (`0|1`)
  - `ENABLE_OUT_USB_PORTAL_NETWORK` must be set (`0|1`) and remains `0` in current phase
  - RX must keep `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
  - RX must not call `portal_init()` while OUT flag is blocked

## Current rollout phase

- **Phase:** SRC/COMBO-only portal enabled
- `ENABLE_SRC_USB_PORTAL_NETWORK=1`
- `ENABLE_OUT_USB_PORTAL_NETWORK=0`

## Upload targets for current session

- SRC: `/dev/cu.usbmodem101`
- OUT: `/dev/cu.usbmodem1101`
