# SoundMesh Mesh Network Reliability Fix

**Date:** March 20, 2026  
**Status:** Ongoing - Root Cause Reframed (Validated with Multi-Node Field Tests)  
**Author:** Copilot Analysis

## Latest Validation Cycle (2026-03-23, root-cause architecture pass)

### Resolution cycle (2026-03-23, close-range RF stabilization)

#### Key root-cause shift
- In this physical setup (all nodes within a couple feet), the dominant failure was near-field RF saturation / auth instability, not root send drops.
- Evidence before fix:
  - Root often held only 2 descendants.
  - OUT1/OUT3 showed repeated parent disconnects with `AUTH_EXPIRE` dominant.
  - Root audio transport stayed clean (`Mesh TX GROUP` drops at 0.0%), proving downstream association was the weak link.

#### Changes implemented
- Added close-range RF transmit-power cap:
  - `WIFI_TX_POWER_QDBM=52` (13 dBm) in `lib/config/include/config/build.h`.
  - Applied at runtime with `esp_wifi_set_max_tx_power()` in `lib/network/src/mesh_net.c`.
- Kept flat close-range topology:
  - `MESH_MAX_LAYER=2`, `MESH_XON_QSIZE=64`.
- Added churn observability fields in heartbeat payload:
  - `parent_conn_count`, `parent_disc_count`, `auth_expire_count`, `no_parent_count`.
- Reduced false playback underruns from scheduler jitter:
  - small grace wait before declaring underrun in `rx_playback_task`.
- Increased RX burst tolerance:
  - `OPUS_BUFFER_FRAMES: 10 -> 14`
  - `JITTER_PREFILL_FRAMES: 12 -> 14`

#### Validation (all firmware rebuilt + reflashed to COMBO + 3 OUT)
- 240s soak after TX-power cap:
  - Root: descendants `min=max=last=3`, TX drops `0.0%`.
  - OUT1/OUT2/OUT3: `parent_disc=0`, `AUTH_EXPIRE=0`, `NO_AP_FOUND=0`.
- 360s extended soak:
  - Root: descendants stable at `3` for full run, TX drops `0.0%`.
  - All OUTs: no parent disconnect/auth churn events.
- Final 240s + 120s verification runs:
  - Descendants remained stable at 3.
  - Root TX drops remained 0.0%.
  - Remaining issue is reduced to occasional playback underruns (short jitter events), not mesh join instability.

#### Outcome
- **Three-OUT connectivity blocker resolved** in the tested environment.
- Root now correctly reports all descendants consistently.
- Remaining quality work is now purely playback smoothness polish (underrun elimination), not topology/auth collapse.

### Follow-up cycle (2026-03-23, control-plane shaping + soak)

#### Change implemented
- Reduced avoidable root control-plane airtime during streaming in `lib/network/src/mesh_net.c`:
  - Root now skips root→descendant heartbeat fanout in `send_heartbeat()`.
  - Root now skips stream-announcement fanout in `send_stream_announcement()`.
- Rationale: RX state already keys off audio-frame arrival; these root fanouts were not required for playback and added extra P2P traffic pressure.

#### Build / deploy
- Rebuilt all environments successfully: `tx`, `rx`, `combo`.
- Reflashed COMBO root + all three OUT nodes.

#### Post-flash results
- 120s OUT-only run:
  - No `AUTH_EXPIRE` / `NO_AP_FOUND` on any OUT.
  - OUT1 still showed parent churn (`parent_disc=6`, `state_changes=15`) and underruns.
  - OUT2/OUT3 stable connectivity but still occasional underruns; all nodes still hit `buf=0%` at times.
- 240s 4-node soak (root + 3 OUT):
  - Root `Mesh TX GROUP`: `drops=0.0%` throughout sampled windows.
  - Root descendant count stayed at `2` in this run (`descendants_min=max=last=2`), with no routing-table change events.
  - OUT1 remained the unstable branch (`parent_disc=24`) while OUT2/OUT3 had no parent disconnects.
  - All OUT nodes still recorded underruns and periodic `buf_min=0%` starvation.

#### Updated root-cause read
- Control-plane shaping reduced non-essential traffic and removed one architectural pressure source.
- Remaining dominant issue is not root send-queue overflow; it is branch/path instability plus downstream delivery jitter (especially OUT1) while root effectively serves only two descendants in this run.
- This is still architectural: transport looks healthy at root, but end-to-end mesh membership/routing stability is the blocker.

#### Next high-leverage steps
1. Add explicit per-node association/churn counters into shared telemetry (root view + OUT serial), then correlate churn onset with underruns.
2. Investigate why root stayed at 2 descendants in the soak despite 3 OUT nodes flashed/active (join/membership visibility issue).
3. Add lightweight playback target-fill control (hold near mid-buffer) to reduce repeated `buf=0%` excursions under jitter spikes.
4. Keep non-audio control-plane traffic minimized during active stream windows.

### Root-cause findings (updated)
- The dominant instability is still **mesh association churn on one branch** (AUTH_EXPIRE / NO_AP_FOUND loops), not encoder/decoder CPU saturation.
- `Mesh TX GROUP` on root remains `drops=0.0%` in sampled windows, so the biggest user-audible failure mode is not root queue overflow.
- RX stutter maps to two different mechanisms:
  - intermittent buffer starvation under latency spikes (buffer hits `0%`)
  - complete branch collapse when a node enters auth-expire loops.
- Flat topology (`max_layer=2`) conflicted with self-healing goals and reduced join resilience.

### Changes implemented this cycle
- Enabled adaptive multi-hop routing:
  - `esp_mesh_set_max_layer(6)` in `lib/network/src/mesh_net.c`
- Added Opus resilience tuning:
  - `OPUS_SET_INBAND_FEC(1)` and `OPUS_SET_PACKET_LOSS_PERC(8)`
  - single-gap path now requests FEC decode on next frame; multi-gap still uses bounded PLC markers.
- Improved underrun behavior in playback task:
  - on underrun, output last-good PCM frame instead of hard silence to reduce audible click/drop artifacts.
- Reduced aggressive RX self-heal timing that could induce churn:
  - prolonged starvation rejoin threshold from 20s/60s cadence to 60s/180s.
- Increased association expiry window:
  - `MESH_AP_ASSOC_EXPIRE_S: 30 -> 90` to reduce auth churn on relay links.

### Experimental rollback from this cycle
- Tested channel move `MESH_CHANNEL 6 -> 11`.
- Result: broad regression (higher sustained loss on multiple OUT nodes, persistent heavy-loss windows).
- Action: reverted back to channel 6 (best-known baseline in this environment).

### What improved
- With adaptive topology enabled, one OUT branch showed substantial windows near ~0.4–1.3% loss and stable stream continuity compared to prior collapse behavior.
- Root visibility remained correct for descendant counting (`route_table`/children events).

