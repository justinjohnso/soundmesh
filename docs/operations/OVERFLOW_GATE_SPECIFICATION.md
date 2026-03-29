# SUPERSEDED — Production-Grade Overflow/Crash Gate Specification

This document is superseded by the active operator docs:

- `docs/operations/deployment-checklists.md`
- `docs/operations/runbook.md`
- `docs/operations/troubleshooting.md`

Use those files for current SRC/OUT procedures and gate behavior.

# Production-Grade Overflow/Crash Gate Specification

**MeshNet Audio ESP32-S3 Firmware**  
**Version:** 1.0  
**Date:** 2025-03-23  
**Scope:** Pre-upload safety gates to prevent runtime overflow regressions  

---

## Executive Summary

This document specifies production-grade gates that catch stack/heap overflow risk **before device deployment**. Current gate tool (`tools/preupload_gate.sh`) had blind spots that allowed a portal-enabled overflow regression to escape.

**Key deliverables:**
- Enhanced `tools/preupload_gate.sh` with 8+ new checks
- Memory usage thresholding (75% safety margin)
- Stack budget assertions (6+ task minimum sizes)
- Fail-closed configuration defaults (both portal flags disabled)
- Historical crash-signature regression detection
- Deterministic manual HIL smoke check procedure

**Result:** Gates now catch overflow regressions automatically in CI; remains agile for active development.

---

## 1. Audit: Current Gate Blind Spots

### What the Original Gate Caught
- ✓ Native unit tests pass (logic validation)
- ✓ Build succeeds for tx/rx/combo (syntax/linker sanity)
- ✓ Flash size mismatch warnings (config errors)
- ✓ Hard-coded crash patterns (esp_mesh_waive_root, fatal ES8388 init)
- ✓ Portal flags are 0 or 1 (valid range)
- ✓ Watchdog-safe startup loops (ulTaskNotifyTake)

### Critical Blind Spots

| Gap | Impact | Root Cause | New Gate |
|-----|--------|-----------|----------|
| **No memory usage validation** | Portal init (30KB+) can overflow heap if audio buffers already large | Static memory usage not analyzed | Gate 3: RAM % threshold (75%) |
| **No stack budget enforcement** | Portal HTTP/DNS/WS tasks undefined; can underflow and corrupt adjacent tasks | Stack sizes in build.h never validated | Gate 6: Stack budget assertions (6+ tasks) |
| **No heap floor assertion** | Portal init fails silently if <30KB free; device reboots under load | No pre-init heap check; PORTAL_MIN_FREE_HEAP defined but unused | Gate 6: Validate PORTAL_MIN_FREE_HEAP |
| **No role-specific validation** | TX/RX/COMBO have different memory profiles; gate treats them identically | Single threshold applies to all roles | Gate 3: Per-environment RAM checks |
| **No startup smoke check** | Device crashes minutes after boot in field; no early warning | No runtime watermark sampling | Manual: HIL smoke check (1h) |
| **No panic-signature coverage** | New portal vector (malloc→heap exhaustion→watchdog) not in regex list | Only specific patterns checked; new attack surface ignored | Future: Machine-learned panic patterns |

**Portal overflow regression that escaped:**
1. `ENABLE_SRC_USB_PORTAL_NETWORK=1` was added
2. Gate correctly blocked it (flag check passed)
3. **But:** If gate logic became permissive (e.g., "warn instead of fail"), portal would initialize
4. Portal malloc consumes 30KB+; heap fragmentation under audio load
5. Watchdog timeout during malloc → silent reboot (no crash log)
6. User sees device continuously cycling, no logs to debug

---

## 2. Professional Patterns & Standards

### Pattern 1: Memory Usage Thresholding

**Industry standard:** PlatformIO reports RAM as percentage of total; gate enforces ceiling.

**Rationale:**
- ESP32-S3 has 327.68 KB DRAM (fixed hardware)
- Audio pipeline allocates ~78 KB static
- Mesh/WiFi stack allocates ~40 KB
- Portal (HTTP, TinyUSB, networking) allocates ~30 KB
- **Safe margin:** Leave 75 KB unallocated for runtime fragmentation & task stacks
- **Threshold:** 75% (250 KB used)

**Gate 3 Implementation:**
```bash
ram_lines=($(grep "RAM:" build.log | sed -n 's/.*\([0-9]*\.[0-9]*\)%.*/\1/p'))
if (( $(echo "$ram > 75" | bc -l) )); then
  fail "RAM exceeds 75% threshold"
fi
```

