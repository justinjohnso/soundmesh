#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_H="lib/config/include/config/build.h"
METRICS_FILE=".pio/build/preupload_gate_metrics.tsv"
PORTAL_EVIDENCE_FILE="${PORTAL_RUNTIME_EVIDENCE_FILE:-docs/operations/runtime-evidence/portal-enable-evidence.env}"
EVIDENCE_MAX_AGE_DAYS="${PORTAL_RUNTIME_EVIDENCE_MAX_AGE_DAYS:-14}"
HIL_CHECK_SCRIPT="tools/hil_soak_check.py"

RAM_TOTAL_BYTES=327680
SRC_RAM_PCT_MAX=70.0
OUT_RAM_PCT_MAX=65.0

SRC_RAM_USED_MAX=180000
OUT_RAM_USED_MAX=150000

PORTAL_RUNTIME_GUARD_BYTES=8192
PORTAL_RUNTIME_HEAP_MIN_BYTES=40000
PORTAL_MAIN_STACK_HWM_MIN_BYTES=1024
PORTAL_HTTP_OK_RUNS_MIN=100
PORTAL_HIL_DURATION_MIN_SECONDS=300
PORTAL_HIL_IGNORE_RESET_WINDOW_MIN_SECONDS=8

fail() {
  echo "[gate][fail] $*"
  exit 1
}

pass() {
  echo "[gate][pass] $*"
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || fail "Missing required file: $path"
}

extract_define_expr() {
  local name="$1"
  local line
  line="$(grep -E "^#define[[:space:]]+${name}[[:space:]]+" "$BUILD_H" | head -n1 || true)"
  [[ -n "$line" ]] || return 1
  echo "$line" | sed -E "s/^#define[[:space:]]+${name}[[:space:]]+//" | sed -E 's://.*$::' | xargs
}

extract_define_int() {
  local name="$1"
  python3 - "$name" "$BUILD_H" <<'PY'
import re
import sys

def extract_defines(file_path):
    defines = {}
    # Basic regex to find #define NAME VALUE
    pattern = re.compile(r"^#define\s+([A-Za-z0-9_]+)\s+(.+)$")
    try:
        with open(file_path, "r") as f:
            for line in f:
                line = line.strip()
                # Remove comments
                line = re.sub(r"//.*$", "", line)
                line = re.sub(r"/\*.*?\*/", "", line)
                match = pattern.match(line)
                if match:
                    name, expr = match.groups()
                    defines[name.strip()] = expr.strip()
    except Exception as e:
        print(f"Error reading {file_path}: {e}", file=sys.stderr)
        sys.exit(1)
    return defines

def resolve(name, defines, visited=None):
    if visited is None:
        visited = set()
    if name in visited:
        raise ValueError(f"Circular dependency detected for {name}")
    
    if name not in defines:
        # Might be a literal number
        if re.fullmatch(r"0x[0-9A-Fa-f]+|[0-9]+", name):
            return name
        return None

    visited.add(name)
    expr = defines[name]
    
    # Find all potential symbols in the expression
    symbols = re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", expr)
    for sym in symbols:
        res = resolve(sym, defines, visited.copy())
        if res is not None:
            # Replace the symbol with its resolved value (wrapped in parens)
            expr = re.sub(r"\b" + sym + r"\b", f"({res})", expr)
    
    return expr

target_name = sys.argv[1]
file_path = sys.argv[2]
defines = extract_defines(file_path)

if target_name not in defines:
    print(f"Error: {target_name} not found in {file_path}", file=sys.stderr)
    sys.exit(1)

try:
    resolved = resolve(target_name, defines)
    # Clean up any non-math characters just in case
    math_expr = re.sub(r"[^0-9()\s+\-*/%]", "", resolved)
    val = int(eval(math_expr, {"__builtins__": None}, {}))
    print(val)
except Exception as e:
    print(f"Error resolving {target_name}: {e}", file=sys.stderr)
    sys.exit(1)
PY
}

