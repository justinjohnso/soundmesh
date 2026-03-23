# meshnet-refactor-split-plan

> Status: superseded — split has been implemented; see `docs/architecture/network.md` for active architecture reference and `docs/roadmap/implementation-roadmap.md` for current execution priorities.

## Scope
Create a compile-safe split plan for `lib/network/src/mesh_net.c` while keeping the external API in `lib/network/include/network/mesh_net.h` unchanged.

## Non-negotiable constraints
- Keep all public symbols and signatures in `mesh_net.h` stable.
- Preserve runtime sequencing semantics:
  - tasks (`mesh_rx`, `mesh_hb`) are created before `esp_mesh_start()`
  - startup notifications still fire on the same readiness events (`MESH_EVENT_STARTED` for designated root path, `MESH_EVENT_PARENT_CONNECTED`, `MESH_EVENT_ROOT_FIXED`, root switch path)
  - `mesh_rx_task` still gates receive loop on root-ready/connected state
- No behavior changes in send flags (`MESH_DATA_GROUP`, `MESH_DATA_P2P`, `MESH_DATA_TODS`, `MESH_DATA_NONBLOCK`) during extraction.

## Current section map (old file)
Source: `lib/network/src/mesh_net.c`

| Old section (line range) | Current ownership | Notes |
|---|---|---|
| 19-107 | global role/state/cache/task handles | Cross-cutting state and constants |
| 109-132 | ID helpers (`mesh_id_from_string`, `derive_src_id`, `network_get_src_id`) | Utility + public getter |
| 134-324 | callbacks + dedupe + uplink helpers | Utility + control-plane config |
| 325-564 | `mesh_event_handler` | Mesh event state machine |
| 568-722 | `mesh_rx_task` packet dispatch/parsing | Data-plane RX loop |
| 724-831 | ping/pong helpers + `network_send_ping` | Latency path |
| 833-988 | `network_init_mesh` | Wi-Fi/mesh bootstrap + task creation |
| 990-1104 | heartbeat + stream announce + heartbeat task | Periodic control plane |
| 1108-1125 | startup notification registration | startup sync |
| 1133-1246 | `network_send_audio`, `network_send_control` | TX data/control plane |
| 1249-1481 | query/getter APIs + callbacks + uplink API | external API implementations |

## Target module/file map
Keep `mesh_net.c` as a stable façade while extracting internals into focused translation units.

### Public façade (unchanged API)
- `lib/network/src/mesh_net.c`
  - Keeps all `network_*` public function definitions from `mesh_net.h`
  - Delegates to internal modules
  - Contains no complex mesh event logic once extraction completes

### New internal files
- `lib/network/src/mesh/mesh_state.c`
- `lib/network/src/mesh/mesh_state.h`
  - Owns all mutable shared state currently at top of `mesh_net.c`
  - Provides typed accessors/mutators for state transitions
  - Owns `g_src_id`, role, connection flags, counters, cached root/parent/child addresses, task notification slots, uplink status blob

- `lib/network/src/mesh/mesh_identity.c`
- `lib/network/src/mesh/mesh_identity.h`
  - `mesh_id_from_string`, `derive_src_id`, source-ID formatting helpers

- `lib/network/src/mesh/mesh_uplink.c`
- `lib/network/src/mesh/mesh_uplink.h`
  - `apply_uplink_router_config`, `publish_uplink_sync`, `request_uplink_sync_from_root`, `handle_uplink_control`
  - root-vs-child uplink sync semantics

- `lib/network/src/mesh/mesh_events.c`
- `lib/network/src/mesh/mesh_events.h`
  - `mesh_event_handler`
  - pure event-to-state transitions and notifications

- `lib/network/src/mesh/mesh_rx.c`
- `lib/network/src/mesh/mesh_rx.h`
  - `mesh_rx_task`
  - packet type dispatch and frame decode callback path
  - dedupe checks delegated to `mesh_dedupe.c`

- `lib/network/src/mesh/mesh_dedupe.c`
- `lib/network/src/mesh/mesh_dedupe.h`
  - `is_duplicate`, `mark_seen`, cache reset/init

- `lib/network/src/mesh/mesh_ping.c`
- `lib/network/src/mesh/mesh_ping.h`
  - `handle_ping`, `handle_pong`, `send_pong`
  - `network_send_ping`, `network_ping_nearest_child`

- `lib/network/src/mesh/mesh_tx.c`
- `lib/network/src/mesh/mesh_tx.h`
  - `network_send_audio`, `network_send_control`
  - root group fanout and child TODS semantics

- `lib/network/src/mesh/mesh_heartbeat.c`
- `lib/network/src/mesh/mesh_heartbeat.h`
  - `send_heartbeat`, `send_stream_announcement`, `mesh_heartbeat_task`

- `lib/network/src/mesh/mesh_init.c`
- `lib/network/src/mesh/mesh_init.h`
  - `network_init_mesh` internals only (Wi-Fi/NVS/netif/mesh config)
  - task creation ordering preserved exactly

- `lib/network/src/mesh/mesh_queries.c`
- `lib/network/src/mesh/mesh_queries.h`
  - topology/status getters and jitter-prefill logic

## Shared state strategy
Use a single internal state owner with narrow accessor APIs.

### State container
`mesh_state.h` defines:
- `typedef struct meshRuntimeState { ... } meshRuntimeState_t;`
- `meshRuntimeState_t *mesh_state_get(void);`

State includes:
- role/identity (`my_node_role`, `my_stream_id`, `my_sta_mac`, `g_src_id`)
- connection/root readiness flags
- topology and latency values
- ping sequencing + pending IDs
- diagnostics counters
- cached root/child addresses
- callback pointers
- startup waiting task slots
- uplink status/password cache
- tx counters / drop stats