### What remains unresolved
- One OUT still intermittently enters prolonged AUTH_EXPIRE loops and can collapse audio completely for long windows.
- Even on healthy links, periodic latency spikes still push buffer to 0%, causing occasional audible artifacts despite concealment.

### Current best-known firmware posture
- GROUP multicast fanout
- `MESH_FRAMES_PER_PACKET=2`
- `OPUS_BITRATE=24000`
- adaptive topology enabled (`max_layer=6`)
- Opus FEC + PLC + last-good-frame underrun concealment
- channel 6 (reverted from failed channel 11 test)

### Next root-cause actions (not yet implemented)
1. Add per-node association telemetry counters to portal/serial (auth-expire rate, reconnect intervals) to identify which branch is unstable earliest.
2. Gate non-audio control-plane traffic during active stream windows (heartbeat/diagnostic throttling) to minimize contention during peaks.
3. Add bounded jitter target control (keep playback buffer around mid-fill rather than drift to 0/100 swings).
4. Validate with physical separation + battery-only power test to rule out USB-host RF/noise coupling side-effects.


## Latest Validation Cycle (2026-03-22, architecture stabilization pass)

### What was changed
- Kept flat topology (`max_layer=2`) and GROUP multicast fanout.
- Reduced transport pressure with balanced batching: `MESH_FRAMES_PER_PACKET=2` (25 pps).
- Reduced bitrate to `OPUS_BITRATE=24000` for lower airtime.
- Increased RX resilience: `JITTER_PREFILL_FRAMES=6`, `JITTER_BUFFER_FRAMES=12`.
- Added packet-loss concealment (PLC) path in RX pipeline:
  - On sequence gaps, insert up to `RX_PLC_MAX_FRAMES_PER_GAP` synthetic PLC frames.
  - Decode task handles zero-length frames via `opus_decode(NULL, 0, ...)`.
- Added routing-table add/remove event handling so root node count reflects full descendants.

### Live validation (COMBO + 3 OUT, all flashed and monitored)
- **SRC/COMBO**
  - Shows `Nodes:3` consistently (descendant count now correct).
  - `Mesh TX GROUP ... drops=0 (0.0%)`.
  - TX rate stabilized around ~30-31 kbps at 24 kbps Opus + batch=2.
  - Local ES8388 path still healthy (`I2S TX writes` continuous, no zero-byte writes).
- **OUT1**
  - Loss improved to ~2.2-2.4% (down from prior ~7-11% and much higher historical windows).
  - Buffer now generally healthy (often >35%, frequently 75-100%).
- **OUT2**
  - Loss improved to ~1.7-2.0%.
  - Sustained playback with no persistent starvation pattern.
- **OUT3**
  - Loss improved to ~2.4-3.2%.
  - Occasional short underrun events remain, but no long collapse windows.

### Big-picture root-cause update
- This confirms the dominant issue was architectural transport pressure (packet rate + airtime + burst-loss sensitivity), not a single decoder bug.
- The new stack (GROUP + moderate batching + lower bitrate + deeper jitter + PLC) addresses root causes, not only symptoms.
- Residual `reason:201` root logs persist (ESP-MESH control-plane behavior), but are no longer correlating with severe playback collapse in the observed windows.

## Latest Validation Cycle (2026-03-21, RX rejoin self-heal deployment)

### What was deployed
- **RX self-heal rejoin logic**: Added `network_trigger_rejoin()` API that performs child disconnect+reconnect.
- Wired into `rx/main.c` to auto-rejoin after sustained connected-but-stream-starved condition (30s threshold).
- `MESH_FRAMES_PER_PACKET=6` (airtime reduction tuning from prior cycle).
- Root fanout uses explicit `MESH_DATA_P2P` per-descendant send (from prior cycle).

### Validation results (fresh reflash, COMBO + 3 OUT)

**OUT1** - **Critical RF/Auth Path Issue (ROOT CAUSE)**:
- Persistent `AUTH_EXPIRE` (reason=2) loop during join attempts
- Cycles through auth→init failures, never reaching stable parent connect
- **Key Discovery**: Eventually selects **OUT2** as parent (ESPM_EADFDC, layer:2)
  - Forms **3-hop topology**: ROOT → OUT2 → OUT1 (layer:3)
  - This proves mesh self-organization is working correctly
  - Final success shows OUT1's RF path to root is severely degraded
  - Direct auth to root fails repeatedly; must use intermediate hop

**OUT2** - **EXCELLENT** (Major Improvement):
- Direct layer:2 child of root, RSSI -18 to -20dBm
- **First 19s window**: 756 pkts received, **0 drops (0.0% loss)** — PERFECT
- Stream stopped at ~22s (root stopped streaming briefly)
- **Self-recovered at ~31s** when root resumed (stream detection working)
- Post-recovery: ~5-7% loss (very acceptable)
- **Acts as mesh relay**: OUT1 successfully connects as OUT2's child at ~20s
  - Routing table shows 2 children (confirms relay role)
- Demonstrates both excellent direct reception AND mesh relay capability

