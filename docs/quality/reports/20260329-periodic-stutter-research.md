# Periodic stutter / robotic artifact research (2026-03-29)

## Scope
Focused research on periodic (~2s cadence) robotic/stutter artifacts in real-time mesh audio, combining:
1) ESP-IDF mesh guidance,
2) Opus + jitter-buffer/NetEq patterns,
3) RTP/WebRTC/VoIP periodic glitch causes,
4) concrete recommendations for this codebase.

## Current codebase signals relevant to ~2s periodicity

- Transport/control constants include multiple 2s windows:
  - `TRANSPORT_GROUP_FAIL_WINDOW_MS=2000`
  - `OUT_REJOIN_MIN_INTERVAL_MS=2000`
  - `CONTROL_HEARTBEAT_RATE_MS=2000`
  - (`lib/config/include/config/build.h`)
- Mesh heartbeat task interval is 5s (`lib/network/src/mesh/mesh_heartbeat.c`), but other 2s control/recovery windows still exist.
- TX fanout policy has short-window mode switching and fallback logic (`lib/network/src/mesh/mesh_tx.c`), which can create cadence if thresholds align with queue pressure cycles.
- Recent evidence packet shows repeated underrun/rebuffer loops and high loss/churn, consistent with starvation oscillation rather than one-time failures:
  - `underruns_per_min=16.6`, `loss_pct=19.9`, `reason201_per_min=21.2`, `buf0_events_per_min=33.2`
  - (`docs/quality/reports/20260329-demo-candidate-freeze.md`, `docs/quality/reports/20260329-urgent-stutter-live-monitor.md`)

---

## Research findings from established docs/projects

### A) ESP-IDF mesh behavior relevant to periodic artifacts

**From ESP-WIFI-MESH guide/API docs:**
- Mesh is event-driven; app should gate traffic on mesh readiness events.
- In self-organized mode, app-level Wi-Fi API calls can interfere with mesh internals.
- Parent switching, root behavior, and channel switching can introduce transient path/latency changes.
- Upstream flow control is explicit (receiving window) and overload/backpressure can occur under bursts.
- Performance depends strongly on PER/hop count and environment; mesh channel/router switching can cause disruption windows.

**Implication:** fixed, synchronized control timers + adaptive route/recovery behavior can produce recurring queue-pressure windows that audio pipeline experiences as periodic starvation.

### B) Opus + jitter buffer / NetEq patterns

**From RFC 7587 + WebRTC NetEq docs:**
- Opus sweet spot for 20 ms fullband mono music is 48–64 kbps (you are in-range).
- In-band FEC is useful but only when receiver requests/uses it correctly and packet timing remains sane.
- NetEq’s core stability pattern: strict periodic playout pulls (10 ms ticks), adaptive target delay from interarrival stats, and time-scale operations (accelerate/decelerate) to avoid oscillation.
- Robust engines distinguish: normal decode vs concealment vs expand/merge and track lifetime + interval stats.

**Implication:** fixed prefill alone is not enough under bursty periodic jitter; adaptive delay control with clear hysteresis is usually required to prevent refill/drain oscillation.

### C) RTP/VoIP periodic glitch patterns

**From RFC 3550 / RFC 3611 / Asterisk / PJMEDIA docs:**
- RTP control traffic timing (RTCP intervals) intentionally uses randomized/scaled intervals to avoid synchronization bursts.
- RTCP XR/VoIP metrics emphasize burst-loss and jitter-buffer telemetry, not just average loss.
- Mature VoIP stacks expose adaptive/fixed jitter modes, resync thresholds, prefetch bounds, and discard algorithms with hysteresis.

**Implication:** exact-period housekeeping (2s, 5s, etc.) can alias with playout buffering and cause “robotic every N seconds” symptoms unless de-synchronized and measured with burst-aware metrics.

---

## Ranked recommendations (impact vs implementation risk)

### 1) De-synchronize periodic control/recovery timers (highest impact, low risk)
**Likely impact:** Very high  
**Implementation risk:** Low

- Add small randomized jitter (e.g., ±150–300 ms) to 2s-class control loops/windows:
  - `TRANSPORT_GROUP_FAIL_WINDOW_MS`
  - `OUT_REJOIN_MIN_INTERVAL_MS`
  - control heartbeat cadence paths
- Goal: prevent timer phase-lock that can repeatedly align control bursts with audio playout depletion.
- This follows RTP/RTCP practice of avoiding synchronized control emission (RFC 3550 timing model).

