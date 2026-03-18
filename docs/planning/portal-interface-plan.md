# SoundMesh Portal Interface — Implementation Plan

**Date:** March 18, 2026  
**Status:** MVP Plan  
**Goal:** USB captive portal providing a "window into the network" with a high-density, three-column telemetry dashboard (Astro-based) for topology, audio analysis, and stream flow visibility

---

## Overview

When a computer is plugged into a SoundMesh node via USB, the device exposes a USB network interface (CDC-NCM). A captive portal auto-opens (best-effort) or the user navigates to `http://10.48.0.1/`. A lightweight Astro-rendered web UI shows:

1. **Audio Analysis Pane** — BPM, log-frequency FFT bars, and uptime
2. **Network Map Pane** — Live topology visualization with a "you are here" marker for the connected node
3. **Telemetry Pane** — Node status plus global stream and system health
4. **Data Flow** — Animated visualization of audio data flowing between nodes

### Scope: Root Node Only (MVP)

The portal runs **only on TX/COMBO builds** (the mesh root). Rationale:

- Root has the best global view of the mesh (routing table, all heartbeats)
- Avoids duplicating HTTP/WebSocket RAM cost on RX nodes
- Simplifies telemetry — root already aggregates all node data
- User plugs into the TX node to observe the entire network

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    USB PORTAL STACK                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Computer/Phone                                              │
│    ├── USB CDC-NCM driver (built-in on macOS/Win/Linux)     │
│    ├── Gets DHCP lease: 10.48.0.x                           │
│    ├── DNS resolves all domains → 10.48.0.1                 │
│    └── Browser → http://10.48.0.1/                          │
│                                                              │
│  ESP32-S3 (Root Node)                                        │
│    ├── TinyUSB CDC-NCM function                             │
│    ├── esp_netif (Ethernet-like interface)                   │
│    ├── lwIP stack                                            │
│    │   ├── DHCP server (10.48.0.0/24)                       │
│    │   └── DNS server (UDP/53, catch-all → 10.48.0.1)       │
│    ├── esp_http_server                                       │
│    │   ├── Static files from SPIFFS (Astro build output)     │
│    │   ├── Captive portal probe handlers                    │
│    │   └── WebSocket endpoint (/ws)                         │
│    └── Portal state aggregator                               │
│        ├── Own node telemetry                                │
│        ├── Heartbeat cache (all mesh nodes)                  │
│        └── 1 Hz JSON snapshot → WebSocket push              │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

```
Mesh nodes (heartbeats every 2s)
    → esp_mesh_recv() on root
    → Portal state cache (keyed by MAC, 32 nodes max)
    → 1 Hz JSON snapshot serialization
    → WebSocket push to connected browser
    → Astro dashboard shell + Canvas 2D layer render map + flow
```

---

## Phase 1: USB Networking (CDC-NCM + DHCP + DNS)

**Duration:** 2–3 days  
**Goal:** Plug USB into root node → computer gets network interface with IP

### 1.1 TinyUSB CDC-NCM Configuration

Create `lib/control/src/usb_portal_netif.c` and header.

**Approach:** Use TinyUSB's NCM (Network Control Model) class, which has native driver support on:
- macOS (built-in)
- Linux (built-in `cdc_ncm` kernel driver)
- Windows 10+ (built-in)
- Android (USB OTG, built-in)

**Why NCM over ECM:** NCM is the more established path in ESP-IDF TinyUSB examples and has better Windows support. ECM can be added as an alternative later.

**Key implementation details:**
- USB descriptors: CDC-NCM function with MAC address derived from ESP32 base MAC
- Register `esp_netif` as Ethernet-like interface
- RX callback: `tinyusb_net_rx_cb()` → `esp_netif_receive()`
- TX callback: `esp_netif_transmit()` → TinyUSB network TX
- Static IP: `10.48.0.1/24` on the ESP32 side
- Only initialize when build is TX or COMBO (`#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)`)

**USB endpoint budget:** ESP32-S3 has limited USB endpoints. For portal MVP, the USB config should be **network-only** — disable any USB audio stubs while portal networking is active.

### 1.2 DHCP Server

Use lwIP's built-in DHCP server (`dhcps`) on the USB netif:
- Pool: `10.48.0.2` – `10.48.0.10` (only need 1–2 clients)
- Gateway: `10.48.0.1`
- DNS: `10.48.0.1` (points to our DNS catch-all)

