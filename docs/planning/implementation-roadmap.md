# MeshNet Audio: Implementation Roadmap (v0.1 - v0.3)

**Date:** November 3, 2025  
**Status:** Master Implementation Plan  
**Goal:** Phased development from basic mesh to full multi-stream portal system

---

## Audio Specification (All Versions)

**Baseline Quality (v0.1+):**
- **Sample Rate:** 48 kHz (CD quality, music-focused)
- **Bit Depth:** 24-bit (professional audio standard)
- **Channels:** Mono (v0.1-v0.2), Stereo (v0.3+)
- **Frame Size:** 5ms = 240 samples = 720 bytes
- **Bitrate:** 1.152 Mbps (raw PCM)

**Network Scheme:**
- **IP Range:** 10.48.0.0/24 (48kHz reference, non-standard LAN)
- **Portal Gateway:** 10.48.0.1 (USB CDC-ECM access)
- **Nodes:** 10.48.0.2 - 10.48.0.254

**Hardware:**
- **Primary:** 3× XIAO ESP32-S3
- **Expansion:** 3× XIAO ESP32-C6
- **Total:** 6-node mesh capable

---

## Version Overview

| Version | Focus | Duration | Key Deliverables |
|---------|-------|----------|------------------|
| **v0.1** | Mesh Formation + Audio Streaming | 2-3 weeks | Self-healing mesh, 24/48 mono audio, local display |
| **v0.2** | Internal Observability + Compression | 1-2 weeks | Telemetry sync, Opus codec, time sync |
| **v0.3** | External Portal + Mixer | 2-3 weeks | USB portal, web UI, multi-stream mixing |

**Total Timeline:** 5-8 weeks to full system

---

## v0.1: Core Mesh + Audio Streaming

### Goal
Prove self-healing mesh topology works with high-quality audio streaming

### Success Criteria
- ✅ 2+ nodes form mesh automatically (first becomes root)
- ✅ Root migration works (kill root → new root elected, <500ms dropout)
- ✅ Audio flows single-hop (TX → RX, latency <30ms)
- ✅ Audio flows multi-hop (TX → Relay → RX, latency <50ms)
- ✅ Display shows mesh status (root, layer, children)
- ✅ 24-bit/48kHz mono audio quality confirmed (oscilloscope + listening test)

### Layer Implementation

#### Network Layer (see [mesh-network-architecture.md](mesh-network-architecture.md))
**Phase 1: Mesh Formation (Days 1-3)**
- [ ] Create `lib/network/src/network_mesh.c`
- [ ] ESP-WIFI-MESH initialization (channel 6, self-organized mode)
- [ ] Event handlers: parent connected, child connected, root election
- [ ] Topology queries: `network_is_root()`, `network_get_layer()`, `network_get_children_count()`
- [ ] **Test:** 2 nodes form mesh, first becomes root

**Phase 2: Single-Hop Audio (Days 4-6)**
- [ ] Mesh audio frame structure (magic, seq, timestamp, stream_id, TTL)
- [ ] `network_send_audio()` using `esp_mesh_send()` with `MESH_DATA_TODS`
- [ ] Mesh RX task with callback to audio layer
- [ ] **Test:** TX → RX direct, measure latency <30ms, verify audio quality

**Phase 3: Multi-Hop Relay (Days 7-10)**
- [ ] Duplicate suppression cache (32-entry circular buffer)
- [ ] Tree broadcast logic (forward to children except sender)
- [ ] TTL decrement to prevent infinite loops
- [ ] **Test:** 3-node chain (TX → Relay → RX), verify no duplicates, measure latency

**Phase 4: Root Migration (Days 11-12)**
- [ ] Handle `MESH_EVENT_ROOT_GOT_IP`, `MESH_EVENT_ROOT_LOST`
- [ ] Display updates for root status changes
- [ ] **Test:** Kill root mid-stream, verify <500ms dropout, new root elected

#### Audio Layer (see [audio-layer-architecture.md](audio-layer-architecture.md))
**ADC Input (Default) - Days 1-4**
- [ ] Configure ADC for 48 kHz sampling (native, no resampling)
- [ ] Implement 12-bit → 24-bit conversion (left-shift, zero-pad)
- [ ] DC blocking filter (1st order high-pass, ~20 Hz cutoff)
- [ ] **Test:** Oscilloscope verify 48 kHz rate, measure SNR