float_gt() {
  local lhs="$1"
  local rhs="$2"
  awk -v a="$lhs" -v b="$rhs" 'BEGIN { exit !(a > b) }'
}

metric_value() {
  local key="$1"
  local value
  value="$(grep -E "^${key}=" "$PORTAL_EVIDENCE_FILE" | tail -n1 | cut -d'=' -f2- || true)"
  value="${value%\"}"
  value="${value#\"}"
  [[ -n "$value" ]] || fail "Missing evidence marker ${key} in ${PORTAL_EVIDENCE_FILE}"
  echo "$value"
}

metric_int() {
  local key="$1"
  local value
  value="$(metric_value "$key")"
  [[ "$value" =~ ^[0-9]+$ ]] || fail "Evidence marker ${key} must be an integer, got '${value}'"
  echo "$value"
}

metric_float_ge() {
  local key="$1"
  local min_value="$2"
  local value
  value="$(metric_value "$key")"
  python3 - "$key" "$value" "$min_value" <<'PY'
import sys
key = sys.argv[1]
raw_value = sys.argv[2]
raw_min = sys.argv[3]
try:
    value = float(raw_value)
except ValueError:
    raise SystemExit(f"{key} must be numeric, got '{raw_value}'")
minimum = float(raw_min)
if value < minimum:
    raise SystemExit(f"{key} too low ({value} < {minimum})")
PY
}

echo "[gate] ============================================================================"
echo "[gate] MeshNet Audio Pre-Upload Overflow/Crash Gate"
echo "[gate] ============================================================================"
echo ""

echo "[gate] Running native unit tests..."
pio test -e native || exit 1

echo "[gate] Resetting generated sdkconfig caches..."
for cfg in sdkconfig.src sdkconfig.out; do
  if [[ -f "$cfg" ]]; then
    rm -f "$cfg"
    echo "[gate] Removed stale $cfg"
  fi
done

echo "[gate] Building src/out environments..."
build_log="$(mktemp)"
trap 'rm -f "$build_log"' EXIT

pio run -e src -e out 2>&1 | tee "$build_log" > /dev/null || exit 1

echo "[gate] Validating build artifacts..."
mkdir -p "$(dirname "$METRICS_FILE")"
echo -e "env\tram_pct\tram_used_bytes\tram_total_bytes\tram_free_bytes\telf_size_bytes\tmap_size_bytes" > "$METRICS_FILE"

for env in src out; do
  elf_path=".pio/build/${env}/firmware.elf"
  map_path=".pio/build/${env}/meshnet-audio.map"
  cfg_path=".pio/build/${env}/config/sdkconfig.h"
  require_file "$elf_path"
  require_file "$map_path"
  require_file "$cfg_path"

done
pass "Artifact set present (firmware.elf + map + sdkconfig per env)"

echo "[gate] Extracting role-specific memory metrics..."
ram_report_lines=()
while IFS= read -r line; do
  ram_report_lines+=("$line")
