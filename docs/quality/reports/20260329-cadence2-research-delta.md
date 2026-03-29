# Cadence2 research delta: periodic artifacts in real-time audio (2026-03-29)

## Scope and delta from prior round
This report adds **delta research** beyond `20260329-periodic-stutter-research.md`, focused on why robotic/stutter artifacts recur every few seconds and what mature real-time stacks do to avoid cadence-locked failures.

Primary questions:
1. Established patterns from NetEq/WebRTC, Asterisk, PJMEDIA, RTP/RTCP/RTCP-XR for periodic bursts and jitter-buffer oscillation.
2. Mesh/control-plane scheduling practices to avoid timer coupling with media cadence.
3. Concrete, codebase-specific recommendations with parameter ranges and risk tradeoffs.

---

## 1) Established patterns for periodic bursts and oscillating jitter buffers

### 1.1 NetEq/WebRTC: strict playout cadence + adaptive delay + operation telemetry
Key pattern (WebRTC NetEq docs):
- `InsertPacket` updates inter-arrival statistics and target delay.
- `GetAudio` is a fixed-rate playout pull (10 ms ticks in NetEq) that continuously compares current delay vs target and applies controlled time scaling (accelerate/decelerate).
- Output operations are explicitly classified (`Normal`, `Expand`, `Merge`, `Acceleration`, `Preemptive expand`, `CNG`) and exposed via stats.

Why this matters for periodic artifacts:
- A fixed prefill without adaptive target/hysteresis tends to produce sawtooth occupancy under bursty arrivals.
- Sawtooth occupancy creates a repeating audible pattern: buffer drains -> conceal/expand -> rebuffer -> resume -> repeat.

### 1.2 Asterisk: explicit knobs for resync threshold and adaptive padding
Asterisk jitterbuffer function exposes configuration around:
- `max_size` (buffer ceiling),
- `resync_threshold` (timestamp jump that forces re-sync),
- `target_extra` (extra adaptive padding).

Why this matters:
- Mature VoIP systems treat timestamp discontinuity handling and adaptive padding as first-class anti-oscillation controls, rather than relying on a single static prefetch value.

### 1.3 PJMEDIA: adaptive/fixed modes + bounded discard policies
PJMEDIA jitter buffer supports:
- fixed vs adaptive mode,
- min/max prefetch bounds,
- configurable discard algorithms (`NONE`, `STATIC`, `PROGRESSIVE`).

Why this matters:
- Oscillation control is not just about adding delay; it also requires bounded discard/convergence behavior to prevent overcorrection loops.

### 1.4 RTP/RTCP + RTCP-XR: de-synchronize control timers and measure burst behavior
RTP/RTCP timing model (RFC 3550):
- RTCP intervals are intentionally randomized and include reconsideration logic to avoid synchronization bursts.

RTCP-XR (RFC 3611):
- emphasizes burst metrics and jitter-buffer metrics (not only average loss/jitter), including VoIP-oriented burst/discard/delay views.

Why this matters:
- Fixed-period control loops in media systems can phase-lock into recurring queue pressure.
- Burst-aware telemetry is required to diagnose cadence artifacts; mean loss alone hides the mechanism.

---

## 2) Mesh/control-plane scheduling best practices to avoid timer coupling with media cadence

### 2.1 Keep media cadence independent from housekeeping cadence
- Media pipeline should run on deterministic audio cadence (already true conceptually in this codebase).
- Control loops (heartbeat, probe, reconnect eligibility, health checks) should avoid exact harmonic relationships with media frame cadence (20 ms) and common buffer drain windows.

### 2.2 Prefer randomized interval windows over fixed metronomes
From RFC 3550-style practice:
- Replace strict N-second periodic emissions with bounded randomized windows.
- Keep jitter bounded (small enough for responsiveness, large enough to break phase lock).

### 2.3 Use event-gated control work and budgeted control TX under backpressure
From ESP-WIFI-MESH guidance:
- gate activity from mesh events,
- avoid app-level Wi-Fi API interference in self-organized mode,
- avoid unnecessary control fanout during stressed transport windows.

### 2.4 Observe burstiness and occupancy trajectories, not only scalar averages
- Track run-length metrics (consecutive low-throughput seconds, consecutive `buf=0` windows, burst-loss lengths, rebuffer cadence).
- Detect periodicity around known control windows (e.g., ~2s/~5s bins) to prove or falsify timer coupling.

---

## 3) Codebase-specific recommendations (parameter ranges + risk tradeoffs)

Current relevant settings (code evidence):
- `MESH_FRAMES_PER_PACKET=2`
- `TRANSPORT_GROUP_FAIL_WINDOW_MS=2300` and jittered recovery windows
- `OUT_REJOIN_MIN_INTERVAL_MS=2000`
- `JITTER_BUFFER_FRAMES=8`, `JITTER_PREFILL_FRAMES=5`
- `RX_UNDERRUN_REBUFFER_MISSES=14`
- `RX_REBUFFER_STALL_RECOVERY_MS=1500`
- `mesh_heartbeat_task` uses fixed 5000 ms heartbeat interval

### Recommendation A — Break remaining fixed timer coupling (highest priority)
Changes:
1. Keep existing transport fail-window jittering (already present).
2. Add bounded jitter to remaining fixed periodic control loops:
   - OUT rejoin minimum interval effective window: **2000 ms ± 200–400 ms**.
   - heartbeat send interval window: **5000 ms ± 400–800 ms**.
