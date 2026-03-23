# Roadmap Alignment Audit (internal pilot pass)

**Date:** 2026-03-23  
**Todo:** `audit-roadmap-alignment-pass`  
**Scope audited:** roadmap/docs vs current implementation, with focus on portal path migration, mesh/audio refactors, demo-mode behavior, and test-suite expansion.

## Alignment matrix

| Area | Expected (roadmap/plan) | Actual (implementation/docs) | Evidence path(s) | Severity | Recommended update |
|---|---|---|---|---|---|
| Portal UI migration status | Portal plan says Astro migration is pending. | Astro is already in-tree (`portal-ui/src`, Astro scripts), but plan still marks migration pending. | `docs/history/superseded-plans/portal-interface-plan.md:697-703`; `lib/control/portal-ui/package.json:7-14`; `lib/control/portal-ui/src/pages/index.astro:1-17` | **Medium** | Update portal plan status to “Astro migration complete”; keep only remaining polish tasks as pending. |
| Portal build path/tooling docs | Build docs should reflect active workflow and package manager. | Roadmap uses `npm install && npm run build`; portal plan uses `pnpm`; repo has Astro scripts with no lockfile consistency guidance. | `docs/roadmap/implementation-roadmap.md:152-153`; `docs/history/superseded-plans/portal-interface-plan.md:555-562,612-613`; `lib/control/portal-ui/package.json:7-10` | **Medium** | Standardize one command path in roadmap + README (prefer `pnpm` or explicitly support both). Add one canonical “pilot release” asset build checklist. |
| Mesh refactor status | Mesh split plan is marked planned/design-only. | Mesh split is implemented: façade `mesh_net.c` + extracted modules + CMake registration. | `docs/history/superseded-plans/meshnet-refactor-split-plan.md:1-4`; `lib/network/src/mesh_net.c:1-4`; `lib/network/CMakeLists.txt:2-16` | **Low** | Mark plan as implemented (or superseded), and point roadmap baseline to modular mesh source layout. |
| Audio refactor status | ADF split plan is marked planned/design-only. | ADF split is implemented: thin API wrapper + core/tx/rx/fft modules. | `docs/history/superseded-plans/adf-pipeline-split-plan.md:4-5`; `lib/audio/src/adf_pipeline.c:1-70`; `lib/audio/src/adf_pipeline_core.c`; `lib/audio/src/adf_pipeline_tx.c`; `lib/audio/src/adf_pipeline_rx.c`; `lib/audio/src/adf_pipeline_fft.c` | **Low** | Mark as implemented and replace “planned split” language with current architecture summary. |
| Portal serving scope (pilot ops) | Portal plan says root-only (TX/COMBO), RX does not serve portal. | RX currently initializes portal too; README says portal runs on SRC and OUT nodes; this conflicts with root-only plan text. | `docs/history/superseded-plans/portal-interface-plan.md:18-26,711`; `src/rx/main.c:310-316`; `src/tx/main.c:152-160`; `src/combo/main.c:167-175`; `README.md:59` | **High** | Decide single pilot behavior (root-only or all-node portal). Then align code guards, README, and portal plan in one pass. |
| Node role naming consistency | Roadmap says source/output class reflected in runtime IDs/telemetry. | Build label uses SRC/OUT, but node role in serialized status JSON still emits TX/RX; contract tests enforce TX/RX roles. | `docs/roadmap/implementation-roadmap.md:73`; `lib/control/src/portal_state.c:287-296,440-445`; `test/native/test_portal_api_contract/main.c:487,610-641` | **High** | Unify role vocabulary for pilot UI/API (prefer SRC/OUT externally). Update serializer, tests, and any UI fallback/normalization docs together. |
| Demo-mode behavior transparency | Plan mentions demo mode in prototype notes. | UI auto-switches to demo mode after repeated WS failures (>=3 attempts), which can mask real connectivity failures during pilot ops unless explicitly surfaced. | `docs/history/superseded-plans/portal-interface-plan.md:690`; `lib/control/portal-ui/public/app.js:650-655,705-713` | **High** | Gate auto-demo behind explicit flag for pilot builds, or add prominent “LIVE vs DEMO” hard indicator + disable control actions in demo. |
| Test-suite expansion alignment | API contract plan proposes scaffold with optional later split files. | Monolithic `test_portal_api_contract` now includes broad status/ws/uplink/ota contract tests; plus new mesh/query, sequence, dedupe, frame, uplink tests. | `docs/history/superseded-plans/api-contract-test-design.md:175-179`; `test/native/test_portal_api_contract/main.c:840-862`; `test/native/test_mesh_queries/main.c`; `test/native/test_sequence_tracker/main.c`; `test/native/test_mesh_dedupe/main.c`; `test/native/test_frame_codec/main.c`; `test/native/test_uplink_control/main.c` | **Aligned (positive)** | Keep monolithic test if preferred, but update plan text to reflect current implemented test topology and IDs. |
| Portal memory gate docs | Plan states portal starts only if free heap >64KB. | Runtime gate is 30KB (`PORTAL_MIN_FREE_HEAP`), and init logs enforce this lower threshold. | `docs/history/superseded-plans/portal-interface-plan.md:593`; `lib/config/include/config/build.h:228`; `lib/control/src/usb_portal_netif.c:181-184` | **Medium** | Update plan threshold to 30KB (or raise code threshold); keep one authoritative value and rationale. |

## Major drift findings

1. **Operational scope drift:** Portal root-only expectation conflicts with code/README enabling RX portal.
2. **External contract drift:** Telemetry role labels are mixed (`SRC/OUT` vs `TX/RX`) across API payloads/tests/UI assumptions.
3. **Reliability visibility drift:** Demo fallback can silently replace live state after WS failures.
4. **Documentation lag:** mesh/audio split plans still read “planned” although implementation is modularized.

## Pilot-readiness prioritized fixes (top 3)

1. **Lock portal scope for pilot (High):** choose root-only or all-node, then align code + docs + tests in one PR.  
2. **Normalize role labels end-to-end (High):** publish one external vocabulary (`SRC/OUT` recommended) across `/api/status`, portal rendering, and contract tests.  
3. **Harden demo-mode behavior (High):** disable auto-demo by default for pilot or show blocking LIVE/DEMO banner and prevent control actions in demo mode.

## Validation run during audit

- `pio test -e native` ✅ (84/84 passed)
- `pio run -e tx && pio run -e rx && pio run -e combo` ✅

## Notes

- No firmware logic changes were made in this audit pass; this document records alignment findings and recommended follow-up actions.