done < <(grep 'RAM:' "$build_log")
[[ ${#ram_report_lines[@]} -ge 2 ]] || fail "Could not parse RAM usage for src/out"

# Runtime memory model constants (SRAM)
RUNTIME_SRAM_TOTAL_MAX=220000 # Keep 100KB+ headroom for WiFi/Mesh/LWIP/Heaps

echo "[gate] Extracting pipeline and stack constants for runtime budget..."
pcm_buf_size="$(extract_define_int PCM_BUFFER_SIZE)"
opus_buf_size="$(extract_define_int OPUS_BUFFER_SIZE)"
capture_stack="$(extract_define_int CAPTURE_TASK_STACK_BYTES)"
encode_stack="$(extract_define_int ENCODE_TASK_STACK_BYTES)"
decode_stack="$(extract_define_int DECODE_TASK_STACK_BYTES)"
playback_stack="$(extract_define_int PLAYBACK_TASK_STACK_BYTES)"
mesh_rx_stack="$(extract_define_int MESH_RX_TASK_STACK_BYTES)"
hb_stack="$(extract_define_int HEARTBEAT_TASK_STACK_BYTES)"

# Note: PCM buffer is moved to PSRAM if > 16KB, so we conditionally count it
pcm_in_sram=$(( pcm_buf_size > 16384 ? 0 : pcm_buf_size ))

ENVS=(src out)
for i in 0 1; do
  line="${ram_report_lines[$i]}"
  env="${ENVS[$i]}"

  ram_pct="$(echo "$line" | sed -E 's/.*] *([0-9]+(\.[0-9]+)?)%.*/\1/')"
  ram_used="$(echo "$line" | sed -E 's/.*used ([0-9]+) bytes from.*/\1/')"
  ram_total="$(echo "$line" | sed -E 's/.*from ([0-9]+) bytes.*/\1/')"

  [[ "$ram_pct" =~ ^[0-9]+(\.[0-9]+)?$ ]] || fail "Unable to parse RAM % for ${env} from: ${line}"
  [[ "$ram_used" =~ ^[0-9]+$ ]] || fail "Unable to parse RAM used bytes for ${env} from: ${line}"
  [[ "$ram_total" =~ ^[0-9]+$ ]] || fail "Unable to parse RAM total bytes for ${env} from: ${line}"

  (( ram_total == RAM_TOTAL_BYTES )) || fail "Unexpected RAM total for ${env}: ${ram_total} (expected ${RAM_TOTAL_BYTES})"

  # Calculate Predicted Peak SRAM Usage
  if [[ "$env" == "src" ]]; then
    predicted_sram=$((ram_used + pcm_in_sram + opus_buf_size + capture_stack + encode_stack + mesh_rx_stack + hb_stack))
  else
    predicted_sram=$((ram_used + pcm_in_sram + opus_buf_size + decode_stack + playback_stack + mesh_rx_stack + hb_stack))
  fi

  echo "[gate] ${env} predicted peak SRAM: ${predicted_sram} bytes (limit: ${RUNTIME_SRAM_TOTAL_MAX})"

  if (( predicted_sram > RUNTIME_SRAM_TOTAL_MAX )); then
    fail "${env} predicted SRAM ${predicted_sram} exceeds safety limit ${RUNTIME_SRAM_TOTAL_MAX}"
  fi

  ram_free=$((ram_total - ram_used))
  elf_size="$(stat -f%z ".pio/build/${env}/firmware.elf")"
  map_size="$(stat -f%z ".pio/build/${env}/meshnet-audio.map")"
  echo -e "${env}\t${ram_pct}\t${ram_used}\t${ram_total}\t${ram_free}\t${elf_size}\t${map_size}" >> "$METRICS_FILE"

  case "$env" in
    src)
      float_gt "$ram_pct" "$SRC_RAM_PCT_MAX" && fail "SRC RAM ${ram_pct}% exceeds ${SRC_RAM_PCT_MAX}%"
      (( ram_used > SRC_RAM_USED_MAX )) && fail "SRC RAM used ${ram_used} exceeds ${SRC_RAM_USED_MAX}"
      ;;
    out)
      float_gt "$ram_pct" "$OUT_RAM_PCT_MAX" && fail "OUT RAM ${ram_pct}% exceeds ${OUT_RAM_PCT_MAX}%"
      (( ram_used > OUT_RAM_USED_MAX )) && fail "OUT RAM used ${ram_used} exceeds ${OUT_RAM_USED_MAX}"
      ;;
  esac
done
pass "Role-specific RAM limits satisfied (src/out)"
pass "Generated artifact metrics: ${METRICS_FILE}"

echo "[gate] Validating stack and heap budget constants..."
portal_http_stack="$(extract_define_int PORTAL_HTTP_STACK_BYTES)"
portal_ws_stack="$(extract_define_int PORTAL_WS_PUSH_STACK_BYTES)"
portal_dns_stack="$(extract_define_int PORTAL_DNS_STACK_BYTES)"
portal_min_heap="$(extract_define_int PORTAL_MIN_FREE_HEAP)"

# Already extracted capture_stack etc above for SRAM prediction

(( portal_http_stack >= 6144 )) || fail "PORTAL_HTTP_STACK_BYTES too small (${portal_http_stack})"
(( portal_ws_stack >= 4096 )) || fail "PORTAL_WS_PUSH_STACK_BYTES too small (${portal_ws_stack})"
(( portal_dns_stack >= 3072 )) || fail "PORTAL_DNS_STACK_BYTES too small (${portal_dns_stack})"
(( portal_min_heap >= 30720 )) || fail "PORTAL_MIN_FREE_HEAP too small (${portal_min_heap})"

(( capture_stack >= 8192 )) || fail "CAPTURE_TASK_STACK_BYTES too small (${capture_stack})"
(( encode_stack >= 24576 )) || fail "ENCODE_TASK_STACK_BYTES too small (${encode_stack})"
(( decode_stack >= 16384 )) || fail "DECODE_TASK_STACK_BYTES too small (${decode_stack})"
(( playback_stack >= 4096 )) || fail "PLAYBACK_TASK_STACK_BYTES too small (${playback_stack})"
(( mesh_rx_stack >= 4096 )) || fail "MESH_RX_TASK_STACK_BYTES too small (${mesh_rx_stack})"
(( hb_stack >= 3072 )) || fail "HEARTBEAT_TASK_STACK_BYTES too small (${hb_stack})"

# Verify consistency between PCM and Jitter buffers
jitter_frames="$(extract_define_int JITTER_BUFFER_FRAMES)"
pcm_frames="$(extract_define_int PCM_BUFFER_FRAMES)"
(( jitter_frames <= pcm_frames )) || fail "JITTER_BUFFER_FRAMES (${jitter_frames}) cannot exceed PCM_BUFFER_FRAMES (${pcm_frames})"

portal_static_runtime_budget=$((portal_http_stack + portal_ws_stack + portal_dns_stack + portal_min_heap + PORTAL_RUNTIME_GUARD_BYTES))
pass "Stack/heap floors valid (portal runtime budget=${portal_static_runtime_budget} bytes)"

echo "[gate] Validating role-specific portal constraints..."
src_portal="$(extract_define_expr ENABLE_SRC_USB_PORTAL_NETWORK)"
out_portal="$(extract_define_expr ENABLE_OUT_USB_PORTAL_NETWORK)"

[[ "$src_portal" =~ ^[01]$ ]] || fail "ENABLE_SRC_USB_PORTAL_NETWORK must be 0 or 1 (got '${src_portal}')"
[[ "$out_portal" =~ ^[01]$ ]] || fail "ENABLE_OUT_USB_PORTAL_NETWORK must be 0 or 1 (got '${out_portal}')"

if [[ "$out_portal" == "1" && "$src_portal" != "1" ]]; then
  fail "ENABLE_OUT_USB_PORTAL_NETWORK=1 requires ENABLE_SRC_USB_PORTAL_NETWORK=1"
fi

if grep -q 'portal_init(' src/out/main.c && [[ "$out_portal" == "0" ]]; then
  fail "OUT calls portal_init() while ENABLE_OUT_USB_PORTAL_NETWORK=0"
fi

if [[ "$src_portal" == "0" ]]; then
  if grep -q 'portal_init(' src/src/main.c; then
    fail "SRC calls portal_init() while ENABLE_SRC_USB_PORTAL_NETWORK=0"
  fi
else
  if ! grep -q '#if ENABLE_SRC_USB_PORTAL_NETWORK' src/src/main.c; then
    fail "SRC portal init path must remain guarded by ENABLE_SRC_USB_PORTAL_NETWORK"
  fi
fi
pass "Role-specific portal constraints validated"

echo "[gate] Validating runtime safety configs from build artifacts..."
for env in src out; do
  cfg_path=".pio/build/${env}/config/sdkconfig.h"
  grep -q '^#define CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 1' "$cfg_path" || fail "${env}: stack overflow canary must be enabled"

  if grep -q '^#define CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK' "$cfg_path"; then
    grep -q '^#define CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK 1' "$cfg_path" || fail "${env}: stack watchpoint symbol present but disabled"
  else
    echo "[gate][warn] ${env}: CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK not emitted by this ESP-IDF profile"
  fi

  if grep -q '^#define CONFIG_HEAP_POISONING_LIGHT 1' "$cfg_path"; then
    :
  elif grep -q '^#define CONFIG_HEAP_POISONING_DISABLED 1' "$cfg_path"; then
    echo "[gate][warn] ${env}: heap poisoning is disabled in this profile"
  else
    fail "${env}: heap poisoning config missing (expected LIGHT or DISABLED)"
  fi

  grep -q '^#define CONFIG_ESP_SYSTEM_MEMPROT_FEATURE 1' "$cfg_path" || fail "${env}: memory protection must be enabled"
  grep -q '^#define CONFIG_ESP_TASK_WDT_EN 1' "$cfg_path" || fail "${env}: task watchdog must be enabled"
  grep -q '^#define CONFIG_ESP_TASK_WDT_INIT 1' "$cfg_path" || fail "${env}: task watchdog init must be enabled"
  grep -q '^#define CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT 1' "$cfg_path" || fail "${env}: panic print+reboot must be enabled"
done
pass "Runtime safety configs validated (stack canary + memprot + task WDT + panic reboot)"

echo "[gate] Validating OTA partition/rollback safety..."
if ! grep -Eq '^[[:space:]]*otadata,[[:space:]]*data,[[:space:]]*ota,' partitions.csv; then
  fail "partitions.csv missing otadata partition"
fi
if ! grep -Eq '^[[:space:]]*ota_0,[[:space:]]*app,[[:space:]]*ota_0,' partitions.csv; then
  fail "partitions.csv missing ota_0 partition"
fi
if ! grep -Eq '^[[:space:]]*ota_1,[[:space:]]*app,[[:space:]]*ota_1,' partitions.csv; then
  fail "partitions.csv missing ota_1 partition"
fi
for env in src out; do
  cfg_path=".pio/build/${env}/config/sdkconfig.h"
  defaults_path="sdkconfig.${env}.defaults"
  cfg_bootloader_rb=0
  cfg_app_rb=0
  defaults_bootloader_rb=0
  defaults_app_rb=0

  if grep -q '^#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' "$cfg_path"; then
    cfg_bootloader_rb=1
  fi
  if grep -Eq '^#define CONFIG_APP_ROLLBACK_ENABLE[[:space:]]+(1|CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)$' "$cfg_path"; then
    cfg_app_rb=1
  fi

  if [[ -f "$defaults_path" ]]; then
    if grep -q '^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' "$defaults_path"; then
      defaults_bootloader_rb=1
    fi
    if grep -q '^CONFIG_APP_ROLLBACK_ENABLE=y' "$defaults_path"; then
      defaults_app_rb=1
    fi
  fi

  if [[ "$cfg_bootloader_rb" -eq 0 && "$defaults_bootloader_rb" -eq 0 ]]; then
    fail "${env}: bootloader rollback must be enabled"
  fi
  if [[ "$cfg_app_rb" -eq 0 && "$defaults_app_rb" -eq 0 ]]; then
    fail "${env}: app rollback must be enabled"
  fi

  if [[ "$cfg_bootloader_rb" -eq 0 || "$cfg_app_rb" -eq 0 ]]; then
    echo "[gate][warn] ${env}: rollback symbols not emitted in sdkconfig.h; accepting explicit sdkconfig.${env}.defaults rollout policy"
  fi
done
if ! grep -q 'esp_ota_mark_app_valid_cancel_rollback' lib/control/src/portal_ota.c; then
  fail "OTA rollback confirmation path missing in portal_ota.c"
fi
pass "OTA partition/rollback safety validated"

echo "[gate] Validating role entrypoints..."
for env in src out; do
  map_path=".pio/build/${env}/meshnet-audio.map"
  require_file "$map_path"

  case "$env" in
    src) expected_obj=".pio/build/src/src/src/main.o" ;;
    out) expected_obj=".pio/build/out/src/out/main.o" ;;
    *) fail "Unknown env ${env}" ;;
  esac

  if ! grep -q "^app_main[[:space:]]\+${expected_obj}$" "$map_path"; then
    fail "${env}: app_main not linked from expected object (${expected_obj})"
  fi