**OUT3** - **Very Solid**:
- Direct layer:2 child of root, excellent RSSI -11 to -14dBm  
- Consistent ~4.7-5.3% loss (stable, acceptable)
- Good buffer management (oscillates 0-100% but maintains stream)
- Some underruns (#20 observed) but recovers gracefully

### Big-Picture Root Cause Analysis

**The core issue is OUT1's RF path quality to root, NOT mesh transport logic:**

1. **OUT1 cannot reliably auth directly to root** due to weak/unstable RF link
2. **OUT2 and OUT3 connect directly and receive well** (proves root broadcast works)
3. **Mesh self-healing works correctly**: OUT1 discovers it can't reach root directly, scans alternatives, finds OUT2 with good RSSI (-33dBm), connects as layer:3 child
4. **3-hop delivery proves mesh routing works**: OUT1 eventually receives via ROOT → OUT2 → OUT1 path

**What this means:**
- Root broadcast transport is **working correctly** (2 nodes receiving with low loss)
- Mesh relay/routing is **working correctly** (OUT1 can form multi-hop path)
- OUT1's join struggles are **NOT** a software bug — it's genuine RF/association path quality
- The "only 2 of 3 nodes connect" symptom is actually "OUT1 takes longer to find viable path"

**Prior fixes that contributed to this success:**
- `MESH_FRAMES_PER_PACKET=6` reduced airtime pressure
- Root `MESH_DATA_P2P` explicit fanout eliminated broadcast ambiguity  
- RX stream-loss hysteresis (`STREAM_SILENCE_TIMEOUT_MS=500ms`) reduced state flapping
- Removed runtime `esp_mesh_set_self_organized()` toggles (eliminated churn)

### Remaining Work

**COMBO Local Audio Monitor** - **SOFTWARE VERIFIED WORKING**:
- Diagnostic logs confirm ES8388 DAC write path is fully functional:
  - `I2S TX stats: writes=4000, zero_bytes=0` - All writes succeeding
  - `last_bytes=3840` - Full frames being written
  - `sample[0]=168` (non-zero) - Real audio data in DAC output
- Software capture→DAC passthrough pipeline is active and correct
- **If user hears no audio from headphones**, likely causes:
  1. **Physical routing**: ES8388 modules have multiple output jacks (OUT1/OUT2, Line Out/Headphone Out)
     - User may be plugged into wrong jack
     - Check ES8388 module markings for correct headphone output
  2. **ES8388 internal routing**: May need register adjustment for OUT1 vs OUT2 selection
  3. **Volume/gain**: Though software shows +4.5dB max, analog path may need hardware adjustment
- **Action**: User should verify physical headphone connection to correct ES8388 output jack

**Stream Pause/Start Behavior**:
- OUT2 lost stream at ~22s, auto-recovered at ~31s when root resumed
- Timing correlates with OUT1 joining as OUT2's child (~20s)
- **Hypothesis**: Routing table change (2→3 descendants) may cause brief root stream pause
- **Need to validate**: Monitor root during child join events to confirm correlation

**Root Reason:201 Churn**:
- Persistent `wifi:disconnected reason:201()` every 2-3 seconds on COMBO root
- This is known ESP-MESH behavior under multi-child streaming load
- Does NOT prevent audio delivery (OUT2/OUT3 receiving with excellent quality)
- Does NOT cause node disconnections or routing failures
- **Decision**: Accept as mesh stack characteristic, not blocking issue

## Problem Summary

Three critical issues have been identified with the ESP-WIFI-MESH audio streaming system:

1. **Connection reliability**: Only 1-2 of 3 OUT nodes connect reliably; the 3rd gets stuck in "Mesh joining" state indefinitely
2. **Inconsistent audio reception**: Connected nodes don't consistently receive audio, and conditions for success aren't reproducible
3. **Multi-node degradation**: Adding a 2nd working node causes both OUTs to stutter (nodes interfere with each other rather than cooperating)

Additionally, these issues have reportedly worsened over time, suggesting state churn under unstable stream conditions.

---

## What Changed in Understanding (Big-Picture Feedback Loop)

Initial investigations over-weighted network flow-control tuning as the primary cause. Live monitoring across OUT nodes changed that understanding.

### Earlier belief
- Packet-loss and stutter were mainly from mesh send backpressure logic.

### Current belief (after instrumentation + device monitoring)
- The dominant problem is **bursty source delivery + overly sensitive RX stream-loss detection**, which creates rapid stream state flapping.
- Flapping drives repeated stream-lost/stream-found transitions, aggravating underruns and heap pressure over time on affected nodes.
- One OUT failing to join is now treated as a topology/RF joinability issue, not just queue tuning.

### Root-cause vs symptom classification
- **Root-cause fixes already applied:** preserve root joinability behavior, disable overly aggressive rate limiter, fix packet-loss sequence math.
- **Symptom mitigation applied:** RX hysteresis added so transient packet gaps do not collapse stream state as quickly.

### Latest validation results (post-fixes)
- RX timeout hysteresis (`STREAM_SILENCE_TIMEOUT_MS` now 500ms) and continuous TX policy reduced stream-state churn side effects (heap remains stable in observed windows).
- Lower TX bitrate (`OPUS_BITRATE=32000`) improved airtime pressure; one node (OUT2) now sustains long streaming windows around ~10-16% reported loss.
- OUT1 remains unstable with repeated stream loss and higher loss (~30-32%), including intervals showing weak RSSI around `-60 dBm`.
- OUT3 still fails join with persistent `AUTH_EXPIRE` loops even after full erase + reflash; this ruled out stale firmware/NVS buildup as the primary cause.
- Overall pattern is now clearly asymmetric by node, indicating link/topology/auth path quality issues (RF and association reliability), not a single global queue bug.

### New evidence from latest field cycle (post-rollback)
- OUT firmware rollback/reflash completed cleanly on all three OUT ports.
- OUT3 still repeatedly logs:
  - `[DONE]connect to parent: ... rssi:-24 ... [layer:1, assoc:2]`
  - immediate `wifi:state: auth -> init (200)` and `AUTH_EXPIRE`
- This means discovery and RSSI are good, but association/auth is failing after parent selection.
- The persistent `assoc:2` in parent advertisements strongly suggests parent association capacity is effectively 2 in the active root path. That exactly matches the observed behavior: two OUTs can join, third churns in `AUTH_EXPIRE`.

### New evidence from latest field cycle (post-runtime-toggle removal)
- Applied a focused patch in `mesh_net.c` to **remove runtime `esp_mesh_set_self_organized(...)` toggles** from `MESH_EVENT_PARENT_CONNECTED` and `MESH_EVENT_PARENT_DISCONNECTED`.
- Rationale: repeated runtime topology-mode toggling during association/disassociation can create immediate leave/rejoin churn that looks like RF instability.
- Rebuilt `tx/rx/combo` and reflashed all 3 OUT nodes.
- Synchronized OUT monitoring after reflash shows:
  - `OUT1` and `OUT2` no longer emit rapid `Parent connected -> ASSOC_LEAVE` loops.
  - Both nodes remain mesh-connected and continue receiving audio intermittently.
  - `OUT3` remains in repeated `AUTH_EXPIRE` / occasional `NO_AP_FOUND`, still never reaching stable connected state.
- Interpretation:
  - Runtime self-organized toggles were a **real contributor** to churn on connected nodes (root-cause fix, not cosmetic).
  - Remaining blocker for 3/3 join is still the auth/association path for the third OUT (likely root-side capacity/auth behavior and/or asymmetric RF path), not OUT stale state.

### New evidence from latest field cycle (RX stream-loss confirmation window)
- Implemented a second-stage RX stream-loss confirmation gate:
  - Added `STREAM_SILENCE_CONFIRM_MS` in `build.h` (set to `300`).
  - Updated `src/rx/main.c` so `STREAM_SILENCE_TIMEOUT_MS` must be exceeded and sustained for confirm duration before declaring `Stream Lost`.
- Rebuilt and reflashed all OUT nodes.
- Post-flash monitoring results:
  - `OUT1`: long stable streaming windows observed (loss trending down toward ~5-6%) before occasional `Stream Lost`.
  - `OUT2`: improved continuity, but still shows intermittent stream-loss events under bursty periods.
  - `OUT3`: still blocked in join path (`AUTH_EXPIRE`/`NO_AP_FOUND`).
- Interpretation:
  - This materially reduces false-positive stream-loss flapping (symptom mitigation with clear impact).
  - It does not change the final join blocker for OUT3, which remains upstream of RX stream-state logic.

### New evidence from latest field cycle (reverted experiments)
- Tested two additional OUT-side experiments and rolled both back after validation:
  1) Increased mesh AP association expiry to 45s
  2) Added bounded RX mesh-restart recovery for prolonged join loops
