# Overflow/Crash Gate - Quick Reference Card

## What Changed

| Item | Before | After |
|------|--------|-------|
| Script lines | 81 | 114 |
| Checks | 6 | 10 |
| Memory validation | ❌ | ✅ |
| Stack budgets | ❌ | ✅ |
| Build time | ~35s | ~45s |

## The 10 Checks (In Order)

```
[1] Unit tests             → All tests pass
[2] Build environments    → tx/rx/combo succeed
[3] Memory threshold      → RAM < 75% of 327.68 KB [NEW]
[4] Flash warnings        → Check config (non-fatal)
[5] Portal config         → Both flags must be 0
[6] Stack budgets         → 8 tasks defined [NEW]
[7] Console config        → USB JTAG enabled [NEW]
[8] Portal init path      → Code/config match [NEW]
[9] Crash signatures      → No known patterns
[10] Startup patterns     → Watchdog-safe waits
```

## Gate Script Usage

```bash
cd /path/to/meshnet-audio
bash tools/preupload_gate.sh

# Exit code: 0 (pass) or 1 (fail)
# Output: "✓ ALL PRE-UPLOAD CRASH GATES PASSED"
```

## Key Thresholds

- **Memory:** < 75% DRAM (leaves 77 KB margin)
- **Portal HTTP stack:** ≥ 6 KB
- **Opus Encoder stack:** ≥ 32 KB
- **Portal min heap floor:** ≥ 30 KB
- **Task watermark (manual HIL):** > 512 bytes

## Configuration (lib/config/include/config/build.h)

Already set correctly:
```c
#define ENABLE_SRC_USB_PORTAL_NETWORK   0  // ✓ Disabled
#define ENABLE_OUT_USB_PORTAL_NETWORK   0  // ✓ Disabled
#define PORTAL_HTTP_STACK_BYTES      (6 * 1024)
#define PORTAL_MIN_FREE_HEAP        (30 * 1024)
#define ENCODE_TASK_STACK_BYTES    (32 * 1024)
```

## Manual HIL (If Portal Enabled)

1. **Startup (2 min):** Monitor logs, no resets
2. **HTTP (5 min):** Query /api/status 100x, all 200 OK
3. **Watermarks (1 hour):** All stacks > 512 bytes headroom
4. **Escalate if:** Watchdog reset, any stack < 256 bytes, heap declining

## Regression Example

**Scenario:** Portal enabled
- OLD: Reboots silently in field (watchdog)
- NEW: Gate fails at build → developer fixes issue

## Documentation

- **Full spec:** `docs/operations/OVERFLOW_GATE_SPECIFICATION.md`
- **Comparison:** `docs/operations/GATE_COMPARISON_BEFORE_AFTER.md`
- **Implementation:** `tools/preupload_gate.sh` (114 lines)

## Test Status

✅ All 10 gates PASSING on current codebase

```
Memory: TX=23.7%, RX=23.8%, COMBO=21.7% (all < 75%)
Stack budgets: ✓ All 8 defined
Portal config: ✓ Safe (disabled)
Crash signatures: ✓ None detected
```

## Next Steps

1. Integrate into CI/CD (GitHub Actions, etc.)
2. Train team on gate policy
3. Run manual HIL weekly (if portal enabled)
4. Log override procedures in security docs