done
pass "Role entrypoints validated (src/out)"

echo "[gate] Validating portal enablement policy (fail-closed)..."
if [[ "$src_portal" == "1" || "$out_portal" == "1" ]]; then
  require_file "$PORTAL_EVIDENCE_FILE"

  approved="$(metric_value PORTAL_ENABLE_APPROVED)"
  [[ "$approved" == "YES" ]] || fail "Portal enabled but PORTAL_ENABLE_APPROVED!=YES in evidence file"

  evidence_src_flag="$(metric_value PORTAL_SRC_FLAG)"
  evidence_out_flag="$(metric_value PORTAL_OUT_FLAG)"
  [[ "$evidence_src_flag" == "$src_portal" ]] || fail "Evidence SRC flag (${evidence_src_flag}) != build flag (${src_portal})"
  [[ "$evidence_out_flag" == "$out_portal" ]] || fail "Evidence OUT flag (${evidence_out_flag}) != build flag (${out_portal})"

  evidence_utc="$(metric_value EVIDENCE_UTC)"
  python3 - "$evidence_utc" "$EVIDENCE_MAX_AGE_DAYS" <<'PY'
from datetime import datetime, timezone
import sys
stamp = sys.argv[1]
max_days = int(sys.argv[2])
try:
    ts = datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