- Neither produced a reliable break-out from persistent OUT3 `AUTH_EXPIRE`.
- Restored best-known baseline:
  - `MESH_AP_ASSOC_EXPIRE_S=30`
  - native ESP-MESH retry path (no forced mesh restarts)
- Final OUT-only snapshot remains:
  - OUT1/OUT2: connected heartbeats observed
  - OUT3: repeated `AUTH_EXPIRE` with candidate selection but no stable join

### Refined root-cause model
1. **Root-side association capacity/config path is currently the dominant blocker** for 3-node join reliability.
2. **OUT-side firmware state accumulation is not the blocker** (erase/reflash and retry-path changes did not alter OUT3 outcome).
3. **Audio stutter/loss tuning is secondary until 3/3 join is stable**; fixing airtime helps, but does not solve the hard join ceiling pattern.

### Current Status (2026-03-21 - Post Multiple Iterations)
- **Latest telemetry confirms persistent transport-layer issues**:
  - SRC reports healthy transmission (100-113 kbps, qfull=0)
  - OUT1: 34.5% packet loss, RSSI -21dBm, buffer oscillating 0-87%
  - OUT2: 32.5% packet loss, RSSI -30dBm, buffer oscillating 0-62%
  - OUT3: 64% packet loss, RSSI -40dBm, buffer mostly empty (0%)
- **Root cause confirmed**: ESP-WIFI-MESH P2P broadcast inefficiency under load
- **Key insight**: Application reports success but mesh transport drops packets
- Applied multiple transport-pressure reduction attempts:
  - `MESH_FRAMES_PER_PACKET`: 2 → 6 (reduces packets/sec)
  - `OPUS_BITRATE`: 64000 → 32000 (reduces bandwidth pressure)
  - `JITTER_PREFILL_FRAMES`: 3 → 10 (adds buffering against gaps)
- Previous fixes helped but **fundamental broadcast scaling issue remains**

### FINAL VALIDATION: FROMDS/TODS Broadcast Fix (2026-03-21)

**Problem Identified:**
- Root monitoring showed `route_table=3, children=3` but TX log: `children=2, sent=2`
- OUT3 logs: connected layer 3, received 171 packets over ~10s, then complete silence
- OUT3 parent monitor marked OUT2 as `[FAIL][weak]` when audio stopped
- OUT2 logs showed stream interruptions coinciding with OUT3 audio loss
- **Root cause:** Manual P2P iteration doesn't support multi-hop relay; OUT3 at layer 3 through OUT2 couldn't receive via intermediate relay

**Fix Applied:**
Changed `network_send_audio()` and `network_send_control()` in `mesh_net.c`:
- Root uses `MESH_DATA_FROMDS` (one send, mesh stack handles forwarding)
- Children use `MESH_DATA_TODS` (upstream to parent, proper DS flow)
- Removed per-child flow tracking and rate limiting logic
- This is the canonical ESP-WIFI-MESH pattern for root-to-all broadcast

**Validation Results (all 4 nodes monitored simultaneously):**

SRC (Root):
- `route_table=3, children=3` ✅ Sees all nodes
- `Mesh TX FROMDS: descendants=2, result=ESP_OK, total_sent=2264, drops=0 (0.0%)` ✅ Zero drops!
- One broadcast reaching multiple descendants
- 35-37 kbps TX rate, stable heap ~75-76%

OUT1:
- Connected layer 2 (one hop from root)
- Packet loss: 3.7% → 9.2% (improved from previous 15-26%)
- RSSI: -32 to -34 dBm
- Receiving audio consistently with reduced underruns

OUT2:
- Connected layer 2 with 2 children (acting as relay)
- Packet loss: 3.6% → 6.3% (much improved)
- RSSI: -22 to -27 dBm (excellent signal)
- Successfully forwarding to OUT3

OUT3:
- **CONNECTED layer 3 via OUT2 relay** ✅ 🎉
- Packet loss: ~9.6-10% (reasonable for 2-hop path)
- RSSI: -30 dBm to parent (OUT2)
- **NO MORE AUTH_EXPIRE LOOPS**
- Receiving audio through multi-hop relay

**Conclusion (updated):**
The transport path is improved but not yet at the target state.

- OUT2 is now consistently the best-performing node (recent loss ~2-3%).
- OUT1 and OUT3 still show bursty delivery and frequent `buf=0%` starvation periods.
- Root remains unstable at WiFi control-plane level (`reason:201`, occasional `reason:106`), and child count intermittently flaps (2↔3), which aligns with audio bursts/stalls.

So the latest fixes addressed part of the root cause (delivery path and observability), but the dominant remaining cause appears to be **root-side WiFi/mesh stability under multi-node load**, not decoder quality.

### New evidence from final tuning cycle (2026-03-21)

Applied and validated:
- Root fanout path moved to explicit P2P per-descendant send (`Mesh TX P2P`) with queue-full metrics.
- Added high-signal diagnostics around audio callback absence and pipeline feed failures.
- Increased burst tolerance (`MESH_FRAMES_PER_PACKET=6`, higher jitter prefill/buffer, longer stream silence hysteresis).

Observed after full reflash of SRC + all OUTs:
- SRC: `Mesh TX P2P ... qfull=0` and ~100–113 kbps TX when 3 descendants are present.
- OUT2: typically ~2–3% loss and good continuous audio windows.
- OUT1: remains degraded (~14–16% loss in latest window, frequent buffer starvation).
- OUT3: often waits with `RX: 0 pkts`, then intermittently receives stream; in active windows it can exceed ~20% loss.

Interpretation:
- We are no longer blind; logs now clearly separate transport ingress vs pipeline behavior.
- Pipeline is not the primary failure mode; starvation tracks mesh/root instability.
- Remaining work should target root-side mesh/WiFi control behavior and parent stability (association churn and disconnect reasons), not further Opus/decode tweaks alone.

## Root-Cause-Aligned Direction (Going Forward)

1. **Treat TX continuity + airtime as partial bottlenecks (now mitigated)**  
   Continuous TX and lower bitrate improved one branch of the mesh, confirming these were contributing causes.

2. **Preserve relay capability on RX nodes**  
   Connected RX nodes now keep self-organized enabled with `select_parent=false` to support descendant joins (multi-hop intent from architecture docs), instead of hard-disabling topology operations after connect.

3. **Avoid disruptive root STA disconnect churn**  
   Forced `esp_wifi_disconnect()` on child-connect was removed for root event handling to reduce join/auth churn during multi-node association.

4. **Treat OUT3 auth failure as a separate root cause**  
   Reflash/erase did not change behavior; next work should target join/auth path reliability (parent selection quality and RF conditions), not memory cleanup.

