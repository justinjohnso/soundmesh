# Security + Operations Audit (Internal Pilot)

Date: 2026-03-23  
Scope: OTA safety/rollback, uplink credential handling, portal API hardening, logging/observability, runbook readiness.

## Executive summary

Current state is **not pilot-ready without compensating controls**. The system has useful operational telemetry and stable build/test coverage, but key control-plane security protections are missing (API auth, OTA rollback strategy, credential minimization).  

**Readiness verdict:** **CONDITIONAL FAIL** for unattended/internal pilot.  
**Readiness verdict with immediate controls (below):** **PASS for supervised pilot** (trusted operator laptop + controlled physical access).

---

## Pass/Fail checklist

| Area | Check | Status | Evidence | Priority |
|---|---|---|---|---|
| OTA safety | OTA requires HTTPS URL | PASS | `lib/control/src/portal_ota.c` (`strncmp(url, "https://", 8)`) | P2 |
| OTA safety | TLS trust chain enabled | PASS | `sdkconfig.shared.defaults` (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`) | P2 |
| OTA safety | Dual-bank OTA (`ota_0/ota_1`) with rollback | **FAIL** | `partitions.csv` has `factory` app only (no OTA slots/otadata) | **P0** |
| OTA safety | Runtime rollback decision (`esp_ota_mark_app_valid_cancel_rollback`) | **FAIL** | No rollback-mark path found in `src/`/`lib/` | **P0** |
| OTA safety | OTA endpoint authorization | **FAIL** | `/api/ota` accepts requests with no auth in `lib/control/src/portal_http.c` | **P0** |
| OTA safety | OTA source allowlist / manifest hash pin | **FAIL** | `portal_ota_start()` accepts arbitrary HTTPS URL | **P1** |
| OTA safety | Sensitive OTA metadata redaction | **FAIL** | `lastUrl` returned by OTA JSON and logged (`ESP_LOGI`) | **P1** |
| Uplink credentials | Input validation (presence/length) | PASS | SSID/password bounds in `portal_http.c`, `uplink_control.c` | P2 |
| Uplink credentials | Credentials protected from API exposure | **FAIL** | SSID exposed in `/api/uplink` and `/api/status` (`portal_http.c`, `portal_state.c`) | **P0** |
| Uplink credentials | Credentials persisted securely (encrypted NVS) | PARTIAL | Stored in RAM globals (`mesh_state.c`), not persisted; avoids flash leakage but no durability controls | P1 |
| Uplink credentials | Credentials propagation minimized | **FAIL** | Password propagated over mesh control packet (`mesh_uplink.c`, `uplink_control.c`) | **P0** |
| Portal API hardening | AuthN/AuthZ on control APIs (`/api/ota`, `/api/uplink`, `/ws`) | **FAIL** | No token/session checks in `portal_http.c` | **P0** |
| Portal API hardening | Rate limiting / brute-force throttling | **FAIL** | No per-route throttling in HTTP handlers | P1 |
| Portal API hardening | Local-only exposure boundary | PARTIAL | USB NCM local network + captive DNS (`usb_portal_netif.c`, `usb_portal_dns.c`); still no app-layer auth | P1 |
| Portal API hardening | WebSocket client isolation | **FAIL** | Single global `ws_fd`, no client auth in `portal_http.c` | P1 |
| Observability | Core health/mesh telemetry available | PASS | `/api/status`, WS push, node metrics in `portal_state.c` | P2 |
| Observability | Security event audit logs (who changed OTA/uplink, when) | **FAIL** | No actor/request provenance in control handlers | **P0** |
| Ops readiness | Recovery/rollback runbook | PARTIAL | Scattered notes in README + progress docs; no single operator runbook | **P0** |
| Ops readiness | Release gate checklist (canary/staged rollout/abort criteria) | **FAIL** | No formal checklist doc in `docs/operations/` | **P0** |

---

## Top gaps (practical impact)

1. **No authenticated control plane** for OTA/uplink/WS. Any host on the USB portal network can issue sensitive control actions.
2. **No robust OTA rollback path** (single `factory` app partition). A bad OTA can strand a node without automated fallback.
3. **Credential/operation metadata leakage** (`ssid`, `lastUrl`) in API responses and logs.
4. **Operations process is undocumented** for incident response, staged rollout, and rollback decision points.

---

## Immediate hardening actions (internal pilot)

### P0 — do before pilot
1. **Add portal control auth token**
   - Require `X-SM-Token` (or `Authorization: Bearer`) for `POST /api/ota`, `POST /api/uplink`, and `/ws`.
   - Reject unauthorized with `401`.
   - Keep token in non-committed local config for pilot operators.

2. **Minimize sensitive output**
   - Remove `lastUrl` from OTA status payload (or mask hostname only).
   - Remove SSID from `GET /api/status` and `GET /api/uplink` (or return masked value).
   - Remove URL logging from `portal_ota.c` info log.

3. **Create a single runbook**
   - Include: preflight checks, OTA canary sequence, rollback steps, uplink reset path, known-failure signatures, and escalation owner.

4. **Define pilot rollout policy**
   - Canary 1 node → observe 10 min → expand.
   - Explicit abort criteria (mesh disconnect loops, repeated OTA failure, audio drop threshold).

### P1 — next hardening wave
1. **Migrate to OTA-safe partitioning**
   - Add `otadata`, `ota_0`, `ota_1`, keep `factory` rescue image.
   - Implement boot validity/rollback confirmation (`esp_ota_mark_app_valid_cancel_rollback`).

2. **Add OTA source guardrails**
   - Allowlist firmware hostnames.
   - Require signed manifest with version + SHA256.

3. **Add route throttling**
   - Per-IP/request rate limit for control POST endpoints.
   - Cooldown for repeated OTA attempts.

4. **Add security event logging**
   - Structured events for config/OTA changes (timestamp, endpoint, result, error).

### P2 — quality improvements
1. Expand observability with security counters (failed auth, rejected OTA, rate-limit hits).
2. Add periodic “ops heartbeat” snapshot export for debugging.

---

## Pilot-readiness checklist (operator gate)

Mark each as PASS before deployment:

- [ ] Control API auth token enabled and tested on OTA/uplink/WS.
- [ ] Sensitive fields redacted in API responses and logs.
- [ ] Runbook published and reviewed by operators.
- [ ] Canary rollout + abort criteria documented and rehearsed.
- [ ] At least one known-good firmware/device pair retained offline.
- [ ] Native tests pass (`pio test -e native`).
- [ ] All firmware targets compile (`pio run -e tx && pio run -e rx && pio run -e combo`).
- [ ] Flash-size mismatch warning triaged/understood for deployment hardware consistency.

---

## Validation snapshot (this audit pass)

- `pio test -e native`: **PASS** (84/84)
- `pio run -e tx`: **PASS**
- `pio run -e rx`: **PASS**
- `pio run -e combo`: **PASS**
- Build warning observed: flash size mismatch warning still present during builds.
