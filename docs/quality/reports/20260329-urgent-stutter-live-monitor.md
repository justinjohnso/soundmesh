# Urgent stutter live monitor evidence (2026-03-29)

## Evidence sources (freshest found)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/src-serial.log` (Mar 29 02:04)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/out-serial.log` (Mar 29 02:04)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/soak-output.log` (Mar 29 01:58)
- `docs/quality/reports/20260329-tonight/05-prefill-6-gain-safe-fec10/flash-out.log` (Mar 29 01:52)

## Marker extraction
- `TX OBS`: not present in captured logs (instrumentation/tag absent in this run)
- Fanout mode changes: explicit mode-switch logs not present; TX path shows stable `Mesh TX GROUP: descendants=3`
- `RX OBS` / `RX NET`: exact tags not present; equivalent per-second `RX:` telemetry exists
- Key counts from this run:
  - `Underrun`: **83**
  - `buf=0%`: **83**
  - `Playback prefilled`: **5**
  - SRC `reason:201`: **106**
  - SRC `Mesh TX GROUP`: **57**
  - OUT churn heartbeat (`churn(...)`): **12**

## Short timeline around bad behavior
- SRC remains actively transmitting:
  - `src-serial.log`: repeated `TX: seq=61184 ...`, `TX: seq=61440 ...`, `TX: seq=62208 ...`
  - `src-serial.log`: repeated `Mesh TX GROUP: descendants=3 result=ESP_OK ... drops=0 (0.0%)`
  - `src-serial.log`: frequent `mesh: [wifi]disconnected reason:201()` churn while still sending
- OUT stutter window example:
  - `[335s] RX ... buf=25%`
  - `Underrun #1880`
  - `[336s] RX ... buf=0%`
  - `[338s] RX ... buf=0%`
  - `Underrun #1900`
  - `Underrun #1920`
  - `[344s] RX ... buf=0%`
  - `Playback prefilled #250 (15360 bytes)` then buffer refills to 100%
  - `Underrun #1940`, then `[347s] RX ... buf=0%`

## Short diagnosis
Evidence is sufficient and consistent with live roboty/stutter caused by repeated RX starvation/rebuffer cycles: OUT buffer repeatedly drains to 0%, underruns fire, prefill recovers, then starvation repeats.

## Top 3 likely causes now
1. **Persistent transport quality loss/jitter** on OUT receive path (about 19% drop reported continuously), causing frequent decoder/playback starvation.
2. **Network churn at SRC/root** (`reason:201` recurring) adding burstiness and delivery timing variance even while aggregate TX/drops look healthy.
3. **Jitter-buffer/prefill policy still too fragile** for current loss profile (recovery occurs, but prefill depth/strategy does not prevent immediate relapse).