**Tone Generator (Testing) - Days 1-2**
- [ ] Update to 48 kHz, 24-bit generation
- [ ] Frequency range 200-2000 Hz (ADC knob control)
- [ ] **Test:** Spectrum analyzer verify purity

**I2S DAC Output - Days 3-5**
- [ ] Configure I2S for 24-bit mode (`I2S_DATA_BIT_WIDTH_24BIT`)
- [ ] Mono → stereo duplication (L = R = mono sample)
- [ ] **Test:** Oscilloscope verify I2S timing, listening test for quality

**Jitter Buffer - Days 6-8**
- [ ] Ring buffer: 10 frames capacity (50ms @ 5ms/frame)
- [ ] Prefill: 4 frames (20ms startup latency)
- [ ] Underrun handling: insert silence, log event
- [ ] **Test:** Continuous 10-minute playback, count underruns (<1/min target)

**Clock Drift Handling (Elastic Buffer) - Days 9-10**
- [ ] Measure buffer fill level every 5 seconds
- [ ] Detect drift: buffer fill >80% (TX faster) or <20% (RX faster)
- [ ] Insert sample (at zero-crossing) if buffer too empty
- [ ] Drop sample (at zero-crossing) if buffer too full
- [ ] Log drift corrections, track PPM error
- [ ] **Test:** 30-minute continuous playback, verify no drift-induced underruns

#### Control Layer (see [control-layer-architecture.md](control-layer-architecture.md))
**Display Updates - Days 11-13**
- [ ] Network view: mesh status (root indicator, layer, children count)
- [ ] Audio view: input mode (Tone/Aux), streaming status, level
- [ ] Metrics view: CPU %, heap free, buffer fill
- [ ] **Test:** Verify all views render correctly, cycle with button

**Settings Persistence - Days 14-15**
- [ ] NVS storage: mesh credentials, last input mode
- [ ] `settings_init()`, `settings_save()`, `settings_reset_to_defaults()`
- [ ] **Test:** Power cycle, verify settings restored

**State Machine - Days 15-16**
- [ ] States: BOOT → MESH_JOIN → READY → STREAMING → ERROR
- [ ] Auto-recovery: mesh disconnect → auto-rejoin
- [ ] **Test:** Disconnect parent, verify auto-recovery

### Configuration Files
- [ ] Update `sdkconfig.shared.defaults` with mesh settings
- [ ] Update `build.h` with 48kHz, 24-bit, 5ms frames
- [ ] Update `AGENTS.md` with build/test commands

### Exit Criteria
- 6-node mesh tested (3× S3, 3× C6)
- Audio quality validated (24/48 mono, THD <1%)
- Latency measured: 1-hop <30ms, 2-hop <50ms
- Root migration recovery <500ms
- Documentation complete

**Estimated Duration:** 2-3 weeks

---

## v0.2: Internal Observability + Compression

### Goal
Distributed mesh-wide telemetry and bandwidth optimization via Opus codec

### Success Criteria
- ✅ Every node caches full mesh state (all 6 nodes visible on any display)
- ✅ Telemetry stays in sync (<1 second staleness)
- ✅ Opus compression achieves 10× bandwidth reduction (1.15 Mbps → ~120 kbps)
- ✅ Time sync accurate to <50ms across mesh
- ✅ Control bandwidth overhead <3% of audio

### Layer Implementation

#### Network Layer
**Bandwidth Baseline - Days 1-2**
- [ ] Measure raw PCM bandwidth (1.152 Mbps confirmed)
- [ ] Measure scalability (how many streams per hop before saturation)
- [ ] **Test:** 3 simultaneous TX nodes, measure total airtime

#### Audio Layer
**ESP-ADF Resample Filter (Clock Drift Correction) - Days 3-6**
- [ ] Integrate ESP-ADF audio pipeline framework
- [ ] Add resample_filter element (complexity=1 for speed)
- [ ] Dynamic rate adjustment based on buffer fill metrics
- [ ] Replace v0.1 elastic buffer with proper SRC
- [ ] **Test:** Long-session drift <10ms over 1 hour

**Opus Codec Integration - Days 7-12**
- [ ] Integrate ESP-ADF Opus encoder (TX side)
- [ ] Integrate ESP-ADF Opus decoder (RX side)
- [ ] Bitrate testing: 64 kbps, 96 kbps, 128 kbps
- [ ] Codec negotiation in mesh frame header
- [ ] **Test:** Perceptual quality (MOS >4.0 target), measure latency overhead