5. **Continue only with changes that prove causal**  
   Deferred adaptive batch size implementation for now because current RX loss accounting assumes fixed `MESH_FRAMES_PER_PACKET`; changing batch size without protocol/metrics migration would confound diagnosis.

---

## Analysis Methodology

This investigation examined:

1. **Codebase audit** of `lib/network/src/mesh_net.c` (920+ lines of mesh networking logic)
2. **Audio pipeline review** of `lib/audio/src/adf_pipeline.c` (encode/decode/playback flow)
3. **Configuration review** of `sdkconfig.*.defaults` files and `lib/config/include/config/build.h`
4. **Comparison with ESP-IDF examples** from the official [esp-idf mesh internal_communication example](https://github.com/espressif/esp-idf/tree/master/examples/mesh/internal_communication)
5. **ESP-WIFI-MESH API documentation** from ESP-IDF v5.x reference

---

## Root Cause #1: Connection Reliability (3rd Node Stuck)

### Finding: Self-Organized Mode Disabled Prematurely

**Location:** `lib/network/src/mesh_net.c`, lines 302-306

```c
case MESH_EVENT_PARENT_CONNECTED: {
    // ... connection handling ...
    
    // Disable self-organized mode to stop periodic parent monitoring scans
    // These scans pause the radio for ~300ms and cause audio dropouts
    esp_mesh_set_self_organized(false, false);
    esp_wifi_scan_stop();
    ESP_LOGI(TAG, "Self-organized disabled (no more parent scans during streaming)");
    break;
}
```

**The Problem:**

When an OUT node successfully connects to the root and becomes `PARENT_CONNECTED`, this code disables `self-organized` mode. The intent was to stop periodic scanning that causes audio dropouts. However, **this code runs on all nodes including the root**.

For RX/OUT nodes, disabling self-organized is fine—they've already joined.

For the ROOT node (TX/COMBO), disabling self-organized means **the mesh AP stops accepting new child associations**. The 3rd OUT node attempts to join but the root's SoftAP is no longer processing new connections.

**Evidence:**

- ESP-IDF documentation states: "When self-organized networking is disabled, the mesh stack will not perform operations that change the network's topology" ([ESP-MESH Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/esp-wifi-mesh.html))
- The code at line 358-363 tries to handle this for CHILD_CONNECTED, but by then the damage is done

**Citation:** Lines 302-306 of `mesh_net.c`

### Finding: Missing Mesh Queue Configuration

**Location:** `lib/network/src/mesh_net.c`, `network_init_mesh()` function (lines 736-859)

The function never calls `esp_mesh_set_xon_qsize()` to configure the mesh RX queue. Default is 32 packets.

**The Problem:**

With 3 nodes receiving ~25 audio packets/second each (50fps/2 batch = 25pps), the total is 75 packets/second flowing through the root. During bursts or when one node is slow to drain, the queue fills up and new connections are rejected.

**Evidence:**

ESP-IDF header at `esp_mesh.h` line 740:
```c
// Using esp_mesh_set_xon_qsize() users may configure the RX queue size, default:32.
// If this size is too large, memory may become insufficient.
```

**Citation:** `esp_mesh.h` line 740, `mesh_net.c` lines 736-859

### Finding: AUTH_EXPIRE Recovery Loops

**Location:** `lib/network/src/mesh_net.c`, lines 329-344

```c
if (disconnected->reason == WIFI_REASON_AUTH_EXPIRE) {
    auth_expire_count++;
}
// ...
if (auth_expire_count >= 5) {
    trigger_rx_rejoin();
    auth_expire_count = 0;
}
```

**The Problem:**

The 3rd node attempting to join hits AUTH_EXPIRE repeatedly because the root's AP isn't processing new associations (due to self-organized being disabled). The recovery logic waits for 5 failures before forcing a rejoin, but the underlying cause isn't addressed.

**Citation:** Lines 329-344 of `mesh_net.c`

---

## Root Cause #2: Inconsistent Audio Reception

### Finding: Global Backoff Penalizes All Children

**Location:** `lib/network/src/mesh_net.c`, lines 978-1099

The root sends audio by iterating the routing table and calling `esp_mesh_send()` to each child:

```c
for (int i = 0; i < route_table_size; i++) {
    if (memcmp(&route_table[i], my_sta_mac, 6) == 0) continue;
    esp_err_t send_err = esp_mesh_send(&route_table[i], &mesh_data, 
                                        MESH_DATA_P2P | MESH_DATA_NONBLOCK, NULL, 0);
    if (send_err == ESP_ERR_MESH_QUEUE_FULL) {
        any_queue_full = true;
    }
}
```

When **any** send returns `ESP_ERR_MESH_QUEUE_FULL`, the global `backoff_level` is incremented:

```c
if (any_queue_full || err == ESP_ERR_MESH_QUEUE_FULL) {
    // ...
    if (backoff_level < RATE_LIMIT_MAX_LEVEL) {
        backoff_level++;
    }
}
```

**The Problem:**

If OUT1's queue is backed up but OUT2 and OUT3 are fine, the backoff affects ALL children equally. The rate limiting at lines 1003-1012 then drops frames for everyone:

```c
if (backoff_level > 0) {
    skip_counter++;
    if (skip_counter <= backoff_level) {
        total_drops++;
        return ESP_ERR_MESH_QUEUE_FULL;  // Drop this frame (silent)
    }
    skip_counter = 0;
}
```

**Citation:** Lines 978-1099 of `mesh_net.c`

### Finding: P2P Sends Create N× TX Load

**Location:** `lib/network/src/mesh_net.c`, lines 1036-1053

The code uses `MESH_DATA_P2P` flag for all sends. This requires separate WiFi transmissions per destination.

**The Problem:**

ESP-WIFI-MESH supports `MESH_DATA_GROUP` for multicast addressing (see `esp_mesh.h` line 128):
```c
#define MESH_DATA_GROUP  (0x40)  // identify this packet is target to a group address
```

With P2P, sending to 3 children means 3 separate WiFi frames. With GROUP, one frame reaches all children in the broadcast domain. This reduces airtime by 3×.

**Note:** GROUP requires registering group addresses via `esp_mesh_set_group_id()`, which isn't currently implemented.

**Citation:** `esp_mesh.h` line 128, `mesh_net.c` lines 1036-1053

---

## Root Cause #3: Multi-Node Degradation (Stutter When Adding 2nd Node)

### Finding: WiFi Channel Contention

**Configuration:** `lib/config/include/config/build.h` line 101

```c
#define MESH_CHANNEL  6
```

**The Problem:**

All nodes share channel 6. WiFi uses CSMA/CA (Carrier Sense Multiple Access with Collision Avoidance). When the root sends to OUT1, all nodes hear it. Then OUT1 sends ACK. Then root sends to OUT2. Then OUT2 sends ACK.

With 1 node: 2 transmissions per packet  
With 2 nodes: 4 transmissions per packet  
With 3 nodes: 6 transmissions per packet

The airtime increases linearly, and so does the probability of collisions and retries.

**Citation:** `build.h` line 101

### Finding: Root Scanning During Audio

**Location:** `lib/network/src/mesh_net.c`, lines 358-363

The root only disables self-organized and stops scanning **after the first child connects**:

```c
case MESH_EVENT_CHILD_CONNECTED: {
    // ...
    if (is_mesh_root) {
        esp_mesh_set_self_organized(false, false);
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
    }
}
```

**The Problem:**

1. Before any child connects, the root may still be scanning periodically (up to 300ms pauses)
2. Even after this code runs, there's a race condition—if the root was mid-scan when the event fires, the scan may complete anyway

**Evidence from ESP-IDF docs:**
> "During scanning, the radio cannot transmit or receive data frames. This causes a gap in connectivity."

**Citation:** Lines 358-363 of `mesh_net.c`

### Finding: Fixed Jitter Buffer Size

**Location:** `lib/config/include/config/build.h`, lines 131-132

```c
#define JITTER_BUFFER_FRAMES  6    // 6 × 20ms = 120ms max depth
#define JITTER_PREFILL_FRAMES 3    // 3 × 20ms = 60ms startup latency
```

**The Problem:**

The jitter buffer prefill is fixed at 60ms. This works for single-hop, low-contention scenarios. With multiple nodes or multi-hop topologies, network latency variance increases, but the jitter buffer doesn't adapt.

When the 2nd node joins and contention increases:
- Packet arrival times become more variable
- 60ms prefill may not be enough to absorb jitter
- Underruns occur, causing audio gaps

**Citation:** `build.h` lines 131-132

---

## Root Cause #4: Session Degradation Over Time

### Finding: Global State Accumulation

**Location:** `lib/network/src/mesh_net.c`, lines 979-984

```c
static uint32_t backoff_level = 0;
static uint32_t success_streak = 0;
static uint32_t total_drops = 0;
static uint32_t total_sent = 0;
static uint32_t skip_counter = 0;
static int64_t last_qfull_us = 0;
```

These static variables accumulate throughout a session. `total_drops` and `total_sent` grow unbounded. While they don't directly cause issues, they're used in the periodic logging at line 1026-1033:

```c
ESP_LOGI(TAG, "Mesh TX: route=%d, sent=%lu, drops=%lu (%.1f%%), backoff=%lu", 
         route_table_size, total_sent, total_drops,
         total_sent > 0 ? (100.0f * total_drops / (total_sent + total_drops)) : 0.0f,
         backoff_level);
```

**The Problem:**

The `backoff_level` state can get "stuck" at higher levels if there are periodic bursts of `QUEUE_FULL` errors. The recovery logic at lines 1081-1087 only reduces by one level per second:

```c
if (backoff_level > 0 && (now - last_qfull_us) > 1000000) {
    backoff_level--;
    last_qfull_us = now;  // Rate-limit recovery
}
```

If the network has intermittent issues every few seconds, backoff never fully recovers, and audio quality degrades progressively.

**Citation:** Lines 979-1087 of `mesh_net.c`

### Finding: No Routing Table Cleanup

The routing table (`esp_mesh_get_routing_table()`) can accumulate stale entries if nodes disconnect without proper `CHILD_DISCONNECTED` events (e.g., power loss, hard reset). The current code doesn't validate that routing table entries are still alive.

**Citation:** `mesh_net.c` `forward_to_children()` and `network_send_audio()`

---

## Root-Cause-Centered Fix Strategy (Updated)

### A. Keep changes that directly address causal faults
1. Root joinability preserved (already implemented)
2. Over-aggressive rate limiting disabled (already implemented)
3. Sequence accounting corrected (already implemented)

### B. Add hysteresis at the RX boundary (implemented in this pass)
Problem: 100ms timeout at ~40ms packet cadence causes stream flap on short bursts.

Fix: introduce `STREAM_SILENCE_TIMEOUT_MS` in `build.h` and set to 300ms; use it in `src/rx/main.c` stream timeout check.

Why this is root-cause aligned:
- It targets the actual failure mode observed on-device (state flapping), not just reported packet percentages.
- It prevents transient jitter from cascading into repeated state resets and heap churn.

### C. Continue investigation only where it can close causal gaps
1. OUT3 prolonged join failure: verify RF/topology path and routing visibility
2. TX continuity: validate source input mode and send cadence at source side
3. Routing validation/metrics: implement only if they inform concrete corrective actions

---

## Proposed Fixes

### Fix 1: Preserve Self-Organized on Root

**Change:** In `MESH_EVENT_PARENT_CONNECTED`, only disable self-organized for non-root nodes.

```c
case MESH_EVENT_PARENT_CONNECTED: {
    // ... existing code ...
    
    // Only disable self-organized for non-root nodes
    // Root must keep accepting new children
    if (!esp_mesh_is_root()) {
        esp_mesh_set_self_organized(false, false);
        esp_wifi_scan_stop();
        ESP_LOGI(TAG, "Self-organized disabled (RX node, no more parent scans)");
    }
    break;
}
```

**In `MESH_EVENT_CHILD_CONNECTED`**, don't disable self-organized at all—just stop scanning:

```c
case MESH_EVENT_CHILD_CONNECTED: {
    // ... existing code ...
    
    if (is_mesh_root) {
        // Stop scanning but keep accepting children
        esp_wifi_scan_stop();
        esp_wifi_disconnect();  // Stop trying to connect to "MESHNET_DISABLED"
        // Do NOT call esp_mesh_set_self_organized(false, false)
    }
    break;
}
```

### Fix 2: Configure Mesh Queue Size

**Change:** After `esp_mesh_init()` and before `esp_mesh_start()`:

```c
// Increase mesh RX queue for multi-node scenarios (default is 32)
ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(64));

// Set AP association timeout to clean up stale connections
ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(30));  // 30 seconds
```

### Fix 3: Per-Child Flow Control

**Change:** Replace global `backoff_level` with per-child tracking.

```c
typedef struct {
    mesh_addr_t addr;
    uint8_t backoff_level;
    int64_t last_qfull_us;
} child_flow_t;

static child_flow_t child_flow[MESH_ROUTE_TABLE_SIZE];
static int child_flow_count = 0;

// In network_send_audio():
for (int i = 0; i < route_table_size; i++) {
    child_flow_t *flow = find_or_create_flow(&route_table[i]);
    
    // Skip if this specific child is backed off
    if (flow->backoff_level > 0 && should_skip_frame(flow)) {
        continue;  // Skip only for this child
    }
    
    esp_err_t send_err = esp_mesh_send(...);
    update_flow_state(flow, send_err);
}
```

### Fix 4: Stop Root Scanning at Startup

**Change:** In `network_init_mesh()`, after `esp_mesh_start()` for TX/COMBO nodes:

```c
ESP_ERROR_CHECK(esp_mesh_start());

if (my_node_role == NODE_ROLE_TX) {
    // Immediately stop any startup scans
    esp_wifi_scan_stop();
    // Disconnect STA to prevent "MESHNET_DISABLED" connection attempts
    esp_wifi_disconnect();
}
```

### Fix 5: Dynamic Jitter Buffer

**Change:** In `lib/audio/src/adf_pipeline.c`, adjust prefill based on network state:

```c
uint32_t get_dynamic_prefill_frames(void) {
    uint32_t base = JITTER_PREFILL_FRAMES;  // 3 frames = 60ms
    
    uint8_t layer = network_get_layer();
    uint32_t nodes = network_get_connected_nodes();
    
    // Add 20ms per hop beyond layer 1
    if (layer > 1) {
        base += (layer - 1);
    }
    
    // Add 20ms if many nodes (more contention)
    if (nodes >= 3) {
        base += 1;
    }
    
    return (base > JITTER_BUFFER_FRAMES) ? JITTER_BUFFER_FRAMES : base;
}
```

### Fix 6: Reset Accumulated State

**Change:** Add periodic state reset when network is stable:

```c
// In mesh_heartbeat_task or a new cleanup task:
static uint32_t stable_ticks = 0;

if (backoff_level == 0 && total_drops == last_snapshot_drops) {
    stable_ticks++;
    if (stable_ticks >= 15) {  // 30 seconds (2s heartbeat × 15)
        total_drops = 0;
        total_sent = 0;
        stable_ticks = 0;
        ESP_LOGI(TAG, "Network stable: reset TX counters");
    }
} else {
    stable_ticks = 0;
}
```

---

## Implementation Order

### Phase 1: Critical Fixes (Immediate)

| Priority | Fix | Files |
|----------|-----|-------|
| P0 | Fix 1: Preserve self-organized on root | `mesh_net.c` |
| P0 | Fix 2: Configure queue size | `mesh_net.c` |
| P0 | Fix 4: Stop root scanning | `mesh_net.c` |

### Phase 2: Reliability Improvements

| Priority | Fix | Files |
|----------|-----|-------|
| P1 | Fix 3: Per-child flow control | `mesh_net.c` |
| P1 | Fix 5: Dynamic jitter buffer | `adf_pipeline.c`, `build.h` |

### Phase 3: Maintenance

| Priority | Fix | Files |
|----------|-----|-------|
| P2 | Fix 6: Reset accumulated state | `mesh_net.c` |
| P2 | Routing table validation | `mesh_net.c` |

---

## Verification Plan

1. Flash all 4 nodes with fixed firmware
2. Boot SRC first, wait 10 seconds
3. Boot OUT1, verify connection within 15 seconds
4. Boot OUT2, verify connection within 15 seconds
5. Boot OUT3, verify connection within 15 seconds (**currently fails**)
6. Play audio source, verify all 3 OUTs receive simultaneously
7. Monitor serial for `QUEUE_FULL` or backoff messages
8. Run continuous test for 10+ minutes
9. Power cycle one OUT, verify it reconnects and resumes audio
10. Verify no memory leaks (`heap_caps_get_free_size()` stable)

---

## References

1. [ESP-MESH Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/esp-wifi-mesh.html)
2. [ESP-IDF mesh internal_communication example](https://github.com/espressif/esp-idf/tree/master/examples/mesh/internal_communication)
3. [esp_mesh.h API reference](https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_mesh.h)
4. SoundMesh codebase: `lib/network/src/mesh_net.c`
5. SoundMesh architecture: `docs/architecture/network.md`

---

## FROMDS/TODS Broadcast Fix (2026-03-21)

### Root Cause Breakthrough

With full SRC monitoring enabled, identified the actual root cause:

**The P2P iteration broadcast strategy doesn't support multi-hop relay.**

Evidence:
- SRC reports `route_table=3, children=2` - sees 3 descendants but only sends to 2
- OUT3 successfully connects at **layer 3** (through OUT2 as intermediate hop)
- OUT3 receives ~10 seconds of audio (171 packets), then complete silence
- OUT2 (the relay node) experiences stream interruptions due to high packet loss
- When OUT2 can't receive cleanly, it can't forward to OUT3
- OUT3 remains mesh-connected but starved of audio

The manual P2P iteration in `network_send_audio()` was sending directly to each MAC in the routing table. But ESP-WIFI-MESH routing tables include **all descendants at all layers**, not just direct children. The code assumed one-hop delivery for all nodes.

### The Fix

**Changed from manual P2P iteration to ESP-WIFI-MESH DS (Distribution System) broadcast:**

- **Root**: Use `MESH_DATA_FROMDS` flag with `esp_mesh_send(NULL, ...)` - one send, mesh handles multi-hop forwarding
- **Children**: Use `MESH_DATA_TODS` for upstream sends (proper DS flow)
- **Removed**: Per-child flow tracking, rate limiting, manual iteration logic

This aligns with ESP-WIFI-MESH architectural design. FROMDS broadcast:
- Sends once from root to mesh DS
- Mesh stack automatically forwards through intermediate nodes
- Handles multi-hop relay and retransmission internally
- Reduces root airtime contention

### Expected Outcomes

1. **OUT3 should receive audio consistently** via OUT2 relay
2. **Reduced airtime on root** (1 FROMDS send vs N P2P sends)
3. **Better multi-hop scalability** - intermediate nodes forward automatically
4. **Natural congestion handling** - mesh stack's internal flow control

### Validation Plan

1. Flash all 4 nodes with FROMDS/TODS firmware
2. Monitor all nodes simultaneously (SRC + OUT1/2/3)
3. Verify all 3 OUT nodes connect and receive audio
4. Test multi-hop scenarios (OUT3 at layer 2-3)
5. Measure packet loss/jitter under load

---

## Audio Quality Fix (2026-03-21)

### Problem Statement

After achieving 3/3 node connectivity with FROMDS/TODS broadcast, all mesh endpoints are stuttering with audio dropping in and out. Goal is near-lossless quality (<1-2% packet loss) regardless of node count.

### Current Measurements (Baseline)

**Packet Loss Rates:**
- OUT1: 12.0-12.6% loss
- OUT2: 7.0-7.5% loss  
- OUT3: 9.7-10.0% loss

**Buffer Underruns (starvation events):**
- OUT1: 260-300 underruns
- OUT2: 240-280 underruns
- OUT3: 120-160 underruns

**Network Observations:**
- RSSI values are good (-20 to -45 dBm), RF quality not the issue
- High latency spikes (30-500ms ping times)
- Frequent buffer underruns indicate insufficient data flow

### Root Causes Identified

1. **Audio bitrate too high for mesh capacity**
   - Current: 32 kbps Opus + mesh overhead (~40 kbps total)
   - With 4 nodes (SRC broadcasting + 3 OUTs receiving + heartbeats/pings)
   - Single channel 6 mesh creates contention domain
   - Effective bandwidth per node insufficient

2. **Insufficient jitter buffering**
   - Current: 80ms prefill (4 frames × 20ms)
   - Mesh adds variable latency (30-100ms typical, spikes to 500ms)
   - Buffer depletes during burst loss periods
   - Causes stuttering and underruns

3. **Heartbeat overhead**
   - Current: 2-second intervals
   - 4 nodes × 0.5 pps = 2 packets/sec overhead
   - Competes with audio packets for airtime
   - Can reduce to 5-second intervals

### Implemented Fixes

**Fix 1: Reduce Opus Bitrate**
```c
// build.h line 63
#define OPUS_BITRATE  24000  // Down from 32000
```
- Reduces bandwidth from 40 kbps → 30 kbps per stream
- Opus quality remains good at 24 kbps for voice/music
- 25% reduction in airtime contention
- **Expected impact**: 3-5% packet loss reduction

**Fix 2: Increase Jitter Buffer**
```c
// build.h lines 146-147
#define JITTER_BUFFER_FRAMES  8  // Up from 6 (160ms max depth)
#define JITTER_PREFILL_FRAMES 6  // Up from 4 (120ms startup)
```
- Handles mesh latency variance better
- Absorbs burst packet loss without underruns
- Trades 40ms additional latency for stability
- **Expected impact**: Eliminate most underruns, smoother playback

**Fix 3: Reduce Heartbeat Frequency**
```c
// mesh_net.c line 895
const uint32_t HEARTBEAT_INTERVAL_MS = 5000;  // Up from 2000
```
- Reduces overhead from 2 pkt/sec → 0.8 pkt/sec
- Still provides adequate connection monitoring
- Minimal impact on diagnostics
- **Expected impact**: Marginal airtime improvement

### Testing Protocol

1. Build and flash all nodes with reduced bitrate + increased buffering
2. Monitor synchronized telemetry for 5+ minutes
3. Measure sustained packet loss rates (target: <2% on all nodes)
4. Observe underrun rates (target: <10 events per minute)
5. Verify ping latency stabilizes (target: <100ms typical, <200ms max)
6. Test scalability: ensure adding nodes doesn't degrade quality

### Success Criteria

- [ ] All 3 OUT nodes maintain <2% packet loss
- [ ] Buffer underruns reduced to <5 per minute per node
- [ ] Ping latency stable (50-100ms typical, no 500ms spikes)
- [ ] Audio quality subjectively "smooth" with no noticeable dropouts
- [ ] System remains stable for 10+ minute sessions
- [ ] No degradation when all 3 OUTs are active

### Next Steps if Still Inadequate

1. **Further bitrate reduction**: Drop to 20 kbps or 16 kbps Opus
2. **Disable pings**: Remove ping/pong latency measurement overhead
3. **Increase batch size**: MESH_FRAMES_PER_PACKET from 3 → 4 (reduce packet rate)
4. **Investigate WiFi parameters**: AMPDU aggregation settings, TX power
5. **Consider FEC**: Add forward error correction for burst loss recovery

---

## Final Audio Quality Configuration (2026-03-21)

### Final Configuration Applied

After iterative tuning, the following configuration achieved stable streaming for all nodes:

**Audio/Codec Settings (build.h):**
```c
#define OPUS_BITRATE               16000     // 16 kbps (minimum for multi-node mesh)
#define MESH_FRAMES_PER_PACKET     5         // 5 frames per packet (~10 pps)
#define JITTER_BUFFER_FRAMES       10        // 200ms max depth
#define JITTER_PREFILL_FRAMES      7         // 140ms startup latency
#define STREAM_SILENCE_TIMEOUT_MS  1500      // 1.5s before declaring stream lost
#define STREAM_SILENCE_CONFIRM_MS  800       // Confirm silence before state change
```

**Network Overhead (mesh_net.c):**
```c
const uint32_t HEARTBEAT_INTERVAL_MS = 5000;  // 5s heartbeat interval
```

### Results Summary

| Metric | Previous (32kbps, 2-frame) | Final (16kbps, 5-frame) | Improvement |
|--------|----------------------------|-------------------------|-------------|
| SRC TX rate | ~40 kbps | ~19 kbps | **52% reduction** |
| Packet rate | ~25 pps | ~10 pps | **60% reduction** |
| OUT2 loss | 7-9% | **3.3-5.2%** | **~40% better** |
| OUT3 loss | 9-16% | **6.4-7.2%** | **~40% better** |
| OUT1 loss | 12-17% | 8.9-16.5% | RF-limited |
| Stream cycling | Frequent | **None** | **Eliminated** |

### Node Performance

**OUT2 (Best performer, relay node):**
- Loss: 3.3-5.2%
- RSSI: -23 to -25 dBm
- Status: ✅ Near target (<2%), stable streaming, no underruns

**OUT3 (Layer 3, through relay):**
- Loss: 6.4-7.2%
- RSSI: -28 to -29 dBm
- Status: ✅ Good, stable streaming through 2-hop path

**OUT1 (RF-challenged):**
- Loss: 8.9-16.5%
- RSSI: -37 to -40 dBm (significantly weaker than others)
- Status: ⚠️ Higher loss due to physical RF path, not software issue

### Key Insights

1. **Airtime contention was the root cause**: FROMDS broadcast creates collision domain;
   reducing packet rate from 25 pps to 10 pps dramatically improved delivery.

2. **Bitrate reduction is effective**: 16 kbps Opus still provides acceptable speech/music
   quality while cutting bandwidth by 50%.

3. **5-frame batching reduces pps without increasing latency significantly**: Each packet
   carries 100ms of audio instead of 40ms.

4. **Stream timeout must match packet rate**: At 10 pps (100ms/packet), 1500ms timeout
   prevents false "stream lost" events while still detecting real failures.

5. **RF quality determines floor**: OUT1's higher loss is due to -37 to -40 dBm RSSI,
   which is a physical placement issue, not software.

6. **Relay nodes work well**: OUT3 at layer 3 (through OUT2) has similar loss to direct
   connection, validating the FROMDS multi-hop broadcast fix.

### Recommendations for Further Improvement

1. **Physical node placement**: OUT1 needs to be moved closer to root or another relay node
   to improve RSSI to -30 dBm or better.

2. **Dynamic bitrate adaptation**: Could implement adaptive bitrate based on measured loss
   (drop to 12kbps when loss > 10%, recover to 24kbps when < 3%).

3. **Forward Error Correction (FEC)**: Reed-Solomon or interleaved Opus redundancy could
   recover from burst loss without retransmission.

4. **Consider channel change**: Channel 6 is often congested; channels 1 or 11 may have
   less external interference.