### 2) Split/shape audio vs control traffic budgets in mesh TX path (high impact, medium risk)
**Likely impact:** High  
**Implementation risk:** Medium

- Preserve strict priority/credit budget for audio payload sends when queue pressure rises.
- Throttle non-audio control fanout during detected backpressure windows.
- Keep queue-full streak escalation for audio conservative and avoid control bursts during fallback transitions.

### 3) Add burst-aware transport metrics + cadence detector (high impact, low risk)
**Likely impact:** High  
**Implementation risk:** Low

- Log rolling metrics for:
  - burst-loss length distribution,
  - interarrival jitter p50/p95/p99,
  - jitter-buffer occupancy sawtooth period,
  - periodicity score around 2s/5s bins.
- Map to RTCP-XR-style observability (burst + jitter-buffer metrics) to confirm root cause quickly.

### 4) Introduce adaptive jitter target with hysteresis (high impact, medium/high risk)
**Likely impact:** High  
**Implementation risk:** Medium-High

- Keep current floor prefill, but add adaptive target delay (increase fast on burst jitter; decrease slowly on stability).
- Prevent rapid refill/drain toggling via hysteresis and min hold times.
- NetEq pattern to emulate: continuous delay optimization + controlled time-scale adjustments.

### 5) Revisit packetization depth under current loss mode (medium-high impact, medium risk)
**Likely impact:** Medium-High  
**Implementation risk:** Medium

- A/B test `MESH_FRAMES_PER_PACKET=2` vs `1` under same conditions.
- At high burst loss, smaller packets often reduce audible damage per loss event (despite higher pps).
- Decide with artifact score + underrun/burst-loss metrics, not only average throughput.

### 6) Harden mesh channel/root stability policy for demo-critical mode (medium impact, medium risk)
**Likely impact:** Medium  
**Implementation risk:** Medium

- Ensure channel/router switch behavior and root policy are explicit for the operating environment.
- Reduce avoidable root/parent churn windows during live audio.
- Use mesh event gating and avoid Wi-Fi API interference during self-organized operation.

### 7) Keep Opus FEC path but verify effective use during bursts (medium impact, low/medium risk)
**Likely impact:** Medium  
**Implementation risk:** Low-Medium

- Keep in-band FEC enabled, but verify decoder-side request/use counters correlate with burst recovery.
- If FEC isn’t materially reducing burst artifacts, retune expected loss / packetization / prefill jointly.

---

## Why these recommendations fit this codebase specifically

- Your strongest symptom is starvation oscillation (`buf=0%`, underrun storms, periodic prefill recoveries), not codec-quality limits.
- The codebase currently has several fixed 2s-class control/recovery windows that can create cadence under load.
- Existing telemetry is good; extending it with burst/cadence observability enables rapid validation of “periodic control aliasing” vs “pure RF loss.”

## References consulted

### Project-local evidence
- `lib/config/include/config/build.h`
- `lib/network/src/mesh/mesh_tx.c`
- `lib/network/src/mesh/mesh_heartbeat.c`
- `src/out/main.c`
- `docs/quality/reports/20260329-urgent-stutter-live-monitor.md`
- `docs/quality/reports/20260329-demo-candidate-freeze.md`

### External sources
- ESP-IDF ESP-WIFI-MESH Programming Guide:  
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/esp-wifi-mesh.html
- ESP-IDF ESP-WIFI-MESH API Reference:  
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp-wifi-mesh.html
- RFC 7587 (RTP payload for Opus):  
  https://www.rfc-editor.org/rfc/rfc7587.txt
- RFC 6716 (Opus codec):  
  https://datatracker.ietf.org/doc/html/rfc6716
- RFC 3550 (RTP/RTCP timing model):  
  https://www.rfc-editor.org/rfc/rfc3550.txt
- RFC 3611 (RTCP XR metrics):  
  https://www.rfc-editor.org/rfc/rfc3611.txt
- WebRTC NetEq overview:  
  https://chromium.googlesource.com/external/webrtc/+/refs/heads/main/modules/audio_coding/neteq/g3doc/index.md
- Asterisk jitter buffer function docs:  
  https://wiki.asterisk.org/wiki/display/AST/Asterisk+14+Function_JITTERBUFFER
- PJMEDIA adaptive jitter buffer docs:  
  https://www.pjsip.org/pjmedia/docs/html/group__PJMED__JBUF.htm

