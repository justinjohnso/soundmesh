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
- crash-recovery config locks:
  - `ENABLE_USB_PORTAL_NETWORK` must remain disabled during recovery mode
  - RX must keep `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`

## Upload targets for current session

- SRC: `/dev/cu.usbmodem101`
- OUT: `/dev/cu.usbmodem1101`