3. Keep jitter bounded to avoid long control blind spots.

Expected effect:
- Reduces phase lock between control-plane bursts and buffer drain periodicity.

Risk tradeoff:
- Slightly less deterministic control timing/log cadence.
- Very low risk to media correctness.

### Recommendation B — Introduce bounded adaptive prefill target (NetEq/PJMEDIA style)
Changes:
1. Keep startup prefill baseline at 5 frames (100 ms).
2. Add dynamic target prefill range tied to short-window conditions:
   - normal target: **5–6 frames**,
   - stressed target (burst/jitter window): **7–9 frames**,
   - cap by `RX_REBUFFER_PREFILL_MAX_FRAMES`.
3. Use fast-up / slow-down hysteresis:
   - increase target after **2–3 consecutive** stress windows,
   - decrease only after **8–12 consecutive** stable windows.

Expected effect:
- Dampens occupancy sawtooth and lowers repeating underrun cadence.

Risk tradeoff:
- Adds variable latency (+40 to +80 ms in stress windows).
- Medium implementation complexity.

### Recommendation C — Tighten rebuffer resume hysteresis to avoid immediate relapse
Changes:
- Evaluate raising `RX_REBUFFER_RESUME_HOLD_MS` from 60 ms into **80–160 ms** range.
- Evaluate `RX_REBUFFER_RESUME_HYSTERESIS_FRAMES` from 2 to **2–3** under stress mode.

Expected effect:
- Prevents resume-too-early behavior in recurring short bursts.

Risk tradeoff:
- Slight extra resume latency after stalls.
- Low/medium risk; easy rollback.

### Recommendation D — Packetization A/B under burst-loss profile
Changes:
- A/B test `MESH_FRAMES_PER_PACKET`:
  - mode 1: `2` (current, lower pps, higher per-loss damage),
  - mode 2: `1` (higher pps, lower per-loss damage).

Expected effect:
- In bursty links, 1 frame/packet often reduces audible "robotic chunking" even if packet rate rises.

Risk tradeoff:
- Higher packet rate may increase mesh overhead and queue pressure.
- Medium risk; must validate against `tx_audio_queue_full` and drops.

### Recommendation E — Control-plane shaping under backpressure
Changes:
- When audio backpressure level rises (already tracked in TX stats), rate-limit non-essential control fanout and defer non-urgent probes.
- Maintain heartbeat liveness but avoid synchronized multi-message control bursts.

Expected effect:
- Preserves audio transport continuity during stress windows.

Risk tradeoff:
- Slower convergence of some control/state updates.
- Medium risk; should be bounded by maximum defer interval.

### Recommendation F — Add cadence-specific observability (XR-style mindset)
Add counters/telemetry for:
- burst-loss run length histogram,
- consecutive low-RX-kbps windows,
- jitter-buffer occupancy slope/sawtooth period,
- periodicity score around 2s/3s/5s bins,
- operation mix (normal/decode, conceal, rebuffer, fade/expand analogs).

Expected effect:
- Rapid validation whether audible artifacts are timer-coupled vs pure RF degradation.

Risk tradeoff:
- Minor telemetry overhead.
- Low risk.

---

## 4) Suggested rollout order for this repository
1. **Immediate (low risk):** Recommendation A + F.
2. **Next:** Recommendation C.
3. **Then:** Recommendation D (A/B experiment).
4. **Then:** Recommendation B.
5. **Parallel hardening:** Recommendation E.

Success criteria for cadence2:
- Eliminate strong periodic peak in underrun intervals (currently ~3s-class in forensics report).
- Reduce `buf=0%` recurrence rate and shorten starvation runs.
- Keep end-to-end latency increase within acceptable demo budget.

---

## References / sources

### Project-local evidence
- `lib/config/include/config/build.h`
- `lib/network/src/mesh/mesh_tx.c`
- `lib/network/src/mesh/mesh_heartbeat.c`
- `src/out/main.c`
- `docs/quality/reports/20260329-periodic-stutter-research.md`
- `docs/quality/reports/20260329-periodic-stutter-forensics.md`
- `docs/quality/reports/20260329-urgent-stutter-live-monitor.md`

### External references
- WebRTC NetEq overview:  
  https://chromium.googlesource.com/external/webrtc/+/refs/heads/main/modules/audio_coding/neteq/g3doc/index.md
- Asterisk JITTERBUFFER function docs:  
  https://wiki.asterisk.org/wiki/display/AST/Asterisk+14+Function_JITTERBUFFER
- PJMEDIA jitter buffer docs:  
  https://www.pjsip.org/pjmedia/docs/html/group__PJMED__JBUF.htm
- RTP/RTCP timing model (randomized intervals/reconsideration):  
  https://www.rfc-editor.org/rfc/rfc3550.txt
- RTCP XR (burst/jitter-buffer oriented reporting model):  
  https://www.rfc-editor.org/rfc/rfc3611.txt
- RTP payload for Opus (packetization/FEC considerations):  
  https://www.rfc-editor.org/rfc/rfc7587.txt
- ESP-WIFI-MESH Programming Guide:  
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/esp-wifi-mesh.html
- ESP-WIFI-MESH API/Programming Model:  
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp-wifi-mesh.html