### Concurrency policy
- Single-writer-by-context policy preserved from current behavior:
  - event handler writes connectivity/topology state
  - rx task writes receive-derived state (nearest child updates when root)
  - tx/heartbeat paths perform read-mostly access
- No mutex added in extraction phase unless required by observed races; preserve current timing and lock-free behavior first.
- Any new helper that mutates shared fields must be called from existing task/event contexts only.

## Old -> New ownership map

| Old location in `mesh_net.c` | New file |
|---|---|
| Global state block (19-107) | `mesh/mesh_state.c` |
| Identity helpers (109-132) | `mesh/mesh_identity.c` |
| Dedupe helpers (197-213) | `mesh/mesh_dedupe.c` |
| Uplink helper block (215-324) | `mesh/mesh_uplink.c` |
| Event handler (325-564) | `mesh/mesh_events.c` |
| RX task + packet dispatch (568-722) | `mesh/mesh_rx.c` |
| Ping/pong block (724-831, 1368-1426) | `mesh/mesh_ping.c` |
| Mesh init (833-988) | `mesh/mesh_init.c` |
| Heartbeat + announcement + task (990-1104) | `mesh/mesh_heartbeat.c` |
| Startup registration (1108-1125) | `mesh/mesh_state.c` (notification API) |
| Audio/control send (1133-1246) | `mesh/mesh_tx.c` |
| Query/getter APIs (1249-1367, 1428-1481) | `mesh/mesh_queries.c` + façade wrappers |

## Compile-safe extraction sequence (low behavioral risk)

1. **Introduce internal state module (no behavior change)**
   - Add `mesh_state.[ch]` with struct mirroring existing globals.
   - Keep existing globals in `mesh_net.c` as temporary aliases/macros to state fields.
   - Build all envs.

2. **Extract pure helpers first**
   - Move `mesh_id_from_string`, `derive_src_id`, `wifi_disconnect_reason_to_str`, dedupe helpers.
   - No event/task flow touched.
   - Build all envs.

3. **Extract uplink control helpers**
   - Move uplink functions to `mesh_uplink.c`.
   - Keep call sites unchanged.
   - Build all envs.

4. **Extract TX send path (`network_send_audio/control`)**
   - Move logic into `mesh_tx.c` and keep API wrappers in `mesh_net.c` forwarding directly.
   - Preserve exact mesh flags and return-code handling.
   - Build all envs.

5. **Extract query/getter logic**
   - Move getter internals into `mesh_queries.c`; façade wrappers remain in `mesh_net.c`.
   - Build all envs.

6. **Extract ping/pong block**
   - Move ping helpers and APIs to `mesh_ping.c`.
   - Keep same pending-ID semantics/timeouts.
   - Build all envs.

7. **Extract heartbeat task path**
   - Move `send_heartbeat`, `send_stream_announcement`, `mesh_heartbeat_task` to `mesh_heartbeat.c`.
   - Keep startup notify handshake unchanged.
   - Build all envs.

8. **Extract RX task parser**
   - Move `mesh_rx_task` to `mesh_rx.c` while preserving packet dispatch order and header compatibility handling.
   - Build all envs.

9. **Extract mesh event handler**
   - Move `mesh_event_handler` to `mesh_events.c` last (highest risk section).
   - Preserve per-event side effects and notify timing.
   - Build all envs.

10. **Extract init/bootstrap last**
   - Move `network_init_mesh` internals to `mesh_init.c` only after all callee modules are stable.
   - Keep wrapper in `mesh_net.c` until final cleanup.
   - Build all envs.

11. **Final façade cleanup**
   - `mesh_net.c` retains API wrappers and module wiring only.
   - Remove temporary aliases.
   - Build all envs + hardware smoke test.

## Validation gates per extraction step
- Compile gate (mandatory every step):
  - `pio run -e tx && pio run -e rx && pio run -e combo`
- Runtime smoke gate (after steps 8-10):
  - root boot (TX/COMBO) + RX join
  - startup notification still releases waiting tasks
  - audio frame RX callback fires
  - ping/pong RTT updates
  - heartbeat task continues 5s cadence

## Key risks and mitigations
- **Risk: event-order regression in readiness notifications**
  - Mitigation: snapshot current notify call sites and keep identical event-trigger points; extraction of event handler is step 9 (late).

- **Risk: mesh send semantic drift (`GROUP/P2P/TODS`)**
  - Mitigation: isolate send flags into constants/macros in `mesh_tx.c`; unit-compare wrapper behavior before/after.

- **Risk: shared-state divergence across modules**
  - Mitigation: single `mesh_state` owner + no duplicated statics in extracted files.

- **Risk: RX parser regressions (v1/v2 header compatibility, batch unpack)**
  - Mitigation: move parser as one intact block; do not refactor logic while relocating.

- **Risk: hidden coupling between init and event paths**
  - Mitigation: extract `network_init_mesh` last; keep task-creation order and registration order byte-for-byte equivalent.

## CMake migration plan
Current `lib/network/CMakeLists.txt` registers only:
- `src/mesh_net.c`
- `src/frame_codec.c`
- `src/uplink_control.c`

During extraction, incrementally append each new source file so every step remains buildable; do not remove `src/mesh_net.c` until final stage (if ever).

## Definition of done for this plan item
- This document exists and is linked from planning/progress context.
- Contains target module map, old-to-new ownership map, shared-state strategy, extraction order, compile-safe gates, and risk mitigations.
- External API stability and task/event ordering invariants are explicit.