except ValueError as exc:
    raise SystemExit(f"invalid evidence timestamp '{stamp}': {exc}")
age_days = (datetime.now(timezone.utc) - ts).total_seconds() / 86400.0
if age_days > max_days:
    raise SystemExit(f"evidence too old: {age_days:.1f} days > {max_days}")
PY

  [[ -n "$(metric_value EVIDENCE_COMMIT)" ]] || fail "EVIDENCE_COMMIT cannot be empty"

  src_heap_min="$(metric_int SRC_FREE_HEAP_MIN_BYTES)"
  src_stack_hwm_min="$(metric_int SRC_MAIN_STACK_HWM_MIN_BYTES)"
  http_ok_runs="$(metric_int PORTAL_STATUS_200_OK_RUNS)"
  hil_duration_s="$(metric_int PORTAL_HIL_DURATION_SECONDS)"
  hil_ignore_reset_window_s="$(metric_value PORTAL_HIL_IGNORE_RESET_WINDOW_SECONDS)"
  hil_src_panic_hits="$(metric_int PORTAL_HIL_SRC_PANIC_HITS)"
  hil_out_panic_hits="$(metric_int PORTAL_HIL_OUT_PANIC_HITS)"
  hil_src_late_resets="$(metric_int PORTAL_HIL_SRC_LATE_RESET_HITS)"
  hil_out_late_resets="$(metric_int PORTAL_HIL_OUT_LATE_RESET_HITS)"
  hil_src_ok_hits="$(metric_int PORTAL_HIL_SRC_OK_HITS)"
  hil_out_ok_hits="$(metric_int PORTAL_HIL_OUT_OK_HITS)"

  (( src_heap_min >= PORTAL_RUNTIME_HEAP_MIN_BYTES )) || fail "SRC_FREE_HEAP_MIN_BYTES too low (${src_heap_min})"
  (( src_stack_hwm_min >= PORTAL_MAIN_STACK_HWM_MIN_BYTES )) || fail "SRC_MAIN_STACK_HWM_MIN_BYTES too low (${src_stack_hwm_min})"
  (( http_ok_runs >= PORTAL_HTTP_OK_RUNS_MIN )) || fail "PORTAL_STATUS_200_OK_RUNS too low (${http_ok_runs})"
  (( hil_duration_s >= PORTAL_HIL_DURATION_MIN_SECONDS )) || fail "PORTAL_HIL_DURATION_SECONDS too low (${hil_duration_s})"
  metric_float_ge PORTAL_HIL_IGNORE_RESET_WINDOW_SECONDS "${PORTAL_HIL_IGNORE_RESET_WINDOW_MIN_SECONDS}" >/dev/null || fail "PORTAL_HIL_IGNORE_RESET_WINDOW_SECONDS too low"
  (( hil_src_panic_hits == 0 )) || fail "PORTAL_HIL_SRC_PANIC_HITS must be 0 (got ${hil_src_panic_hits})"
  (( hil_out_panic_hits == 0 )) || fail "PORTAL_HIL_OUT_PANIC_HITS must be 0 (got ${hil_out_panic_hits})"
  (( hil_src_late_resets == 0 )) || fail "PORTAL_HIL_SRC_LATE_RESET_HITS must be 0 (got ${hil_src_late_resets})"
  (( hil_out_late_resets == 0 )) || fail "PORTAL_HIL_OUT_LATE_RESET_HITS must be 0 (got ${hil_out_late_resets})"
  (( hil_src_ok_hits > 0 )) || fail "PORTAL_HIL_SRC_OK_HITS must be >0 (got ${hil_src_ok_hits})"
  (( hil_out_ok_hits > 0 )) || fail "PORTAL_HIL_OUT_OK_HITS must be >0 (got ${hil_out_ok_hits})"

  if [[ "$out_portal" == "1" ]]; then
    out_heap_min="$(metric_int OUT_FREE_HEAP_MIN_BYTES)"
    out_stack_hwm_min="$(metric_int OUT_MAIN_STACK_HWM_MIN_BYTES)"
    (( out_heap_min >= PORTAL_RUNTIME_HEAP_MIN_BYTES )) || fail "OUT_FREE_HEAP_MIN_BYTES too low (${out_heap_min})"
    (( out_stack_hwm_min >= PORTAL_MAIN_STACK_HWM_MIN_BYTES )) || fail "OUT_MAIN_STACK_HWM_MIN_BYTES too low (${out_stack_hwm_min})"
  fi

  pass "Portal enablement evidence accepted (${PORTAL_EVIDENCE_FILE})"
  if [[ ! -f "$HIL_CHECK_SCRIPT" ]]; then
    fail "Missing HIL check script: ${HIL_CHECK_SCRIPT}"
  fi
  pass "HIL validation script present (${HIL_CHECK_SCRIPT})"
