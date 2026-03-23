# API Contract Test Design (Portal)

> Status: superseded — contract tests are implemented; see `test/native/test_portal_api_contract/main.c` and `docs/roadmap/implementation-roadmap.md`.

## Scope

This design defines contract tests for:
- `GET /api/status`
- `GET/POST /api/uplink`
- `GET/POST /api/ota`
- `GET /ws` (WebSocket handshake + message payload contract)

The tests are **strict on required fields, types, and invariants** and **permissive on additional fields** (forward-compatible by default).

## Contract Philosophy

1. Required fields must exist and satisfy type/invariant checks.
2. Unknown/additional fields must be ignored.
3. Tests must not require exact object equality or fixed key order.
4. For dynamic values (timestamps, heap, latency, node count), validate shape/ranges instead of constants.

---

## 1) `GET /api/status` contract

### Required top-level fields

| Field | Type | Invariant |
|---|---|---|
| `ts` | number (integer) | `>= 0` (ms epoch from `esp_timer_get_time()/1000`) |
| `self` | string | MAC format `^[0-9A-F]{2}(:[0-9A-F]{2}){5}$` |
| `heapKb` | number (integer) | `>= 0` |
| `core0LoadPct` | number \| null | if number: integer in `[0,100]` |
| `latencyMs` | number (integer) | `>= 0` |
| `netIf` | string | non-empty |
| `buildLabel` | string | one of `SRC`, `OUT`, `UNKNOWN` |
| `meshState` | string | one of `Mesh OK`, `Mesh Degraded` |
| `bpm` | number \| null | current code emits `null`; test allows future numeric |
| `fftBins` | array<number> \| null | if array: each item finite number |
| `nodes` | array<object> | length `<= 32` (`PORTAL_MAX_NODES`) |

### Required per-node fields (`nodes[]`)

| Field | Type | Invariant |
|---|---|---|
| `mac` | string | MAC format uppercase hex with `:` |
| `role` | string | one of `TX`, `RX` |
| `root` | boolean | — |
| `layer` | number (integer) | `>= 0` |
| `rssi` | number (integer) | finite integer |
| `children` | number (integer) | `>= 0` |
| `streaming` | boolean | — |
| `parent` | string \| null | if string: MAC format |
| `uptime` | number (integer) | `>= 0` |
| `stale` | boolean | — |

### Optional top-level fields (if present must be valid)

- `monitor`: array of objects with optional `seq:number`, `line:string`
- `ota`: object with current minimal shape `enabled:boolean`, `mode:string`
- `uplink`: same object contract as `GET /api/uplink`

### Invariants

- `nodes` should not contain duplicate `mac` values.
- If `node.root === true`, `node.layer` should be `0`.
- If `node.parent` is non-null, it must be valid MAC string.

---

## 2) `GET /api/uplink` contract

### Required fields

| Field | Type | Invariant |
|---|---|---|
| `enabled` | boolean | — |
| `configured` | boolean | — |
| `rootApplied` | boolean | — |
| `pendingApply` | boolean | — |
| `ssid` | string | length `<= 32` |
| `lastError` | string | length `<= 47` (buffer 48 incl `\0`) |
| `updatedMs` | number (integer) | `>= 0` |

### Invariants

- If `enabled === false`, `ssid` may be empty.
- `pendingApply` and `rootApplied` may co-exist depending on timing; do not enforce mutual exclusion.

---

## 3) `POST /api/uplink` contract

### Request body rules (current parser behavior)

- Required: `enabled:boolean`
- If `enabled:true`: required non-empty `ssid:string`
- If `enabled:true`: `password:string` optional (defaults to empty string)
- If `enabled:false`: `ssid/password` optional and ignored
- Additional fields allowed

### Response rules

- Success: `200` + `{"ok":true}`
- Client errors:
  - `400` + `{"error":"empty body"}`
  - `400` + `{"error":"missing enabled"}`
  - `400` + `{"error":"missing ssid"}` when enabling without SSID
- Apply failure: `409` + `{"error":"uplink apply failed"}`

### Important parser compatibility note

String extraction is currently pattern-based (`"field":"value"`), so payloads with spaces between `:` and opening quote for string fields are not guaranteed to parse. Contract tests should include a compatibility case documenting this current limitation.

---

## 4) `GET /api/ota` contract

### Required fields

| Field | Type | Invariant |
|---|---|---|
| `enabled` | boolean | currently always `true` |
| `inProgress` | boolean | — |
| `phase` | string | one of `idle`, `queued`, `downloading`, `failed`, `restarting` |
| `lastOk` | boolean | — |
| `lastErr` | number (integer) | any signed int |
| `lastUrl` | string | length `< 192` |

---

## 5) `POST /api/ota` contract

### Request body rules

- Required: `url:string` (non-empty)
- Additional fields allowed
- Validity constraints delegated to OTA start routine:
  - URL must start with `https://`
  - URL length `< 192`
  - OTA must not already be in progress

### Response rules

- Success: `200` + `{"ok":true}`
- Client errors:
  - `400` + `{"error":"empty body"}`
  - `400` + `{"error":"missing url"}`
  - `400` + `{"error":"invalid url"}` (empty string)
- Start failure (invalid scheme/in-progress/no mem/etc.): `409` + `{"error":"ota start failed"}`

---

## 6) `/ws` contract

### Handshake / connection

- Endpoint: `GET /ws`
- WebSocket text frames are pushed ~1 Hz when connected.

### Message payload contract

- Each pushed text frame is JSON object with the **same contract as `GET /api/status`**.

### Invariants

- Message must parse as JSON object.
- Required `/api/status` fields must exist and pass same checks.
- Unknown top-level and nested fields must be ignored.

---

## Actionable test-case structure

## Proposed files

- `test/native/test_portal_api_contract/main.c` (scaffold + shared assertion helpers)
- Later split (optional):
  - `test/native/test_portal_api_contract_status_ws/main.c`
  - `test/native/test_portal_api_contract_uplink/main.c`
  - `test/native/test_portal_api_contract_ota/main.c`

## Planned test IDs

### Status + WS payload
- `status-required-fields-present`
- `status-types-and-ranges`
- `status-node-object-shape`
- `status-allows-additional-top-level-fields`
- `status-allows-additional-node-fields`
- `ws-message-matches-status-contract`

### Uplink
- `uplink-get-required-fields`
- `uplink-post-enable-valid`
- `uplink-post-disable-valid`
- `uplink-post-missing-enabled-400`
- `uplink-post-enabled-missing-ssid-400`
- `uplink-post-extra-fields-ignored`
- `uplink-post-string-spacing-compat-limit-documented`

### OTA
- `ota-get-required-fields`
- `ota-post-valid-https-url`
- `ota-post-missing-url-400`
- `ota-post-empty-url-400`
- `ota-post-invalid-scheme-409`
- `ota-post-extra-fields-ignored`

## Shared assertions to implement

- `assertHasRequiredKey(obj, "key")`
- `assertTypeString/Number/Bool/Object/Array/NullOrNumber(...)`
- `assertMacString(...)`
- `assertIntRange(...)`
- `assertAllowsAdditionalFields(...)` (ensures no failure on unknown keys)

These assertions should be reused across endpoint tests to keep contract validation consistent and maintainable.