**USB Audio Input (Future-Proofing) - Days 13-16**
- [ ] TinyUSB UAC1 device configuration
- [ ] Enforce 48 kHz (reject other sample rates)
- [ ] Stereo → mono downmix (average L+R)
- [ ] 16-bit → 24-bit zero-padding
- [ ] **Test:** Connect to Mac/PC, verify enumeration, stream audio

#### Control Layer
**Telemetry Schema - Days 1-3**
- [ ] Implement unified JSON schema (version 1.0.0)
- [ ] `telemetry_encode_json()`, `telemetry_decode_json()` using cJSON (ESP-IDF built-in)
- [ ] gzip compression using miniz (`telemetry_compress()`, `telemetry_decompress()`)
- [ ] **Test:** Encode/decode round-trip, measure compression ratio (target >60%)
- [ ] **Decision:** Keep JSON+gzip for readability (1.4% overhead acceptable)

**Mesh Control Messages - Days 4-7**
- [ ] Control frame header structure
- [ ] Message types: HEARTBEAT, TELEMETRY, TIME_SYNC
- [ ] Mesh RX task (separate from audio)
- [ ] **Test:** Broadcast telemetry, verify all nodes receive

**State Cache - Days 8-10**
- [ ] Distributed cache: `node_id → telemetry_snapshot_t`
- [ ] Age-out stale entries (120 second TTL)
- [ ] Lock-free reads, mutex for writes
- [ ] **Test:** 6-node mesh, verify each caches all 6 states

**Telemetry Publisher - Days 11-12**
- [ ] Periodic broadcast (1 Hz dynamic, 0.1 Hz static)
- [ ] Bandwidth measurement (target <20 kbps for 10 nodes)
- [ ] **Test:** Measure control overhead vs audio bandwidth

**Time Synchronization - Days 13-15**
- [ ] Root node as time authority
- [ ] Broadcast sync messages (10 second interval)
- [ ] Child offset adjustment
- [ ] **Test:** Measure drift (<50ms across 6 nodes)

### Configuration Files
- [ ] Add Opus bitrate settings to `build.h`
- [ ] sdkconfig: Enable ESP-ADF components

### Exit Criteria
- Opus codec working (perceptual quality validated)
- Telemetry synced across all nodes (<1 sec staleness)
- Display shows remote node metrics (not just own)
- Time sync accurate <50ms
- USB audio input functional (computer → TX → mesh)

**Estimated Duration:** 1-2 weeks

---

## v0.3: External Portal + Multi-Stream Mixer

### Goal
"Window into the network" - USB portal with web UI for mixer control

### Success Criteria
- ✅ USB plug-in → node appears as network interface (10.48.0.1)
- ✅ Web UI accessible at http://10.48.0.1/ (no external WiFi needed)
- ✅ Real-time telemetry visible (VU meters, topology graph)
- ✅ Multi-stream mixer functional (2+ TX nodes, adjustable gain/pan)
- ✅ Mixer commands propagate via mesh (global + per-RX control)
- ✅ Web UI works on Windows, Mac, Linux, Android (USB OTG)

### Layer Implementation

#### Network Layer
**Multi-Stream Support - Days 1-3**
- [ ] Multiple TX nodes publish simultaneously (unique stream IDs)
- [ ] RX receives multiple streams in parallel
- [ ] **Test:** 3 TX nodes streaming, 3 RX nodes receiving all streams

#### Audio Layer
**Stream Mixing - Days 4-8**
- [ ] Additive mixing (sum multiple streams)
- [ ] Per-stream gain control (0.0-2.0)
- [ ] Per-stream panning (-1.0 to +1.0, constant power law)
- [ ] Clipping prevention (normalize if sum exceeds headroom)
- [ ] **Test:** 2 TX → 1 RX, adjust gains, verify mix quality

**Stereo Output (Optional) - Days 9-11**
- [ ] Mono transmission → stereo output (panning creates L/R image)
- [ ] Update I2S to true stereo (not just duplicated mono)
- [ ] **Test:** Pan TX-A left, TX-B right, verify stereo separation

#### Control Layer
**USB Networking (CDC-ECM) - Days 1-4**
- [ ] TinyUSB CDC-ECM configuration (native Mac/Linux/Android support)
- [ ] IP assignment: node = 10.48.0.1, host = 10.48.0.2
- [ ] **Test:** Plug USB, verify OS detects network adapter (Mac/Linux/Android)
- [ ] **Note:** CDC-ECM chosen over RNDIS (deprecated on Win11, driver issues)