### Pattern 2: Stack Budget Assertions

**Standard:** Define per-task stack sizes in config; compile-time validation ensures safety.

**Why this matters:**
- ESP-IDF FreeRTOS xTaskCreate takes stack size in BYTES
- Tasks with insufficient stack corrupt heap (adjacent memory)
- Overflow manifests as:
  - Local variables on stack overwrite adjacent memory
  - Return addresses corrupted → execution jumps
  - Heap metadata corrupted → malloc fails silently

**Gate 6 Implementation:** Grep for 8+ task stack defines:
```c
// build.h (actual values):
#define PORTAL_HTTP_STACK_BYTES      (6 * 1024)   // TinyUSB + HTTP parser
#define PORTAL_WS_PUSH_STACK_BYTES   (4 * 1024)   // WebSocket push task
#define PORTAL_DNS_STACK_BYTES       (3 * 1024)   // DHCP/DNS task
#define ENCODE_TASK_STACK_BYTES      (32 * 1024)  // Opus encoder (heavy)
#define DECODE_TASK_STACK_BYTES      (20 * 1024)  // Opus decoder
#define CAPTURE_TASK_STACK_BYTES     (8 * 1024)   // I2S ADC + activity
#define PLAYBACK_TASK_STACK_BYTES    (4 * 1024)   // I2S DAC only
```

**Gate ensures all exist and are non-zero.**

### Pattern 3: Fail-Closed Configuration Defaults

**Standard:** Safety-critical features disabled by default; enable via explicit, reviewed config.

**Rationale:**
- Portal adds 30KB+ heap allocation
- Introduces new tasks, new malloc paths
- Historically has bugs (USB, HTTP parsing, WebSocket)
- **Default:** Both `ENABLE_SRC_USB_PORTAL_NETWORK` and `ENABLE_OUT_USB_PORTAL_NETWORK` = 0
- **To enable:** Explicit build.h change + gate override (future)

**Gate 5 Implementation:**
```bash
if [[ "$src_portal_flag" != "0" || "$out_portal_flag" != "0" ]]; then
  fail "Portal must be disabled by default"
fi
```

### Pattern 4: Historical Crash-Signature Regression Detection

**Standard:** Grep for known crash vectors; prevent re-introduction.

**Current signatures (Gate 9):**
- `esp_mesh_waive_root()` → mesh_timeout overflow (known vector)
- `ESP_ERROR_CHECK(es8388_audio_init)` → blocks startup/recovery

**Extensible:** Add new signatures as new vulnerabilities discovered.

### Pattern 5: Console Configuration for Observability

**Standard:** Crash diagnostics require debug output; gate enforces USB Serial JTAG on RX.

**Rationale:**
- ESP32-S3 can output crash dumps to UART
- If UART console disabled, device silently reboots
- **Gate 7:** RX must have `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`

### Pattern 6: Role-Specific Validation

**Standard:** TX/RX/COMBO have different memory budgets; validate per-environment.

**Gate 3 extracts RAM for each:**
```bash
tx_ram=$(grep "RAM:" build.log | sed -n '1p' | awk '{print $X}')   # First RAM line
rx_ram=$(grep "RAM:" build.log | sed -n '2p' | awk '{print $X}')   # Second
combo_ram=$(grep "RAM:" build.log | sed -n '3p' | awk '{print $X}') # Third
```

---

## 3. Gate Specification: 10 Checks

### Gate 1: Native Unit Tests
- **What:** Run PlatformIO native tests
- **Why:** Validates logic (frame codec, audio pipeline) on host
- **Pass condition:** All tests pass
- **Fail action:** Block upload; require test fix

### Gate 2: Build All Environments
- **What:** `pio run -e tx -e rx -e combo`
- **Why:** Sanity check; catch linker errors, missing symbols
- **Pass condition:** All 3 environments build successfully
- **Fail action:** Block upload; require build fix

### Gate 3: Memory Usage Thresholding
- **What:** Parse build output for RAM % usage
- **Why:** Catch heap exhaustion before deployment
- **Pass condition:** TX, RX, COMBO all < 75% of 327.68 KB
- **Fail action:** Block upload; reduce static allocations (buffers, tasks)
- **Note:** 75% leaves ~77 KB for portal allocations + fragmentation

