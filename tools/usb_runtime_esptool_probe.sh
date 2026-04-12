#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "[probe][fail] $*"
  echo "[probe][evidence][fail] result=FAIL"
  exit 1
}

pass() {
  echo "[probe][pass] $*"
  echo "[probe][evidence][pass] result=PASS"
}

usage() {
  cat <<'EOF'
Usage:
  bash tools/usb_runtime_esptool_probe.sh [options]

Probe whether direct esptool access is viable while SRC is running on the runtime USB CDC path.

Options:
  --port <path>       USB CDC serial port (example: /dev/cu.usbmodem12301)
  --chip <name>       esptool chip name (default: esp32s3)
  --baud <baud>       esptool baud rate (default: 460800)
  --esptool <cmd>     esptool executable (default: esptool.py)
  --help              Show this help

Behavior:
  - Non-destructive probe only (uses esptool read_mac).
  - Does NOT erase, write, or modify flash.
EOF
}

PORT=""
CHIP="esp32s3"
BAUD="460800"
ESPTOOL_BIN="esptool.py"

resolve_esptool_bin() {
  if command -v "$ESPTOOL_BIN" >/dev/null 2>&1; then
    return 0
  fi

  if [[ "$ESPTOOL_BIN" == "esptool.py" ]]; then
    if command -v esptool >/dev/null 2>&1; then
      ESPTOOL_BIN="esptool"
      return 0
    fi
    if [[ -x "${HOME}/.platformio/packages/tool-esptoolpy/esptool.py" ]]; then
      ESPTOOL_BIN="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"
      return 0
    fi
  fi

  fail "esptool not found: ${ESPTOOL_BIN}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      [[ $# -ge 2 ]] || fail "Missing value for --port"
      PORT="$2"
      shift 2
      ;;
    --chip)
      [[ $# -ge 2 ]] || fail "Missing value for --chip"
      CHIP="$2"
      shift 2
      ;;
    --baud)
      [[ $# -ge 2 ]] || fail "Missing value for --baud"
      BAUD="$2"
      shift 2
      ;;
    --esptool)
      [[ $# -ge 2 ]] || fail "Missing value for --esptool"
      ESPTOOL_BIN="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "Unknown option: $1 (use --help)"
      ;;
  esac
done

if [[ -z "$PORT" ]]; then
  for candidate in /dev/cu.usbmodem* /dev/tty.usbmodem*; do
    if [[ -e "$candidate" ]]; then
      PORT="$candidate"
      break
    fi
  done
fi

[[ -n "$PORT" ]] || fail "No USB modem serial port found; pass --port explicitly"
[[ -e "$PORT" ]] || fail "Port does not exist: $PORT"
resolve_esptool_bin
[[ "$BAUD" =~ ^[0-9]+$ ]] || fail "--baud must be numeric (got '$BAUD')"

echo "[probe] ============================================================================"
echo "[probe] Runtime USB CDC direct-esptool viability probe"
echo "[probe] ============================================================================"
echo "[probe][evidence] selected_port=${PORT}"
echo "[probe][evidence] chip=${CHIP}"
echo "[probe][evidence] baud=${BAUD}"
echo "[probe][evidence] esptool_bin=${ESPTOOL_BIN}"
echo "[probe][evidence] mode=non-destructive-read-mac"

set +e
probe_output="$("$ESPTOOL_BIN" --chip "$CHIP" --port "$PORT" --baud "$BAUD" read_mac 2>&1)"
probe_rc=$?
set -e

echo "$probe_output"

if (( probe_rc != 0 )); then
  fail "esptool read_mac failed on runtime CDC path (rc=${probe_rc})"
fi

mac_line="$(echo "$probe_output" | grep -E 'MAC: ' | tail -n1 || true)"
[[ -n "$mac_line" ]] || fail "read_mac completed but MAC evidence line was not found"

echo "[probe][evidence][pass] esptool_read_mac_rc=${probe_rc}"
echo "[probe][evidence][pass] ${mac_line}"
pass "Direct esptool read probe succeeded on runtime USB CDC path (${PORT})"