**HTTP REST API - Days 5-7**
- [ ] Endpoints: `/api/status`, `/api/mesh/nodes`, `/api/mesh/topology`, `/api/streams`
- [ ] POST `/api/mixer/command` (gain, pan, mute)
- [ ] **Test:** curl commands verify JSON responses

**WebSocket Server - Days 8-10**
- [ ] WebSocket endpoint: `ws://10.48.0.1/ws`
- [ ] Push telemetry updates (delta encoding for efficiency)
- [ ] Client subscription (all nodes or specific node IDs)
- [ ] **Test:** wscat connection, verify real-time updates

**Web UI (Astro - Minimal Scope) - Days 11-18**
- [ ] Dashboard page (status overview, active streams, mesh health)
- [ ] Mixer page (2-stream faders, basic VU meters, mute buttons)
- [ ] Settings page (node name, auth token, factory reset)
- [ ] Simple status indicators (no D3 topology graphs in v0.3)
- [ ] Responsive design (works on mobile browsers)
- [ ] **Test:** Full user flow on Mac/Linux/Android via USB
- [ ] **Deferred to v0.4:** Topology visualization, metrics graphs, advanced UI

**Mixer Control - Days 19-22**
- [ ] Simple 2-stream additive mixer (int32 accumulator)
- [ ] Per-stream gain (0.0-2.0), pan (-1.0 to +1.0, constant power)
- [ ] -6dB headroom (prevent clipping)
- [ ] Mixer command encoding (mesh control message type)
- [ ] Broadcast to all RX nodes (global config)
- [ ] **Test:** 2 TX nodes, adjust gains via web UI, verify mix quality
- [ ] **Limitation:** PCM-only mixing in v0.3 (Opus mixing in v0.4)

**Security - Days 23-24**
- [ ] Generate auth token on boot, persist to NVS
- [ ] Display token/QR code on OLED
- [ ] HTTP middleware: require token for POST/PUT endpoints
- [ ] Bind HTTP/WebSocket to USB interface only (not mesh)
- [ ] **Test:** Access without token (blocked), with token (allowed)

**LED Indicators (Optional/Deferred) - v0.4**
- Deferred to reduce v0.3 scope

### Web UI Development
**Tech Stack:**
- Astro (framework)
- Svelte (interactive components)
- Tailwind CSS (styling)
- D3.js (topology visualization)
- Chart.js (metrics graphs)

**Build Process:**
```bash
cd lib/control/web
npm install
npm run build  # → dist/
# Embed dist/ into firmware via SPIFFS
```

### Configuration Files
- [ ] sdkconfig: Enable TinyUSB CDC-ECM, HTTP server, WebSocket
- [ ] build.h: Add USB networking IP config

### Exit Criteria
- USB portal tested on Windows, Mac, Linux, Android
- Web UI fully functional (all pages interactive)
- Multi-stream mixer works (2+ TX, adjustable gains)
- Real-time telemetry visible (VU meters update <1 sec)
- Documentation: User guide for web UI operation

**Estimated Duration:** 2-3 weeks

---

## Testing Strategy (All Versions)

### Unit Tests (Per Component)
- Tone generator: frequency accuracy, spectral purity
- ADC input: sample rate verification, bit depth conversion
- I2S output: timing (oscilloscope), quality (THD analyzer)
- Jitter buffer: underrun recovery, prefill behavior
- Telemetry: JSON encode/decode, compression ratio
- State cache: staleness detection, lock-free reads

### Integration Tests
- Single-hop audio: TX → RX latency, quality
- Multi-hop audio: TX → Relay → RX latency, duplicate suppression
- Root migration: dropout duration, recovery time
- Mesh formation: 2, 4, 6 node topologies
- Telemetry sync: staleness across mesh (<1 sec)
- Mixer control: web UI → mesh → RX audio output

### Stress Tests
- Continuous playback: 1 hour, 10 hours (no crashes, underruns <1/min)
- Rapid topology changes: nodes joining/leaving every 10 seconds
- Maximum node count: How many nodes before saturation? (target: 10+)
- Worst-case latency: 3 hops, 3 TX streams, measure end-to-end

### Quality Validation
- **Audio Quality Metrics:**
  - THD+N: <1% (tone → I2S)
  - SNR: >90 dB (ADC input)
  - Frequency response: 20 Hz - 20 kHz (±1 dB)
  - Latency: <30ms (1-hop), <50ms (2-hop), <80ms (3-hop)

- **Network Metrics:**
  - Packet loss: <0.1%
  - Jitter: <10ms
  - Control overhead: <3% of audio bandwidth