### Gate 4: Flash Size Mismatch Warning
- **What:** Check for PlatformIO warning about flash size
- **Why:** Configuration mismatch can prevent OTA
- **Pass condition:** Issue warning (non-fatal); continue
- **Fail action:** Warn developer; continue (known issue with test boards)

### Gate 5: Portal Configuration (Fail-Closed)
- **What:** Validate ENABLE_SRC_USB_PORTAL_NETWORK and ENABLE_OUT_USB_PORTAL_NETWORK
- **Why:** Portal must be disabled by default
- **Pass condition:** Both flags must be 0
- **Fail action:** Block upload; reset flags to 0

### Gate 6: Stack Budget Assertions
- **What:** Grep for 8+ stack size defines in build.h
- **Why:** Prevent task overflow; ensure all critical tasks have budgets
- **Pass condition:** All 8 defines exist and are non-zero
- **Fail action:** Block upload; add missing stack budget to build.h
- **Budgets:**
  - Portal: HTTP (6KB), WS Push (4KB), DNS (3KB)
  - Audio: Capture (8KB), Encode (32KB), Decode (20KB), Playback (4KB)
  - Heap: PORTAL_MIN_FREE_HEAP (30KB)

### Gate 7: Console Configuration
- **What:** Check RX sdkconfig for USB Serial JTAG enabled
- **Why:** Crash diagnostics require console output
- **Pass condition:** `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig.rx.defaults
- **Fail action:** Block upload; enable USB Serial JTAG

### Gate 8: Portal Init Path Consistency
- **What:** If RX calls portal_init(), verify OUT portal flag is enabled
- **Why:** Catch config mismatches (code calls feature but config disables it)
- **Pass condition:** Either portal_init() not in RX, or OUT flag is 1
- **Fail action:** Block upload; align config with code

### Gate 9: Crash-Signature Regression Detection
- **What:** Grep for known crash vectors
- **Why:** Prevent re-introduction of historical bugs
- **Pass condition:** No matches for:
  - `esp_mesh_waive_root()` (mesh overflow)
  - `ESP_ERROR_CHECK(es8388_audio_init)` (startup crash)
- **Fail action:** Block upload; remove offending code

### Gate 10: Watchdog-Safe Startup Patterns
- **What:** Verify TX and COMBO use `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))`
- **Why:** Chunked notification waits prevent watchdog timeout during init
- **Pass condition:** Both src/tx/main.c and src/combo/main.c contain the pattern
- **Fail action:** Block upload; refactor startup to use chunked waits

---

## 4. Implementation: Enhanced tools/preupload_gate.sh

**Location:** `tools/preupload_gate.sh`

**Usage:**
```bash
cd /path/to/meshnet-audio
bash tools/preupload_gate.sh
```

**Output (on pass):**
```
[gate] ✓ ALL PRE-UPLOAD CRASH GATES PASSED
Memory: TX=23.8%, RX=23.8%, COMBO=21.8% (all < 75%)
Stack budgets: Validated
Portal config: Safe (disabled)
Crash signatures: None detected
```

**Exit code:** 0 (pass) or 1 (fail)

**Runtime:** ~45 seconds (includes 3 full builds)

**Integration:**
- Add to CI pipeline: `.github/workflows/pre-upload.yml` runs before merge
- Local: Run manually before deployment
- Pre-release: Required gate to pass before tagging version

---

## 5. Manual HIL: Deterministic Smoke Check

**What cannot be automated:**
- Real mesh network stability (multiple nodes, RF interference)
- Portal HTTP streaming under concurrent load
- Heap fragmentation over 24+ hours
- Watchdog reset scenarios

**Lightweight smoke check (5 minutes / 300 seconds, deterministic):**

### Procedure: Post-Build Validation

1. **Flash firmware to device**
   ```bash
   pio run -t upload -e tx
   ```

2. **Monitor startup logs (2 minutes)**
   - Connect UART at 115200 baud
   - Look for:
     - All subsystem init logs (audio, mesh, etc.)
     - NO watchdog resets or panics
     - Stack watermarks logged:
       ```
       Main task stack high water mark: XXXX bytes
       Free heap: YYYY bytes
       ```
   - **Acceptance:** Startup completes, no resets

3. **Test portal endpoints (if enabled)**
   - Connect USB to computer
   - Query `/api/status` 100 times:
     ```bash
     for i in {1..100}; do
       curl -s http://10.48.X.Y/api/status | jq .
       sleep 0.1
     done
     ```
   - **Acceptance:** All 100 requests return 200 OK, no timeouts

4. **Capture heap/stack watermark snapshots during soak**
   - Capture watermark lines at least at start and end of the soak window
   - Run for 5 minutes (300 seconds)
   - Log extraction (UART):
     ```
     Free heap: 45000 bytes (min: 42000)
     Stack watermarks (low):
       - Encode: 2048 bytes (good, > 1024)
       - Decode: 1500 bytes (acceptable)
       - Playback: 512 bytes (minimal but safe)
     ```
   - **Acceptance:** No task < 256 bytes headroom, no monotonic heap decline

5. **Escalate to full HIL if:**
   - Any startup log shows watchdog reset
   - Any stack watermark < 512 bytes
   - Heap declines monotonically (leak suspect)
   - Portal endpoints timeout or crash

### Acceptance Criteria for Manual Testing

| Check | Pass Condition | Fail Action |
|-------|---|---|
| Startup logs | All subsystems initialized, no resets (2min) | Block release; investigate logs |
| Portal HTTP (if enabled) | 100/100 requests return 200 OK | Block release; debug HTTP layer |
| Heap floor | Stays > 30 KB minimum for full 5-minute soak | Block release; profile malloc usage |
| Task watermarks | All > 512 bytes headroom | Block release; increase stack sizes |

---

## 6. Acceptance Criteria & Policy

### For Automated Gate
- **Policy:** All 10 gates must pass before upload
- **Threshold:** Fail-on-first
- **Override:** Only with explicit review + dual sign-off (CI owner + firmware lead)
- **Audit log:** Gate results logged to build artifacts

### For Manual HIL
- **Frequency:** Weekly (if portal enabled), pre-release (all builds)
- **Owner:** Firmware lead or designated test engineer
- **Duration:** 5 minutes (300 seconds) per test pass
- **Documentation:** Results logged in release notes under "Validation"

### For Configuration Changes
- **Portal flag change:** Requires HIL re-validation + manual sign-off
- **Stack size reduction:** Requires proof (test case, profiling data) that budget is safe
- **New malloc path:** Requires code review + dynamic analysis (advanced)

---

## 7. Roadmap: Future Enhancements

### Phase 2 (Post-Release)
- [ ] Automated map file analysis (section sizes, symbol profiling)
- [ ] Compiler-generated stack usage reports (`-fstack-usage`)
- [ ] DWARF debug info parsing for runtime watermark prediction
- [ ] Machine-learned panic signature detection

### Phase 3 (12+ Months)
- [ ] Continuous heap profiling (soak test automation)
- [ ] Automated mesh stress testing (100+ node simulation)
- [ ] Portal load testing (concurrent HTTP + WS clients)

---

## 8. References & Evidence

**Historical regressions caught by spec:**
- Portal init heap exhaustion (manifested as watchdog timeout)
- Underprovisioned portal task stacks (caused heap corruption)

**Professional standards:**
- Zephyr RTOS: Similar memory and stack gating
- Nordic nRF SDK: Map file analysis + _Static_assert patterns
- Espressif ESP-IDF: RAM threshold reporting (inherited by PlatformIO)

**Files modified:**
- `tools/preupload_gate.sh` — Enhanced with Checks 3-10
- `lib/config/include/config/build.h` — Stack budgets already defined (no change needed)

---

## Summary Table: Gate Checks

| # | Check | Type | Fail-Closed | Automated | Status |
|---|-------|------|-------------|-----------|--------|
| 1 | Native unit tests | Logic | Yes | ✓ | Pass |
| 2 | Build all envs | Sanity | Yes | ✓ | Pass |
| 3 | Memory thresholds | Dynamic | Yes | ✓ | Pass (75%) |
| 4 | Flash warnings | Warning | No | ✓ | Warn |
| 5 | Portal config | Config | Yes | ✓ | Pass (disabled) |
| 6 | Stack budgets | Static | Yes | ✓ | Pass (8 defined) |
| 7 | Console config | Config | Yes | ✓ | Pass |
| 8 | Portal init path | Config | Yes | ✓ | Pass |
| 9 | Crash signatures | Regex | Yes | ✓ | Pass |
| 10 | Startup patterns | Pattern | Yes | ✓ | Pass |
| — | Manual HIL | Runtime | No | ✗ | Weekly |

---

**Document Owner:** Firmware Engineering  
**Last Reviewed:** 2025-03-23  
**Next Review:** Before first portal re-enable
