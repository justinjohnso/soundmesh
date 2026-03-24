# SRC/OUT Rename Execution Plan

**Status:** Ready for review  
**Created:** 2026-03-23  
**Current state:** Fresh start — no rename work completed

## Overview

### Why this rename?

The codebase currently uses three build flavors (tx, rx, combo) but only two are actually deployed:
- **COMBO** → **SRC** (Source node): Captures audio, encodes, mesh broadcasts, has portal + headphone monitor
- **RX** → **OUT** (Output node): Receives mesh audio, decodes, plays via DAC
- **TX** → Delete (unused legacy code — COMBO does everything TX did plus monitoring)

### Critical distinction: Node identity vs. data flow direction

This rename addresses **node identity only** — the type of device and its build configuration.

**What changes (node identity):**
- `CONFIG_COMBO_BUILD` → `CONFIG_SRC_BUILD`
- `CONFIG_RX_BUILD` → `CONFIG_OUT_BUILD`
- `combo_status_t` → `src_status_t`
- `NODE_ROLE_TX` → `NODE_ROLE_SRC`
- Display labels: "COMBO" → "SRC", "RX" → "OUT"

**What stays (data flow direction):**
- `ADF_PIPELINE_TX` / `ADF_PIPELINE_RX` — audio pipeline stages
- `tx_capture_task`, `rx_playback_task` — FreeRTOS task names
- `tx_seq`, `last_rx_seq` — protocol sequence tracking fields
- `TX_TEST_TONE_MODE`, `RX_TEST_TONE_MODE` — debug flags
- `network_send_audio()`, `mesh_rx_buffer` — network direction semantics

**Rationale:** TX/RX are universal audio/networking terms for transmission direction. Renaming these would create semantic confusion. We only rename symbols that describe *what the device is*, not *which direction data flows*.

## Strategy

### Architectural principle: Single source of truth

Instead of scattering `#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)` across the codebase, we create **one central header** that defines node identity and capabilities. Future product name changes happen in one place.

New file: `lib/config/include/config/build_role.h`
```c
#pragma once

#if defined(CONFIG_SRC_BUILD)
  #define BUILD_IS_SOURCE         1
  #define BUILD_IS_OUTPUT         0
  #define BUILD_NODE_LABEL        "SRC"
  #define BUILD_HAS_CAPTURE       1
  #define BUILD_HAS_ENCODER       1
  #define BUILD_HAS_LOCAL_MONITOR 1
  #define BUILD_HAS_PORTAL        1
  #define BUILD_IS_MESH_ROOT      1

#elif defined(CONFIG_OUT_BUILD)
  #define BUILD_IS_SOURCE         0
  #define BUILD_IS_OUTPUT         1
  #define BUILD_NODE_LABEL        "OUT"
  #define BUILD_HAS_CAPTURE       0
  #define BUILD_HAS_ENCODER       0
  #define BUILD_HAS_LOCAL_MONITOR 0
  #define BUILD_HAS_PORTAL        0
  #define BUILD_IS_MESH_ROOT      0

#else
  #error "Unknown build flavor. Define CONFIG_SRC_BUILD or CONFIG_OUT_BUILD."
#endif
```

**Benefits:**
1. Changes propagate automatically — rename SRC→MAIN in one place
2. Capability flags are self-documenting — `BUILD_HAS_PORTAL` is clearer than checking env
3. Type-safe at compile time — typos in env checks cause build failures
4. CMakeLists.txt can still check raw CONFIG_* when C headers aren't available

### Migration pattern

**Old (scattered 14 occurrences):**
```c
#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)
    portal_init();
#endif
```

**New (centralized):**
```c
#include "config/build_role.h"

#if BUILD_HAS_PORTAL
    portal_init();
#endif
```

### Risk mitigation

- **Small commits per phase** — each phase builds and passes tests before next
- **Verification at every step** — run `pio run -e src && pio run -e out` after each phase
- **Git revert-friendly** — phases are independent, can rollback individually
- **No runtime behavior changes** — pure compile-time rename, zero functional impact



## Implementation Phases

### Phase 1: Build system foundation
**Goal:** Rename environments and configs, establish new build identity flags