else
  pass "Portal disabled in build config (fail-closed default)"
fi

# Check flash warnings
if grep -q "Flash memory size mismatch detected" "$build_log"; then
  echo "[gate][warn] Flash size mismatch (known configuration issue)"
fi

# Preserve historical crash-signature checks
echo "[gate] Checking for crash-signature regressions..."
if grep -R --line-number --fixed-strings 'esp_mesh_waive_root(' lib/network/src src 2>/dev/null; then
  fail "esp_mesh_waive_root() regression"
fi
if grep -R --line-number --fixed-strings 'ESP_ERROR_CHECK(es8388_audio_init' src 2>/dev/null; then
  fail "Fatal ES8388 init pattern"
fi
pass "No crash-signature regressions"

# Check watchdog-safe startup
echo "[gate] Validating watchdog-safe startup..."
if ! grep -q 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))' src/src/main.c; then
  fail "SRC startup missing watchdog-safe notify loop"
fi
pass "Watchdog-safe startup patterns OK"

echo ""
echo "================================================================================"
echo "[gate] ✓ ALL PRE-UPLOAD CRASH GATES PASSED"
echo "================================================================================"
echo "Role RAM limits: SRC<=${SRC_RAM_PCT_MAX}% OUT<=${OUT_RAM_PCT_MAX}%"
echo "Portal runtime budget floor: ${portal_static_runtime_budget} bytes"
echo "Metrics artifact: ${METRICS_FILE}"
echo ""

exit 0