### 1.3 DNS Catch-All Server

Create `lib/control/src/usb_portal_dns.c` — a minimal DNS server on UDP port 53.

**Behavior:** Answer ALL A-record queries with `10.48.0.1`. This enables:
- Captive portal detection (OS probes resolve to our IP)
- Any URL typed in browser lands on the portal

**Implementation:** ~80 lines — parse DNS header, extract query, respond with fixed A record pointing to `10.48.0.1`. IPv4 only for MVP.

### 1.4 Start/Stop Lifecycle

Portal subsystem starts only when:
1. Build is TX or COMBO
2. Device is mesh root (`is_mesh_root_ready == true`)
3. Free heap > 64 KB (safety threshold)

Portal stops when:
- USB disconnects
- Root role is lost (shouldn't happen with User Designated Root, but defensive)
- Heap drops below safe threshold

**Files to create:**
```
lib/control/include/control/usb_portal.h        [NEW] — Public API
lib/control/src/usb_portal_netif.c               [NEW] — CDC-NCM + esp_netif + DHCP
lib/control/src/usb_portal_dns.c                 [NEW] — DNS catch-all server
```

**Files to modify:**
```
sdkconfig.shared.defaults                        [MODIFY] — Add DHCP server, HTTP, WS
sdkconfig.tx.defaults                            [MODIFY] — TinyUSB NCM config
sdkconfig.combo.defaults                         [MODIFY] — TinyUSB NCM config
lib/config/include/config/build.h                [MODIFY] — Portal constants
platformio.ini                                   [MODIFY] — SPIFFS partition config
partitions.csv                                   [MODIFY] — Add SPIFFS partition
```

### 1.5 Verification

```bash
# Flash TX firmware
pio run -e tx -t upload

# Plug USB into computer
# Verify: network interface appears in OS settings
# Verify: computer gets 10.48.0.x IP via DHCP
# Verify: ping 10.48.0.1 succeeds
```

---

## Phase 2: HTTP Server + Captive Portal Detection

**Duration:** 1–2 days  
**Goal:** Browser auto-opens portal page (best-effort), or user navigates to `http://10.48.0.1/`

### 2.1 HTTP Server

Use ESP-IDF's built-in `esp_http_server` — proven, supports WebSocket, minimal RAM.

Create `lib/control/src/portal_http.c`:

**Configuration:**
```c
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.server_port = 80;
config.max_open_sockets = 4;       // 1 page load + 1 WS + headroom
config.max_uri_handlers = 16;
config.lru_purge_enable = true;    // Reclaim stale sockets
config.stack_size = 6144;          // 6 KB task stack
```

**Static file serving from SPIFFS:**
- Mount SPIFFS at `/spiffs`
- Serve `index.html`, `app.js`, `app.css` with correct MIME types
- Set `Cache-Control: max-age=3600` for JS/CSS (cache on browser)
- Serve pre-gzipped files if available (check for `.gz` variant, set `Content-Encoding: gzip`)

### 2.2 Captive Portal Probe Handlers

Register HTTP handlers for OS-specific captive portal detection URLs:

| OS | Probe URL | Expected Response |
|----|-----------|-------------------|
| **Android** | `GET /generate_204` | 302 redirect to `http://10.48.0.1/` |
| **Apple (macOS/iOS)** | `GET /hotspot-detect.html` | 302 redirect to `http://10.48.0.1/` |
| **Apple** | `GET /library/test/success.html` | 302 redirect |
| **Windows** | `GET /connecttest.txt` | 302 redirect |
| **Windows** | `GET /ncsi.txt` | 302 redirect |
| **Firefox** | `GET /success.txt` | 302 redirect |
| **Chrome** | `GET /canonical.html` | 302 redirect |

**Catch-all handler:** Any unrecognized path → 302 redirect to `http://10.48.0.1/`.

### 2.3 Captive Portal Reality Check

> **⚠️ Important:** Captive portal auto-open over USB Ethernet is **not guaranteed** on any OS. This is an OS-level decision based on connectivity probes, and USB network interfaces are often treated differently than WiFi. The DNS catch-all + probe handlers provide best-effort coverage.

**Guaranteed fallback:** User can always manually navigate to `http://10.48.0.1/`.

**Optional enhancement:** mDNS advertisement as `soundmesh.local` for `http://soundmesh.local/` access.

**Files to create:**
```
lib/control/src/portal_http.c                    [NEW] — HTTP server + static files + captive portal
```

### 2.4 Verification

```bash
# With USB connected and network up:
# Verify: http://10.48.0.1/ returns index.html
# Verify: captive portal popup appears on macOS (best-effort)
# Verify: http://10.48.0.1/generate_204 redirects
```

---

## Phase 3: Portal State Aggregation + WebSocket

**Duration:** 2–3 days  
**Goal:** Real-time mesh state pushed to browser at 1 Hz via WebSocket

### 3.1 Extend Heartbeat with Parent Address

The current `mesh_heartbeat_t` lacks **parent address**, which is required to draw topology edges (who is connected to whom).

**Modify `mesh_heartbeat_t`** in `mesh_net.h`:
```c
typedef struct __attribute__((packed)) {
    uint8_t type;           // 0x02 = HEARTBEAT
    uint8_t role;           // 0=RX, 1=TX
    uint8_t is_root;
    uint8_t layer;
    uint32_t uptime_ms;
    uint16_t children_count;
    int8_t rssi;
    uint8_t stream_active;  // NEW: 1 if currently streaming audio
    uint8_t parent_mac[6];  // NEW: parent node MAC (for topology edges)
    uint8_t self_mac[6];    // NEW: own MAC (for identification)
} mesh_heartbeat_t;
```

This adds 13 bytes to the heartbeat (still well under mesh MTU). The `self_mac` enables the root to build a complete node→parent adjacency map for the topology visualization.

### 3.2 Portal State Cache

Create `lib/control/src/portal_state.c` — aggregates mesh state for the web UI.

**State structure:**
```c
#define PORTAL_MAX_NODES 32

typedef struct {
    uint8_t mac[6];
    uint8_t role;           // 0=RX, 1=TX
    uint8_t is_root;
    uint8_t layer;
    int8_t rssi;
    uint16_t children_count;
    uint8_t stream_active;
    uint8_t parent_mac[6];
    uint32_t uptime_ms;
    int64_t last_seen_us;   // esp_timer_get_time() when last heartbeat received
} portal_node_t;

typedef struct {
    portal_node_t nodes[PORTAL_MAX_NODES];
    uint8_t node_count;
    uint8_t connected_node_index;  // "you are here" — index of this node
} portal_state_t;
```

**Update flow:**
1. `mesh_rx` task receives heartbeat → calls `portal_state_update(heartbeat)`
2. Root's own state updated from local metrics
3. Stale nodes (no heartbeat for 3× `CONTROL_HEARTBEAT_RATE_MS` = 6 seconds) marked stale
4. Expired nodes (no heartbeat for 30 seconds) removed from cache

### 3.3 WebSocket Endpoint

Add WebSocket support to the HTTP server at `/ws`.

**Protocol (simple, MVP):**
- Client connects to `ws://10.48.0.1/ws`
- Server sends **full JSON snapshot** every 1 second
- No client→server messages needed for MVP (read-only view)
- Support 1 concurrent WebSocket client (2 max)

**JSON snapshot format:**
```json
{
  "ts": 1234567890,
  "self": "A3:F2:01:02:03:04",
  "nodes": [
    {
      "mac": "A3:F2:01:02:03:04",
      "role": "TX",
      "root": true,
      "layer": 0,
      "rssi": 0,
      "children": 2,
      "streaming": true,
      "parent": null,
      "uptime": 384512,
      "stale": false
    },
    {
      "mac": "B1:C4:05:06:07:08",
      "role": "RX",
      "root": false,
      "layer": 1,
      "rssi": -65,
      "children": 0,
      "streaming": true,
      "parent": "A3:F2:01:02:03:04",
      "uptime": 382100,
      "stale": false
    }
  ]
}
```

**Serialization:** Use `snprintf` into a fixed 4 KB scratch buffer. No dynamic JSON library needed — the structure is flat and predictable. At 32 nodes, worst case is ~3 KB.

**Files to create:**
```
lib/control/include/control/portal_state.h       [NEW] — State cache API
lib/control/src/portal_state.c                    [NEW] — State aggregation + serialization
lib/control/src/portal_ws.c                       [NEW] — WebSocket endpoint + 1 Hz push task
```

**Files to modify:**
```
lib/network/include/network/mesh_net.h           [MODIFY] — Extend heartbeat struct
lib/network/src/mesh_net.c                       [MODIFY] — Populate new heartbeat fields, call portal_state_update
```

### 3.4 Verification

```bash
# With USB connected:
# Open browser console: new WebSocket("ws://10.48.0.1/ws")
# Verify: JSON snapshots arrive at ~1 Hz
# Verify: all mesh nodes appear in snapshot
# Verify: "self" field matches connected node's MAC
```

---

## Phase 4: Web UI — Astro Telemetry Dashboard

**Duration:** 3–4 days  
**Goal:** Interactive three-column dashboard with TE-style visual language, deterministic topology map, and animated data flow

### 4.1 Technology Choice

**Astro (static-site generation)** is the canonical frontend framework for maintainability, with a minimal client-side runtime.

**Rendering model:**
- Astro renders the dashboard shell and static layout at build time.
- A lightweight client module drives the real-time WebSocket telemetry updates.
- Canvas 2D remains the rendering surface for topology/data-flow animation.

**Why Astro + Canvas:**
- Maintains a predictable project structure as UI complexity grows
- Preserves low runtime overhead by shipping mostly static HTML/CSS
- Keeps animation performance by using Canvas for moving elements
- Supports clear separation of layout, visual theme tokens, and realtime rendering logic

**Build tooling:** Astro build pipeline outputs static assets, then assets are gzip-compressed into `data/` for SPIFFS serving.

### 4.2 UI Layout

Single-page, high-density telemetry dashboard with a global header, three-column body, and global footer.

```
┌────────────────────────────────────────────────────────────────────────────┐
│ [SOUNDMESH PORTAL]    ● Connected | Core 0 Load: 14% | Heap: 92 KB Free   │
├────────────────────────────────────────────────────────────────────────────┤
│ LEFT: AUDIO ANALYSIS │ CENTER: TOPOLOGY + FLOW │ RIGHT: NODE/STREAM STATE │
│ BPM + LOG FFT + Uptime│ deterministic mesh map   │ selected node telemetry  │
├────────────────────────────────────────────────────────────────────────────┤
│ Mesh Nodes: 12 | Net IF: usb_ncm (10.48.0.1) | State: Mesh OK             │
│ ● WS Active | 1 Hz Push | Build: TX v1.0.1                                 │
└────────────────────────────────────────────────────────────────────────────┘
```

### 4.2.1 Visual Design Tokens (Canonical)

- **Background:** matte charcoal `#121212`
- **RX/status accent:** system green `#76FF03`
- **Root TX accent:** TE blue `#2196F3`
- **Text and separators:** muted off-white
- **Typography:** compact monospaced font for labels and telemetry values
- **Framing:** thin single-pixel pane borders

### 4.2.2 Pane Responsibilities

- **Left pane (Audio Analysis):**
  - Large BPM readout (`126 BPM` style)
  - Log-frequency FFT (20 Hz → 20 kHz) with log-spaced grid lines
  - 24–32 constant-width bars in `#76FF03`
  - Uptime display (`1h 03m 41s` style)
- **Center pane (Topology + Data Flow):**
  - Deterministic layered network map
  - "You are here" marker for connected node
  - Animated flow pulses along parent→child edges
- **Right pane (Telemetry Detail):**
  - Selected node metadata (role, layer, RSSI, children, streaming, uptime, parent)
  - Stream and mesh health indicators mirroring footer state

### 4.3 Network Map — Deterministic Layout

**No force-directed simulation.** Use the `layer` field for deterministic ring-based layout:

- **Layer 0 (root):** Center of canvas
- **Layer 1:** Evenly spaced on first ring (radius R1)
- **Layer 2:** Evenly spaced on second ring (radius R2)
- Siblings ordered by MAC address (stable ordering)

**Node rendering:**
| Node State | Visual |
|------------|--------|
| TX (root) | Large blue circle, crown/star icon |
| TX (non-root) | Medium blue circle |
| RX (streaming) | Medium green circle, filled |
| RX (idle) | Medium green circle, outline only |
| COMBO | Medium purple circle |
| Stale node | Faded (50% opacity), dashed border |
| "You Are Here" | Pulsing ring animation + label |

**Edge rendering:**
- Solid lines from parent → child
- Line color: green (good RSSI > -60), yellow (-60 to -75), red (< -75)
- Line thickness proportional to RSSI quality

### 4.4 Data Flow Animation

When a node has `streaming: true`, animate dots/pulses along the edges from TX to RX nodes:

- Small circles travel along parent→child edges
- Direction: TX (source) → through tree → RX (leaves)
- Speed: constant, ~2 seconds per edge traversal
- Color: matches TX node color
- Pulse frequency: ~3 pulses per second when streaming

**Implementation:** `requestAnimationFrame()` loop at 30 fps, update particle positions along edge paths.

### 4.5 Node Detail Panel (Right Pane)

Tap/click a node to populate the right-side telemetry panel:

```
┌─────────────────────┐
│  RX Node             │
│  B1:C4:05:06:07:08   │
├─────────────────────┤
│  Layer:    1          │
│  RSSI:     -65 dBm   │
│  Children: 0          │
│  Streaming: Yes       │
│  Uptime:   1h 3m      │
│  Parent:   Root (TX)  │
└─────────────────────┘
```

### 4.6 Connection Status Indicators

Header and footer expose connection/runtime state:
- 🟢 **Connected** — receiving data
- 🟡 **Reconnecting** — WebSocket lost, auto-retry every 2 seconds
- 🔴 **Disconnected** — no connection

Auto-reconnect logic in JS with exponential backoff (2s → 4s → 8s → max 30s).

### 4.7 File Structure

```
portal/                                     [NEW] — Astro UI source
├── src/
│   ├── pages/
│   │   └── index.astro                      — Dashboard shell (header/body/footer)
│   ├── components/
│   │   ├── PortalHeader.astro
│   │   ├── AudioAnalysisPane.astro
│   │   ├── TopologyPane.astro
│   │   ├── TelemetryPane.astro
│   │   └── PortalFooter.astro
│   ├── styles/
│   │   └── portal.css                        — TE palette, mono type, pane framing
│   └── scripts/
│       └── portal-client.js                  — WS client + Canvas draw/update loop
├── public/                                   — Static assets (icons/fonts if needed)
├── astro.config.mjs
└── package.json
data/                                       [GENERATED] — SPIFFS image source from Astro build
├── index.html.gz
├── assets/*.js.gz
└── assets/*.css.gz
```

### 4.8 Verification

```bash
# With firmware flashed and USB connected:
# Open http://10.48.0.1/ in browser
# Verify: header shows [SOUNDMESH PORTAL], connection LED, Core 0 load, and heap
# Verify: three-column layout renders (Audio Analysis, Topology, Telemetry)
# Verify: left pane shows BPM, log-frequency FFT bars (20Hz-20kHz), and uptime
# Verify: Root TX uses TE blue (#2196F3), RX/status uses system green (#76FF03)
# Verify: pane borders are 1px and typography is monospaced/compact
# Verify: network map renders with all connected nodes
# Verify: "you are here" marker highlights the connected node
# Verify: animated dots flow along edges when audio is streaming
# Verify: tapping a node updates right telemetry pane details
# Verify: adding/removing a node updates map within 3 seconds
# Verify: UI works on mobile browser (responsive)
```

---

## Phase 5: Integration + Polish

**Duration:** 1–2 days  
**Goal:** End-to-end testing, firmware integration, documentation

### 5.1 Build System Integration

**SPIFFS partition:** Add to `partitions.csv`:
```
# Name,    Type, SubType,  Offset,   Size
spiffs,    data, spiffs,   ,         256K
```

Reduce app partition slightly to accommodate SPIFFS (from ~2MB to ~1.75MB).

**PlatformIO SPIFFS upload:**
```ini
# platformio.ini additions
board_build.filesystem = spiffs
board_build.spiffs_data_path = data
```

**Build workflow:**
```bash
# 1. Build web UI (from portal/ directory)
cd portal
pnpm install
pnpm run build               # Astro build -> dist/

# 2. Export + gzip dist assets into ../data/ for SPIFFS
pnpm run export:spiffs

# 3. Upload SPIFFS image
pio run -e tx -t uploadfs

# 4. Build and flash firmware
pio run -e tx -t upload
```

### 5.2 Conditional Compilation

All portal code guarded by build variant:
```c
#if defined(CONFIG_TX_BUILD) || defined(CONFIG_COMBO_BUILD)
    portal_init();   // Only root nodes get portal
#endif
```

RX builds compile without any portal code — zero RAM/flash overhead.

### 5.3 RAM Budget

| Component | Estimated RAM | Notes |
|-----------|--------------|-------|
| HTTP server task + internals | 10–15 KB | 6 KB stack + httpd state |
| WebSocket client buffers | 8–12 KB | TCP + WS frame buffers |
| Portal state cache | 2–3 KB | 32 × ~80 bytes |
| JSON scratch buffer | 4 KB | Fixed allocation, reused |
| DNS server | 1–2 KB | UDP socket + parse buffer |
| DHCP server | 2–3 KB | lwIP built-in |
| **Total** | **~30–40 KB** | |

**Safety:** Portal only starts if free heap > 64 KB after audio pipeline init.

### 5.4 Task Allocation

| Task | Core | Priority | Stack | Notes |
|------|------|----------|-------|-------|
| `portal_ws` | Core 0 | 3 | 4 KB | 1 Hz snapshot push |
| `httpd` | Core 0 | 5 | 6 KB | ESP-IDF managed |
| `dns_server` | Core 0 | 2 | 2 KB | UDP/53 responder |

All portal tasks on **Core 0** (alongside networking), keeping Core 1 clean for audio.

### 5.5 Verification — Full Integration

```bash
# Build all environments
pio run -e tx && pio run -e rx && pio run -e combo

# Flash TX with portal
cd portal && pnpm install && pnpm run build && pnpm run export:spiffs
pio run -e tx -t uploadfs
pio run -e tx -t upload

# Flash RX (no portal)
pio run -e rx -t upload

# Test scenario:
# 1. Power TX — becomes root, portal starts
# 2. Power RX — joins mesh
# 3. Plug USB into TX from computer
# 4. Navigate to http://10.48.0.1/
# 5. Verify: map shows TX (root, "you are here") + RX (child)
# 6. Verify: audio streaming shows animated data flow
# 7. Verify: unplug RX — node fades from map within 6 seconds
# 8. Verify: re-plug RX — node reappears on map
# 9. Check serial: no heap warnings, no crashes
```

---

## Implementation Order

| Step | Phase | Deliverable | Duration |
|------|-------|-------------|----------|
| 1 | Phase 1 | USB CDC-NCM + esp_netif + DHCP + DNS | 2–3 days |
| 2 | Phase 2 | HTTP server + SPIFFS serving + captive portal probes | 1–2 days |
| 3 | Phase 3 | Extend heartbeat + state cache + WebSocket push | 2–3 days |
| 4 | Phase 4 | Astro dashboard UI + Canvas topology/flow rendering | 3–4 days |
| 5 | Phase 5 | Integration, build system, testing | 1–2 days |

**Total estimated duration: 9–14 days**

---

## Files Summary

### New Files
```
lib/control/include/control/usb_portal.h         — Portal public API (init/start/stop)
lib/control/include/control/portal_state.h        — State cache API
lib/control/src/usb_portal_netif.c                — CDC-NCM + esp_netif + DHCP server
lib/control/src/usb_portal_dns.c                  — DNS catch-all (UDP/53)
lib/control/src/portal_http.c                     — HTTP server + static files + captive portal
lib/control/src/portal_state.c                    — State aggregation + JSON serialization
lib/control/src/portal_ws.c                       — WebSocket endpoint + 1 Hz push
portal/src/pages/index.astro                      — Dashboard page shell
portal/src/components/*.astro                     — Header/panes/footer components
portal/src/scripts/portal-client.js               — WS client + Canvas draw logic
portal/src/styles/portal.css                      — TE visual system tokens/styles
portal/astro.config.mjs                           — Astro build config
portal/package.json                               — Frontend scripts (build + SPIFFS export)
```

### Modified Files
```
lib/network/include/network/mesh_net.h            — Extend mesh_heartbeat_t (parent_mac, self_mac, stream_active)
lib/network/src/mesh_net.c                        — Populate new heartbeat fields + call portal_state_update
lib/config/include/config/build.h                 — Portal constants (stack sizes, task priorities, thresholds)
platformio.ini                                    — SPIFFS config for tx/combo envs
partitions.csv                                    — Add SPIFFS partition
sdkconfig.shared.defaults                         — HTTP server + WebSocket support
sdkconfig.tx.defaults                             — TinyUSB NCM mode
sdkconfig.combo.defaults                          — TinyUSB NCM mode
src/tx/main.c                                     — Call portal_init() after mesh ready
src/combo/main.c                                  — Call portal_init() after mesh ready
```

---

## Implementation Status

### ✅ Completed
- **Phase 3: Heartbeat extension** — `mesh_heartbeat_t` extended with `self_mac[6]`, `parent_mac[6]`, `stream_active`
- **Phase 3: Portal state cache** — `portal_state.c` aggregates heartbeats, serializes JSON snapshots, expires stale nodes
- **Phase 3: Heartbeat callback** — `network_register_heartbeat_callback()` feeds heartbeats to portal state on root
- **Phase 2: HTTP server** — `portal_http.c` with SPIFFS static file serving, gzip support, captive portal probe redirects, WebSocket endpoint with 1 Hz push task
- **Phase 2: DNS catch-all** — `usb_portal_dns.c` responds to all A queries with 10.48.0.1
- **Phase 4: Web UI prototype** — `portal/` directory includes Canvas 2D prototype with dark theme, demo mode, and animated data flow
- **Phase 5: Build system** — SPIFFS partition in `partitions.csv`, static asset gzip pipeline to `data/`, all three firmware variants build clean
- **Phase 5: Conditional compilation** — Portal only initialized on TX/COMBO builds

### ⚠️ Pending: USB CDC-NCM Networking
The bundled ESP-IDF 5.1.x (`framework-espidf@3.40406.240122`) does not include the `tinyusb_net` NCM driver or the `esp_tinyusb` managed component with network class support. The `tusb_ncm` example was added in ESP-IDF 5.2+.

**Options to complete USB networking:**
1. **Upgrade to ESP-IDF 5.2+** via `platform = espressif32@~6.9.0` (or later) — easiest path, `esp_tinyusb` v1.5+ has NCM support built in
2. **Add `espressif/esp_tinyusb` as managed component** — update `idf_component.yml` to `espressif/esp_tinyusb: "^1.5.0"`, requires compatible ESP-IDF version
3. **Implement NCM manually** using TinyUSB's low-level `net_device.h` API — the USB device class support exists in the bundled TinyUSB, but requires writing the esp_netif bridge manually (~200 lines)

The portal subsystem (`portal_init()`) currently registers the heartbeat callback and initializes state tracking, but skips USB network setup. All HTTP/DNS/WebSocket code is compiled and ready to activate.

### ⚠️ Pending: Astro Migration + TE Visual Alignment
- Migrate the current prototype frontend from flat static files to Astro project structure.
- Implement canonical TE-style visual language:
  - `#121212` background, `#76FF03` RX/status accents, `#2196F3` root TX accent
  - compact monospaced typography and 1px pane framing
  - three-column dashboard layout with explicit audio-analysis left pane
- Preserve current low-overhead runtime behavior (static shell + lightweight WS/Canvas client).

---

## Known Limitations (MVP)

1. **Captive portal auto-open is best-effort** — OS behavior varies. Manual `http://10.48.0.1/` always works.
2. **Single WebSocket client** — Only one browser tab actively connected. Second tab gets stale data or dropped.
3. **Root-only** — RX nodes don't serve the portal. Plug into TX/COMBO to see the network.
4. **No historical data** — Map shows live state only. No graphs/charts of past metrics.
5. **No mixer control** — Read-only view for MVP. Mixer UI is a follow-up feature.
6. **USB endpoint budget** — Portal networking may conflict with USB audio stubs. Portal takes priority in TX/COMBO builds.

## Future Enhancements (Post-MVP)

- **Mixer control page** — Faders for per-stream gain/pan/mute via WebSocket commands
- **Metrics graphs** — Chart.js for RSSI/latency/buffer history (ring buffer on root)
- **mDNS** — `soundmesh.local` hostname resolution
- **REST API** — `/api/status`, `/api/mesh/nodes`, `/api/mesh/topology` for curl/scripting
- **OTA firmware updates** — Upload `.bin` via portal
- **Multi-client** — Support 2–4 simultaneous WebSocket viewers
- **RX portal** — Lightweight local-only status page on RX nodes
- **ECM alternative** — Build flag to switch CDC-NCM ↔ CDC-ECM

---

## References

- [ESP-IDF TinyUSB Network Device](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
- [ESP-IDF HTTP Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html)
- [ESP-IDF WebSocket Support](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html#websocket-server)
- [ESP-IDF SPIFFS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/spiffs.html)
- [Canvas 2D API (MDN)](https://developer.mozilla.org/en-US/docs/Web/API/Canvas_API)
- [Astro Framework](https://astro.build/)
- [Captive Portal Detection (various OS)](https://en.wikipedia.org/wiki/Captive_portal#Detection)

---

*Plan created March 18, 2026. Ready for phased implementation.*