- **Perceptual Quality (Opus v0.2):**
  - MOS (Mean Opinion Score): >4.0
  - ABX test: Opus vs raw PCM (indistinguishable at 128 kbps)

---

## Risk Mitigation

### Technical Risks

**Risk 1: Mesh Formation Instability**
- **Mitigation:** Extensive testing with 2, 4, 6 nodes in various topologies
- **Fallback:** Manual root selection if auto-election unreliable

**Risk 2: Audio Dropout During Root Migration**
- **Mitigation:** Implement buffering, measure dropout <500ms
- **Fallback:** Prefer RX-first root election (less disruption)

**Risk 3: USB CDC-ECM Driver Issues (Mobile)**
- **Mitigation:** Test on Android USB OTG early (v0.3 Day 1)
- **Fallback:** None needed (CDC-ECM native on Android)

**Risk 4: Opus Latency Too High**
- **Mitigation:** Measure codec latency, use 5ms frame size (lowest latency mode)
- **Fallback:** Keep raw PCM as option, make codec selectable

**Risk 5: Web UI Performance on ESP32**
- **Mitigation:** Static site generation (Astro), minimize JS bundle size
- **Fallback:** Simplify UI (text-based, no graphs/animations)

### Schedule Risks

**Risk 1: v0.1 Mesh Formation Takes Longer**
- **Mitigation:** Allocate 1 extra week for debugging
- **Impact:** Delays v0.2 start, but foundational

**Risk 2: ESP-ADF Opus Integration Complex**
- **Mitigation:** Start with ESP-ADF examples, adapt incrementally
- **Fallback:** Use standalone libopus (more work, but proven)

**Risk 3: Web UI Scope Creep**
- **Mitigation:** Strict feature freeze (mixer, topology, metrics only)
- **Fallback:** Text-based UI first, polish later

---

## Future Enhancements (v0.4+)

### Audio Quality
- **Stereo transmission:** 2-channel @ 48kHz (2.3 Mbps, still viable)
- **Hi-res audio:** 96 kHz, 24-bit (requires better ADC hardware)
- **Multi-band compression:** Dynamic range control
- **Spatial audio:** Positional metadata, HRTF rendering

### Mixer Features
- **Advanced routing:** Solo, mute, channel grouping
- **Effects:** Reverb, EQ, limiter (mesh-wide shared processing)
- **Presets:** Save/recall mixer scenes
- **MIDI control:** Physical faders/knobs via MIDI USB

### Network Features
- **Internet gateway:** One node bridges to home WiFi (remote access)
- **Cloud sync:** MQTT to AWS IoT, remote dashboard
- **OTA updates:** Firmware upgrades via web UI
- **Multi-mesh bridging:** Link multiple mesh networks

### Hardware
- **Dedicated relay nodes:** No audio I/O, pure forwarding
- **Subwoofer RX:** LPF-only output (crossover at 80 Hz)
- **Monitor RX:** Headphone amp, OLED spectrum analyzer
- **External codecs:** ES8388, PCM5102A for better ADC/DAC

---

## References

### ESP-IDF Documentation
- [ESP-WIFI-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/esp-wifi-mesh.html)
- [I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
- [TinyUSB CDC-ECM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)

### ESP-ADF Documentation
- [ESP-ADF Programming Guide](https://docs.espressif.com/projects/esp-adf/en/latest/)
- [Opus Codec Elements](https://docs.espressif.com/projects/esp-adf/en/latest/api-reference/codecs/opus_encoder.html)

### Audio Standards
- [24-bit Audio Specification](https://en.wikipedia.org/wiki/Audio_bit_depth)
- [Opus Codec](https://opus-codec.org/)
- [I2S Protocol](https://www.sparkfun.com/datasheets/BreakoutBoards/I2S_Bus_Specification.pdf)

### Web Technologies
- [Astro Framework](https://astro.build/)
- [Svelte](https://svelte.dev/)
- [WebSocket API](https://developer.mozilla.org/en-US/docs/Web/API/WebSocket)

---

**Document Structure:**
- **This file:** High-level roadmap, version goals, timelines
- **[mesh-network-architecture.md](mesh-network-architecture.md):** Network layer details
- **[audio-layer-architecture.md](audio-layer-architecture.md):** Audio processing pipeline
- **[control-layer-architecture.md](control-layer-architecture.md):** Observability & portal

---

*Master implementation plan created November 3, 2025. Ready for phased execution.*