**Files to modify:**
- `platformio.ini`: Delete `[env:tx]`, rename `[env:combo]` → `[env:src]`, `[env:rx]` → `[env:out]`
  - Update `default_envs = src`
  - Change -D flags: `CONFIG_COMBO_BUILD` → `CONFIG_SRC_BUILD`, `CONFIG_RX_BUILD` → `CONFIG_OUT_BUILD`
- `sdkconfig.combo.defaults` → rename to `sdkconfig.src.defaults`
- `sdkconfig.tx.defaults` → delete
- `sdkconfig.rx.defaults` → keep as-is (no internal changes needed)
- `extra_script.py`: Update env name checks (`"combo"` → `"src"`, `"tx"` → delete, `"rx"` → `"out"`)
- `CMakeLists.txt` (root): Change fallback from `"tx"` → `"src"`

**Verification:** `pio run -e src --target clean` should work

### Phase 2: Central build role header
**Goal:** Create single source of truth for node identity and capabilities

**Action:** Create `lib/config/include/config/build_role.h` (see strategy section for content).

**Verification:** Include in one test file and compile.


Create `lib/config/include/config/build_role.h`:
```c
#pragma once

#if defined(CONFIG_SRC_BUILD)
  #define BUILD_IS_SOURCE         1
  #define BUILD_IS_OUTPUT         0
  #define BUILD_NODE_LABEL        "SRC"
  #define BUILD_HAS_CAPTURE       1
  #define BUILD_HAS_ENCODER       1
  #define BUILD_HAS_LOCAL_MONITOR 1
  #define BUILD_HAS_PORTAL        1
  #define BUILD_IS_MESH_ROOT      1

#elif defined(CONFIG_OUT_BUILD)
  #define BUILD_IS_SOURCE         0
  #define BUILD_IS_OUTPUT         1
  #define BUILD_NODE_LABEL        "OUT"
  #define BUILD_HAS_CAPTURE       0
  #define BUILD_HAS_ENCODER       0
  #define BUILD_HAS_LOCAL_MONITOR 0
  #define BUILD_HAS_PORTAL        0
  #define BUILD_IS_MESH_ROOT      0

#else
  #error "Unknown build flavor"
#endif
```

Verification: Include in one test file and compile

### Phase 3: Migrate preprocessor guards
**Goal:** Replace scattered `CONFIG_*` checks with centralized `BUILD_*` macros

**Files with CONFIG_TX_BUILD || CONFIG_COMBO_BUILD (replace with BUILD_IS_SOURCE or BUILD_HAS_PORTAL):**
- `lib/control/src/usb_portal_netif.c` (2 occurrences)
- `lib/audio/src/usb_audio.c` (2 occurrences)
- `lib/control/src/portal_state.c` (4 occurrences)
- `lib/network/src/mesh/mesh_identity.c` (1)
- `lib/network/src/mesh/mesh_init.c` (1)

**Files with CONFIG_RX_BUILD (replace with BUILD_IS_OUTPUT):**
- Similar pattern across `lib/`

