# Architecture Maintainability Audit

## Scope

This pass audits maintainability after recent refactors across:

- `lib/audio`
- `lib/network`
- `lib/control`
- `lib/config`
- `src/{tx,rx,combo}`

Assessment dimensions: module boundaries, coupling, state ownership, file complexity, naming consistency, and layering discipline.

## Scorecard (0-10)

| Subsystem | Score | Summary |
| --- | ---: | --- |
| `lib/audio` | 7.0 | Clear internal split (`adf_pipeline_core/tx/rx/fft`) but still directly coupled to network API. |
| `lib/network` | 4.5 | Good feature split under `src/mesh/`, but maintainability is heavily reduced by globally shared mutable state. |
| `lib/control` | 6.0 | Functional separation exists (display, portal, buttons), but large files and cross-layer reach-ins remain. |
| `lib/config` | 9.0 | Clean, focused constants/pins ownership with low complexity. |
| `src/{tx,rx,combo}` | 5.5 | Entry-point orchestration is understandable, but state handling in `rx/main.c` is still dense and callback-coupled. |
| **Overall** | **6.0** | Refactors improved structure, but boundary discipline and shared-state control still threaten long-term velocity. |

## Boundary and Layering Findings

### [CRITICAL] Network state ownership is global and unconstrained

- `lib/network/src/mesh/mesh_state.h:16-67` exposes broad mutable globals (`is_mesh_connected`, `mesh_layer`, counters, callback pointers, shared buffers).
- `lib/network/src/mesh/mesh_state.c:4-63` defines these globals centrally; cross-file mutation is unrestricted.
- Consequence: hidden coupling, hard-to-test behavior, race-risk under task concurrency, difficult refactors.

### [CRITICAL] Audio layer is still directly coupled to network transport details

- `lib/audio/src/adf_pipeline_tx.c:6` includes `network/mesh_net.h`.
- `lib/audio/src/adf_pipeline_tx.c:247-267` builds network frame headers and calls `network_send_audio(...)` directly.
- Consequence: audio pipeline cannot evolve independently from mesh protocol/API changes; test seams are weak.

### [HIGH] Control layer reaches into audio pipeline internals for portal rendering

- `lib/control/src/portal_state.c:4` includes `audio/adf_pipeline.h`.
- `lib/control/src/portal_state.c:396-399` directly pulls FFT bins via `adf_pipeline_get_latest_fft_bins(...)`.
- Consequence: control/portal data model is tightly bound to a specific audio implementation detail.

### [HIGH] Internal network headers are externally exposed by build config

- `lib/network/CMakeLists.txt:17` uses `INCLUDE_DIRS "include" "src"`.
- This makes internal `lib/network/src/mesh/*.h` discoverable outside the network component boundary.
- Consequence: boundary breaks can spread over time; private implementation details become accidental API.

## Complexity Hotspots

### [HIGH] File-size and concern concentration hotspots

- `lib/control/src/display_ssd1306.c` — **769 LOC**
- `lib/control/src/portal_state.c` — **516 LOC**
- `src/rx/main.c` — **462 LOC**
- `lib/audio/src/adf_pipeline_tx.c` — **292 LOC**

These files mix multiple responsibilities (device IO + rendering, topology + JSON serialization, callback handling + app state), increasing change blast radius.

### [MEDIUM] Dense stateful orchestration in RX app entry

- `src/rx/main.c:93-114` contains static shared state (`receiving_from_src_id`, `min_raw_delta`, `ewma_oneway_ms`) used in callback-driven flow.
- Consequence: state transitions are harder to reason about and validate in isolation.

## Naming and Organization

### [MEDIUM] Naming consistency is strong in audio/network, weaker in control surface

- Audio/network have stable module prefixes (`adf_pipeline_*`, `mesh_*`, `network_*`).
- Control APIs are broader and mixed (`display_*`, `portal_*`, `dashboard_*`, `usb_portal_*`) without a tighter namespace contract.
- Consequence: discoverability and ownership boundaries degrade as control features expand.

## Practical Remediation Backlog

| Priority | Severity | Item | Concrete action | Effort |
| --- | --- | --- | --- | --- |
| P0 | CRITICAL | Encapsulate mesh runtime state | Replace exported globals with accessor/mutator API in network module; keep storage private to `mesh_state.c`; gate writes through explicit functions. | 3-5 days |
| P0 | CRITICAL | Introduce audio transport adapter | Move frame construction/send behind `audio_transport_if` callbacks injected at pipeline init; remove direct `network_send_audio` usage from audio tasks. | 2-4 days |
| P1 | HIGH | Remove control→audio direct FFT pull | Shift FFT propagation to push model (audio publishes snapshot/event, control subscribes). | 1-2 days |
| P1 | HIGH | Enforce internal header privacy | Change `lib/network/CMakeLists.txt` to export only public `include/`; use private include paths for `src/mesh`. | 0.5 day |
| P1 | HIGH | Split display monolith | Separate SSD1306 driver primitives, view rendering, and layout formatting into focused files. | 2-3 days |
| P2 | MEDIUM | Isolate portal serialization concerns | Split topology collection vs JSON composition in `portal_state.c`; add formatter helpers. | 1-2 days |
| P2 | MEDIUM | Formalize RX runtime state API | Wrap RX callback-shared state in one struct with explicit update/read helpers. | 1 day |
| P3 | MEDIUM | Control naming harmonization | Define/control namespace conventions and align new control modules to a common prefixing pattern. | 0.5-1 day |

## Recommended Execution Order

1. **State boundary hardening** (mesh global encapsulation + private headers)
2. **Audio/network decoupling seam** (transport adapter)
3. **Control/audio boundary cleanup** (FFT subscription)
4. **Complexity reduction** (`display_ssd1306.c`, `portal_state.c`)

This order reduces architectural risk first, then shrinks ongoing maintenance cost.
