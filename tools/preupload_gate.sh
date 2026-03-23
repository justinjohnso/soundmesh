#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[gate] Running native tests"
pio test -e native

echo "[gate] Building tx/rx/combo"
build_log="$(mktemp)"
trap 'rm -f "$build_log"' EXIT
pio run -e tx -e rx -e combo | tee "$build_log"

echo "[gate] Checking for flash-size mismatch warning"
if grep -q "Flash memory size mismatch detected" "$build_log"; then
  echo "[gate][warn] Flash size mismatch warning present (known environment warning)."
fi

echo "[gate] Checking crash-prone config toggles"
src_portal_flag="$(grep -E '^#define ENABLE_SRC_USB_PORTAL_NETWORK[[:space:]]+[01]$' lib/config/include/config/build.h | awk '{print $3}')"
out_portal_flag="$(grep -E '^#define ENABLE_OUT_USB_PORTAL_NETWORK[[:space:]]+[01]$' lib/config/include/config/build.h | awk '{print $3}')"

if [[ -z "${src_portal_flag:-}" || -z "${out_portal_flag:-}" ]]; then
  echo "[gate][fail] Missing phased portal flags in build.h"
  exit 1
fi

if [[ "$src_portal_flag" != "0" && "$src_portal_flag" != "1" ]]; then
  echo "[gate][fail] ENABLE_SRC_USB_PORTAL_NETWORK must be 0 or 1"
  exit 1
fi

if [[ "$out_portal_flag" != "0" && "$out_portal_flag" != "1" ]]; then
  echo "[gate][fail] ENABLE_OUT_USB_PORTAL_NETWORK must be 0 or 1"
  exit 1
fi

if [[ "$out_portal_flag" == "1" ]]; then
  echo "[gate][fail] ENABLE_OUT_USB_PORTAL_NETWORK=1 is blocked in current rollout phase."
  exit 1
fi

if grep -q '^CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n' sdkconfig.rx.defaults; then
  echo "[gate][fail] RX console is not on USB Serial JTAG; high crash/debug-risk config."
  exit 1
fi

if grep -q 'portal_init(' src/rx/main.c; then
  echo "[gate][fail] RX portal_init path is enabled while OUT portal flag is blocked."
  exit 1
fi

echo "[gate] Checking historical crash-signature regressions"
if grep -R --line-number --fixed-strings 'esp_mesh_waive_root(' lib/network/src src 2>/dev/null; then
  echo "[gate][fail] esp_mesh_waive_root() regression detected (historical mesh_timeout overflow vector)."
  exit 1
fi

if grep -R --line-number --fixed-strings 'ESP_ERROR_CHECK(es8388_audio_init' src 2>/dev/null; then
  echo "[gate][fail] Fatal ES8388 init pattern detected (historical SRC crash-loop vector)."
  exit 1
fi

if ! grep -q 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))' src/tx/main.c; then
  echo "[gate][fail] TX startup wait no longer uses watchdog-safe chunked notify loop."
  exit 1
fi

if ! grep -q 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))' src/combo/main.c; then
  echo "[gate][fail] COMBO startup wait no longer uses watchdog-safe chunked notify loop."
  exit 1
fi

echo "[gate] PASS: pre-upload crash gate satisfied"