**CMakeLists.txt files (can't use C headers):**
- `lib/network/CMakeLists.txt`: Update to check `CONFIG_SRC_BUILD`

**Verification:** `pio run -e src && pio run -e out`

### Phase 4: Rename node-identity types & functions
**Goal:** Update type names, function names, enum values to match SRC/OUT

**Headers:**
- `lib/control/include/control/status.h`:
  - Delete `tx_status_t` (unused legacy)
  - Rename `combo_status_t` → `src_status_t`
  - Rename `rx_status_t` → `out_status_t`

- `lib/control/include/control/display.h`:
  - Delete `display_render_tx()` declaration
  - Rename `display_render_combo()` → `display_render_src()`
  - Rename `display_render_rx()` → `display_render_out()`

- `lib/network/src/mesh/mesh_state.h`:
  - Rename `NODE_ROLE_TX` → `NODE_ROLE_SRC`
  - Rename `NODE_ROLE_RX` → `NODE_ROLE_OUT`

**Implementations:**
- `lib/control/src/display_ssd1306.c`: Rename functions + update "COMBO"/"RX" strings
- `lib/control/src/serial_dashboard.c`: Rename functions + header strings
- `lib/control/src/portal_state.c`: Update JSON `"TX"` → `"SRC"`, `"RX"` → `"OUT"`
- `lib/network/src/mesh/mesh_*.c`: Update `NODE_ROLE` enum usage, log strings
- `lib/config/include/config/build.h`: Update comments, rename `RX_OUTPUT_VOLUME` → `OUT_OUTPUT_VOLUME`

**Main entry points:**
- `src/combo/main.c`: Update to use `src_status_t`, call `display_render_src()`
- `src/rx/main.c`: Update to use `out_status_t`, call `display_render_out()`

**Verification:** `pio run -e src && pio run -e out` + check serial output for correct labels

### Phase 5: Rename source directories
**Goal:** Match directory names to new build environment names

**Commands:**
```bash
mv src/combo src/src
mv src/rx src/out
rm -rf src/tx
```

**Updates:** Update `src/CMakeLists.txt` to reference new paths.

**Verification:** `pio run -e src && pio run -e out`

### Phase 6: Portal UI updates
**Goal:** Update web interface to use SRC/OUT terminology

**Files in `lib/control/portal-ui/`:**
- `public/app.css`:
  - CSS vars: `--tx` → `--src`, `--rx` → `--out`
  - Classes: `.node-rx` → `.node-out`, `.node-root` stays (or becomes `.node-src`)
  
- `public/app.js`:
  - Search/replace role labels `'TX'` → `'SRC'`, `'RX'` → `'OUT'`
  - Update any default/fallback values

- `src/components/PortalFooter.astro`:
  - Use dynamic `buildLabel` from state instead of hardcoded `"TX v1.0.1"`

**Rebuild portal:**
```bash
cd lib/control/portal-ui
pnpm install
pnpm run build
cd ../../..
pio run -e src -t uploadfs
```

**Verification:** Load portal at `http://10.48.x.x` and verify labels show SRC/OUT

### Phase 7: VS Code configuration
**Goal:** Update IDE tasks to use new environment names

**`.vscode/tasks.json`:**
- Delete all "TX:" tasks
- Rename "COMBO:" → "SRC:" (update `-e combo` → `-e src`)
- Rename "RX:" → "OUT:" (update `-e rx` → `-e out`)
- Update "Build All" to `pio run -e src -e out`

**`.vscode/launch.json`:**
- Update `projectEnvName`: `"tx"` → `"src"`
- Update firmware path: `.pio/build/tx/` → `.pio/build/src/`

**Verification:** Run a task from VS Code command palette

### Phase 8: Documentation updates
**Goal:** Update all active docs; mark historical docs clearly

**Active docs (must update):**
- `AGENTS.md`: Full update for build commands, file paths, sdkconfig hierarchy
- `README.md`: Update quick reference build commands
- `docs/roadmap/implementation-roadmap.md`: Reference this rename as completed

**Historical docs (leave as-is):**
- All files in `docs/history/` — these are historical records

**Verification:** Grep for remaining TX/RX references in active docs

### Phase 9: Cleanup & final verification
**Goal:** Remove stale artifacts and run full test suite

**Cleanup:**
```bash
rm -f sdkconfig.tx sdkconfig.rx sdkconfig.combo  # Stale caches
rm -rf .pio/build/tx .pio/build/combo .pio/build/rx  # Old build dirs
pio run -e src -t clean
pio run -e out -t clean
```

**Full verification suite:**
```bash
# Unit tests
pio test -e native

# Full builds
pio run -e src && pio run -e out

# Portal build + SPIFFS
cd lib/control/portal-ui && pnpm run build && cd ../../..
pio run -e src -t uploadfs

# Verify no remaining old identifiers
grep -r "CONFIG_COMBO_BUILD\|CONFIG_TX_BUILD" lib/ src/ --exclude-dir=.pio | grep -v "^Binary"
```

**Expected:** All tests pass, both envs build clean, no old config flags remain

## Rollback Strategy

Each phase is a separate commit. If issues arise:
1. Identify failing phase via git log
2. `git revert <commit-hash>` for that phase
3. Fix issues and retry

**Optional safety net:** Add backward-compat aliases in `build_role.h` during migration (removed in cleanup phase):
```c
// Temporary compat
#define CONFIG_COMBO_BUILD CONFIG_SRC_BUILD
#define CONFIG_TX_BUILD    CONFIG_SRC_BUILD
```
