# API Contract

This reference defines the HTTP interface shared by NAMO firmware, the embedded
dashboard, demo and mock implementations, and external integrations. It covers
response envelopes, status and error codes, endpoint behavior, compatibility,
and the boundaries between device state and user-facing text.

It is written for firmware, frontend, integration, and test developers. This is
not a generated OpenAPI description: common response rules appear in Sections
2 through 6, the code registry appears in Section 7, and endpoint-specific
contracts appear in Sections 8 and 9. Examples are illustrative unless a field
or value is explicitly marked as required.

Quick navigation:

- [Common response rules](#2-core-rule)
- [Status and error code registry](#7-status-and-error-code-registry)
- [Client response handling](#8-client-response-handling)
- [Firmware and endpoint requirements](#9-firmware-response-requirements)
- [Compatibility and configuration values](#10-compatibility-and-legacy-migration)
- [LD2410C presence assistance](#15-ld2410c-presence-assistance-contract)

In normative text, **must** and **must not** identify compatibility requirements.
**May** identifies optional behavior.

---

## 1. Scope

This contract applies to APIs that expose device state, accept configuration or
control operations, or report an error to a client.

Covered interfaces include:

- `/api/system/status`
- `/api/platform/descriptor`
- `/api/platform/participants`
- `/api/platform/groups`
- `/api/platform/site-configuration`
- `/api/setup/*`
- Device config upload APIs
- Floorplan upload APIs
- Stats upload and backup/restore APIs
- Calibration-related APIs
- OTA upload and reboot confirmation flow
- Diagnostic/debug APIs

Distributed Spatial Platform behavior that is already implemented is described
in the endpoint-specific sections below. Unimplemented cluster and open
Satellite interfaces remain outside this contract; their design is documented
in the [Spatial Platform Architecture](platform/architecture.md) and
[Satellite Protocol](platform/satellite-protocol.md). A new endpoint enters this
compatibility contract only when its firmware behavior and focused tests are
added together.

`/api/platform/participants` projects registered Site members from the
canonical Site configuration. Runtime endpoint state, local readiness,
credentials, and direct-pair presence are joined onto that projection. A
missing direct-pair record must not hide a canonical member after configuration
Owner handover. `managementAllowed` is true only on the current configuration
Owner; `canonicalMember` and `directPair` describe the two independent sources.

---

## 2. Core Rule

API responses use stable machine-readable codes. The client maps those codes to
the user's display language.

Contract-compliant example:

```json
{
  "ok": false,
  "error": {
    "code": "calibration_pir_active",
    "severity": "warning",
    "detail": {
      "pir": true
    }
  }
}
```

Non-compliant example for a new API:

```json
{
  "ok": false,
  "status": "PIR motion was detected, so calibration cannot start."
}
```

---

## 3. Response Shape

### 3.1 Success Response

Successful responses must use `ok: true`.

Standard shape:

```json
{
  "ok": true,
  "data": {},
  "status": {
    "code": "ready",
    "severity": "info",
    "detail": {}
  }
}
```

Rules:

- `ok` must be `true`.
- `data` contains domain data.
- `status` is optional when there is no useful state to report.
- If present, `status.code` must be stable and machine-readable.
- `status` must not be used for failure causes.

### 3.2 Failure Response

Failure responses must use `ok: false`.

Standard shape:

```json
{
  "ok": false,
  "error": {
    "code": "upload_session_mismatch",
    "severity": "error",
    "detail": {
      "expected": 2,
      "received": 0
    }
  }
}
```

Rules:

- `ok` must be `false`.
- Failure cause must be placed in `error.code`.
- `error.severity` must be `warning` or `error`.
- `error.detail` contains only small, bounded structured data.
- UI logic must not depend on `error.message`.

### 3.3 In-Progress Response

Long-running or deferred operations return `ok: true` with a status code.

Example:

```json
{
  "ok": true,
  "status": {
    "code": "ota_reboot_waiting",
    "severity": "info",
    "detail": {
      "retryAfterMs": 2000
    }
  }
}
```

Use this for accepted operations, reboot waits, deferred actions, and polling
flows.

---

## 4. HTTP Status Code Rules

HTTP status codes describe the request/transport result. JSON `code` values
describe the device/domain state.

Standard mapping:

| HTTP | Meaning | Use for |
|---:|---|---|
| 200 | OK | Normal synchronous success or state response |
| 202 | Accepted | Deferred action accepted, such as OTA/reboot/pending work |
| 400 | Bad Request | Missing parameter, invalid JSON, invalid target, bad chunk header |
| 401 | Unauthorized | API key or PSK is required but missing/invalid |
| 403 | Forbidden | Request is understood but blocked by current device state |
| 404 | Not Found | Unknown endpoint or resource |
| 409 | Conflict | Active session conflict, upload mismatch, operation already running |
| 413 | Payload Too Large | Upload or request body exceeds allowed size |
| 415 | Unsupported Media Type | Unsupported `Content-Type` |
| 500 | Internal Server Error | Unexpected firmware/storage/internal failure |
| 503 | Service Unavailable | Device is temporarily unavailable, rebooting, or changing network state |

Examples:

- Upload session mismatch: HTTP `409`, error code `upload_session_mismatch`.
- Boot guard blocks rebooting actions: HTTP `403`, error code `boot_guard_active`.
- Oversized floorplan image: HTTP `413`, error code `upload_payload_too_large`.
- SPIFFS write failure: HTTP `500`, error code `storage_write_failed`.

---

## 5. Status and Error Object

`code`

- Stable machine-readable identifier used by client logic.
- Lowercase `snake_case` value, not a sentence or translated label.

`severity`

- One of `info`, `warning`, or `error`.
- Used for UI tone, logs, and diagnostics.

`detail`

- Structured data needed to explain the state.
- Keep it small because firmware memory is limited.
- Prefer numbers, booleans, ids, and short enum strings.

`message`

- Optional compatibility field for logs or legacy clients.
- It is not a control field. Client behavior and new UI text are derived from
  `code`.

---

## 6. Code Naming Rules

Codes use a domain prefix when one is available.

Examples:

- `setup_wifi_connecting`
- `setup_wifi_connected`
- `setup_wifi_failed`
- `setup_ready`
- `setup_scan_started`
- `setup_scan_failed`
- `setup_ap_close_scheduled`
- `setup_ap_closing`
- `ha_handoff_started`
- `api_client_waiting`
- `api_client_connected`
- `api_client_idle`
- `calibration_ready`
- `calibration_pir_active`
- `calibration_max_zones_reached`
- `upload_started`
- `upload_committed`
- `upload_session_mismatch`
- `ota_uploading`
- `ota_reboot_waiting`
- `ota_device_reappeared`
- `debug_snapshot_ready`

Codes identify states; they do not contain display text.

Invalid names:

- `pir_motion_detected_cannot_start_calibration`
- `please_wait_for_home_assistant_connection`

Valid names:

- `calibration_pir_active`
- `api_client_waiting`

---

## 7. Status and Error Code Registry

The following codes are part of the compatibility contract. A code is added
only when it is emitted by firmware or a documented mock implementation, or
consumed by the dashboard or an external interface.

### 7.1 Common Error Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `invalid_request` | error | 400 | Request shape or parameter is invalid. | `field`, `reason` |
| `unauthorized` | error | 401 | Required authentication is missing or invalid. | none |
| `forbidden` | error | 403 | Request is not allowed in the current state. | `reason` |
| `not_found` | error | 404 | Endpoint or resource does not exist. | `resource` |
| `conflict` | warning | 409 | Request conflicts with active state. | `reason` |
| `payload_too_large` | error | 413 | Request body exceeds the limit. | `maxBytes`, `receivedBytes` |
| `unsupported_media_type` | error | 415 | Unsupported content type. | `contentType` |
| `internal_error` | error | 500 | Unexpected internal failure. | `where` |
| `service_unavailable` | warning | 503 | Device is temporarily unavailable. | `reason`, `retryAfterMs` |

### 7.1.1 Device Configuration Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `config_not_found` | error | 404 | Saved device configuration does not exist. | `target` |
| `config_write_failed` | error | 500 | Device configuration could not be written. | `target`, `where` |

### 7.2 Setup and Wi-Fi Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `setup_wifi_connecting` | info | 200 | Wi-Fi connection attempt is in progress. | `ssid` |
| `setup_wifi_connected` | info | 200 | Wi-Fi connection succeeded. | `ssid`, `ip` |
| `setup_wifi_failed` | warning | 200 | Wi-Fi connection failed. | `ssid`, `reason` |
| `setup_ready` | info | 200 | Setup mode is ready for user action. | `stage` |
| `setup_scan_started` | info | 200 | Wi-Fi scan has started. | none |
| `setup_scan_failed` | error | 500 | Wi-Fi scan could not be started. | none |
| `setup_finish_pending` | info | 202 | Setup finish was accepted and is being applied. | none |
| `setup_ap_close_scheduled` | info | 200 | Setup AP close was scheduled after a grace period. | `delayMs` |
| `setup_ap_closing` | info | 202 | Setup AP is closing. | `delayMs` |
| `ha_handoff_started` | info | 200 | User-confirmed handoff was accepted and the device will reboot into ESPHome Wi-Fi ownership. | `waitSeconds`, `rebootExpected` |
| `api_client_waiting` | info | 200 | Native API is ready but no client has connected yet. This is not a Wi-Fi failure. | none |
| `api_client_connected` | info | 200 | Native API client is connected. | none |
| `api_client_idle` | info | 200 | Native API has no active client warning. | none |
| `api_service_not_ready` | info | 200 | ESPHome Wi-Fi or the Native API service is not ready yet. | none |
| `api_key_ready` | info | 200 | API key is already customized and ready. | none |
| `api_key_rotated` | info | 200 | Default/demo API key was replaced. | none |
| `api_key_unavailable` | error | 503 | Native API key service is unavailable. | none |
| `api_key_not_set` | error | 404 | Native API key is not set. | none |
| `api_key_random_failed` | error | 500 | API key random generation failed. | none |
| `api_key_save_failed` | error | 500 | API key could not be saved. | none |
| `api_key_unsupported` | error | 503 | API key operation is unsupported in this build. | none |
| `wifi_unavailable` | error | 503 | Wi-Fi service is unavailable. | none |
| `wifi_not_ready` | warning | 409 | Wi-Fi STA configuration is not ready. | none |
| `wifi_unsupported` | error | 503 | Wi-Fi handoff is unsupported in this build. | none |
| `open_wifi_unsupported` | error | 400 | Open Wi-Fi networks are not supported. | none |
| `invalid_password_length` | error | 400 | Wi-Fi password length is outside the WPA range. | `min`, `max` |
| `boot_guard_active` | warning | 403 | Reboot, reset requiring restart, or OTA is blocked during initial boot guard. | `remainingSeconds` |

### 7.2.1 System Management Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `nothing_selected` | warning | 400 | Reset request did not select any reset target. | `target` |
| `space_reset_failed` | error | 500 | Floorplan and placement reset failed. | `target` |
| `detection_reset_failed` | error | 500 | Detection settings reset failed. | `target` |
| `connections_reset_failed` | error | 500 | Connected-device state reset failed. | `target` |
| `stats_reset_failed` | error | 500 | Statistics reset failed. | `target` |

`GET /api/system/api-key` returns the currently active Native API key with
`visibleSeconds: 30`. `POST /api/system/api-key/rotate` generates and persists a
new key through ESPHome's Native API service, returns that key once with
`visibleSeconds: 30`, and lets ESPHome disconnect clients using the old key.
System reset never reveals, deletes, or replaces the API key.

`POST /api/system/reset` accepts five independent booleans:

| Field | Reset scope |
|---|---|
| `space` | Floorplan image, room geometry, furniture, and local radar placement |
| `detection` | User and calibration zones plus environment and general settings |
| `connections` | Participant list, peer credentials, readiness/calibration, automatic replacement, and shared Site state; stable Node identity is retained |
| `stats` | Statistics and heatmap history |
| `wifi` | Saved Wi-Fi credentials |

The legacy `settings` field remains accepted and maps to `space` plus
`detection`. New clients must use the five explicit fields.

### 7.2.2 Control API Codes

Control endpoints use the common `invalid_request` code for invalid
request fields. Keep the legacy error string during migration, and put the
invalid `field` plus the affected `target` in `detail`.

| Endpoint group | Code | Severity | HTTP | Detail |
|---|---|---|---:|---|
| Control settings | `invalid_request` | error | 400 | `field`, `target` |

Timezone control uses the existing bounded control-request pattern.

Endpoints:

- `GET /api/control/status`
- `POST /api/control/timezone`
- `GET /api/control/static-radar-tuning`
- `POST /api/control/static-radar-tuning/session`
- `POST /api/control/static-radar-tuning/gate`

Rules:

- `POST /api/control/timezone` requires form field `data` containing a JSON
  object with a supported IANA `timezone` identifier.
- A successful change returns HTTP `200` with `changed: true` and
  `todayStatsReset: true`. Requesting the active timezone also returns HTTP
  `200`, with `changed: false`, and does not reset statistics. The change is
  applied asynchronously and its pending state is exposed by the status API.
- Applying a different timezone resets only the current-day counters and
  current-day heatmap. Completed daily history must be preserved.
- `GET /api/control/status` exposes `timezone`, `timezoneKnown`, and
  `timezoneApplyPending` so clients can wait for the asynchronous device-side
  apply operation.
- Unsupported timezone identifiers return the standard `invalid_request`
  envelope with `field: timezone` and `target: timezone`.

LD2410C tuning uses a short-lived engineering session so per-gate telemetry
does not run during normal operation.

- `GET /api/control/static-radar-tuning` always returns `available`,
  `recovering`, `active`, `phase`, and `result`. `phase` is one of `idle`, `starting`, `active`,
  `stopping`, or `failed`; `active` becomes true only after a real engineering
  periodic frame is received. While inactive, `gates` is empty. While active, `gates` contains
  G0 through G8 with `startMm`, `endMm`, `sensitivity`, `moveEnergy`,
  and `stillEnergy`. Gate energy is tuning telemetry and must not be treated
  as a detected target position. The response also contains the latest gate
  `operation` with `id`, `state`, `result`, requested gate and sensitivity,
  and the move/still thresholds read back from the module.
- `POST /api/control/static-radar-tuning/session` requires boolean `active`.
  An active request is also a keepalive. The firmware must leave engineering
  mode automatically when no active keepalive arrives for 15 seconds.
- Starting a session when LD2410C is unavailable returns HTTP `409` with code
  `static_radar_unavailable`.
- Session and gate requests received during command recovery return HTTP `409`
  with `static_radar_recovering`; they are never queued for later replay.
- `POST /api/control/static-radar-tuning/gate` requires integer `gate` in
  `[0, 8]` and integer `sensitivity` in `[0, 100]`. The product-level
  sensitivity is translated to both LD2410C move and still thresholds as
  `threshold = 100 - sensitivity`. An accepted request returns an
  `operationId`; acceptance does not mean that the module has applied it.
- Only one gate operation may be in flight. A concurrent request returns HTTP
  `409` with code `static_radar_tuning_busy` instead of replacing the pending
  request.
- After writing, firmware clears the local values and queries the LD2410C.
  The operation becomes `applied` only when both returned thresholds match the
  requested threshold. A different value produces `failed` with
  `readback_mismatch`; no response produces `readback_timeout`.
- Gate writes are accepted only while the engineering session is active.
  Otherwise the endpoint returns HTTP `409` with code
  `static_radar_tuning_inactive`.
- `POST /api/control/static-radar-tuning/factory-reset` restores the LD2410C
  module's complete factory configuration with its native reset command. It
  cancels the active engineering session and any pending gate write before the
  module restarts. The endpoint returns `restartExpected: true`.
- Factory reset is rejected with `static_radar_unavailable` when no LD2410C is
  available, and with `static_radar_factory_reset_pending` while an accepted
  reset has not yet been consumed by the device control loop.
- Startup metadata synchronization, engineering-session changes, gate writes,
  and factory reset are serialized. Each command waits for its matching ACK,
  and mode changes complete only after the expected periodic frame resumes.
  A bounded recent-ACK history preserves response order; ACKs for other
  commands are ignored instead of failing the active transaction.
- An active engineering-session request takes priority over startup metadata
  synchronization. Any command-protocol failure cancels queued tuning and
  gate requests and reports `sensor_recovering`; it must not retry a stale UI
  request after recovery.
- The vendored LD2410 component is the canonical ACK parser and command writer.
  Raw UART diagnostics must not create a second ACK event or command owner.
- Stream watchdog recovery is a bounded fallback and must not overlap an
  active command transaction. Recovery completes only after configuration
  entry, module restart, at least ten new periodic frames, and a 1.5 second
  settling interval. Startup synchronization and tuning remain blocked during
  that interval.
- Engineering telemetry is diagnostic only. It must not change presence,
  motion, target coordinates, target counts, zones, rooms, or heatmaps.

### 7.3 Upload Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `upload_started` | info | 200 | Upload session has started. | `target`, `session`, `size` |
| `upload_committed` | info | 200 | Upload was committed successfully. | `target`, `session`, `size`, `hash` |
| `upload_session_mismatch` | error | 409 | Chunk does not match the active upload session. | `expected`, `received` |
| `upload_offset_mismatch` | error | 409 | Chunk offset does not match expected offset. | `expected`, `received` |
| `upload_incomplete` | error | 409 | Commit was requested before all bytes were received. | `expected`, `received` |
| `upload_payload_too_large` | error | 413 | Upload payload exceeds limit. | `maxBytes`, `receivedBytes` |
| `upload_storage_failed` | error | 500 | Upload could not be written or committed. | `target`, `where` |
| `firmware_upload_failed` | error | 500 | Firmware upload endpoint rejected or failed the upload. | optional legacy response text |
| `firmware_network_error` | error | 0 | Dashboard client lost the network while uploading firmware. | none |
| `firmware_upload_aborted` | warning | 0 | Dashboard client aborted the firmware upload. | none |

### 7.3.1 Device Config Upload

Device configuration writes use the same chunked upload pattern as
floorplan and stats data because zone, floorplan, and future spatial-platform
settings can exceed safe single-form request sizes.

Endpoints:

- `POST /api/config/upload/start`
- `POST /api/config/upload/chunk`
- `POST /api/config/upload/commit`

Rules:

- `start` requires `session` and `size`.
- `chunk` requires `session`, `offset`, and hex-encoded `data`.
- `commit` requires `session`.
- `commit` must reload the stored device config into the firmware cache before
  returning success.
- Single-form legacy writes such as `POST /api/config`, `POST /api/stats`,
  `POST /api/floorplan`, and `POST /api/floorplan/image` are removed from the
  embedded API. Dashboard and restore flows must use the chunked upload
  endpoints.

`GET /api/floorplan/status` is the authoritative read-only floorplan storage
status. In addition to payload presence and byte counts, it returns
`configHash` and `imageHash`. Clients use these hashes to notice an atomic local
save or authenticated Site replication without treating Node-local device
configuration, radar pose, or zones as shared state.

`POST /api/floorplan/delete` requires form field `confirm=1`. It is allowed
only on the configuration owner when no non-revoked remote Site member
remains. A connected Site returns HTTP `409` with
`floorplan_delete_requires_standalone`; its floorplan may be replaced but not
deleted. A follower returns `site_configuration_owner_required`. A successful
standalone deletion removes the local CONFIG and IMAGE payloads and publishes
an uncommitted floorplan manifest. Node-local radar placement, zones, and
tuning remain local.

All full-document uploads and shared patch writes require the configuration
owner and a writable shared-configuration continuity state. The dashboard
starts replacement without deleting the active payloads first; the previous
floorplan remains authoritative until the new payloads and manifest commit.

### 7.3.2 Floorplan Patch Writes

Small floorplan edits may use dedicated single-form patch endpoints instead of
re-uploading the full floorplan document. These endpoints are intentionally
limited to bounded, low-memory fields and must not be used for image uploads,
room geometry replacement, wall segment replacement, object list replacement, or
full restore flows.

Endpoints:

- `POST /api/floorplan/radar`
- `POST /api/floorplan/room-name`
- `POST /api/floorplan/occlusion`
- `POST /api/floorplan/objects`

Rules:

- `POST /api/floorplan/radar` requires form field `data`, a JSON object with
  `originPx`, `rotationDeg`, `scale`, and the current `poseVersion` when the
  stored document uses floorplan schema version 2.
- `POST /api/floorplan/radar` remains a compatibility endpoint for older
  dashboard builds. The current dashboard stores routine radar-placement edits
  in `deviceConfig.floorplan.radar` so the Node-local pose does not rewrite the
  shared floorplan document.
- `POST /api/floorplan/room-name` requires form fields `id` and `name`.
- `POST /api/floorplan/occlusion` requires form field `data`, a JSON object
  with `ignoredEdges`.
- `POST /api/floorplan/objects` requires form field `data`, a JSON array of
  floorplan furniture objects.
- The device updates the stored floorplan config payload in place and keeps the
  raw-partition metadata authoritative.
- If a patch target is missing, invalid, or too large, the device returns the
  standard error envelope with `invalid_request`, `not_found`, or
  `upload_storage_failed`.
- Full floorplan document writes, initial saves, restore flows, room geometry
  edits, scale edits, wall segment edits, and image writes must continue to use
  `POST /api/floorplan/upload/start|chunk|commit`.

### 7.3.3 Floorplan Identity and Revisions

Floorplan document schema version 2 adds persistent identity and independent
geometry and Node-pose revisions:

```json
{
  "version": 2,
  "floorplanId": "fp-00112233445566778899aabbccddeeff",
  "geometryVersion": 7,
  "source": "manual",
  "outline": {
    "pointsPx": [[120, 120], [760, 120], [760, 560], [120, 560]]
  },
  "scale": {
    "mmPerPxX": 10.16,
    "mmPerPxY": 9.15
  },
  "radar": {
    "originPx": [120, 240],
    "rotationDeg": 15,
    "scale": 1,
    "poseVersion": 11
  }
}
```

Rules:

- `floorplanId` is a random persistent identity. It is not derived from image
  bytes, room IDs, geometry, or a hash of the document.
- `source` is an optional additive field. `image` identifies an uploaded
  floorplan and `manual` identifies a drawing made in the dashboard. A manual
  document stores its exact outer polygon in `outline.pointsPx`; `outerBoundsPx`
  remains the bounding coordinate frame used by existing transforms.
- The dashboard renders a manual drawing to WebP as a compatibility snapshot
  and commits the same CONFIG + IMAGE pair used by uploaded floorplans. Current
  structured manual documents render their rooms, physical walls, and doors
  from CONFIG in the creation editor, stored editor, and dashboard instead of
  displaying those same elements a second time from the WebP. No upload,
  replication, or storage endpoint changes. The first save freezes the outer outline and the
  physical wall geometry in `wallSegments`; correcting those physical walls
  requires deleting the floorplan and creating it again. `rooms[].pointsPx`
  remains the editable logical room-area geometry, independent of the frozen
  physical walls. Manual doors are stored in the optional `doors` array with
  `id`, `kind`, `centerPx`, `widthPx`, and `rotationDeg`; the creation editor,
  stored-floorplan editor, and dashboard render that same geometry with the
  same door overlay. Stored door edits remain snapped to frozen
  `wallSegments`; moving, resizing, adding, or removing a door updates only the
  exit zone previously generated for that door and preserves unrelated zones.
- Manual creation keeps one fixed 3000 by 2000 coordinate canvas. The existing
  zoom controls select a work scale in 25 percent steps before the outer
  outline is confirmed. The default 100 percent covers 30 m by 20 m, 25 percent
  covers 120 m by 80 m, and zooming above 100 percent uses the same background
  rescaling path. The selected physical scale is then locked. Geometry snaps to
  a 0.5 m grid, while edge lengths are shown to 0.1 m; users do not enter a
  separate image scale.
- The outer boundary and room polygons are separate inputs. The boundary must
  be a simple polygon, every room must remain inside it, and at least one room
  is required before radar placement. During manual creation, confirmed room
  edges become the initial physical wall set as well as the initial logical
  room areas. Cancelling an unfinished polygon leaves the creation session
  available so the user can start that polygon again.
- Before the first save, the outer boundary and room vertices remain editable.
  The resulting WebP, `outline.pointsPx`, and `wallSegments` use the same
  coordinate frame, and scale calculations use the explicit outer boundary
  rather than the union of room bounds. The WebP contains only the frozen
  physical walls and doors; logical room fills and labels remain dashboard
  overlays.
- Radar occlusion is calculated from physical `wallSegments`. Outer-outline
  segments are blocking walls just like internal walls: each visibility ray
  stops at its nearest physical wall, and the remaining radar coverage beyond
  that wall is shaded. The shading is not clipped back to `outline.pointsPx`.
  Logical room-area edits do not change occlusion. `occlusion.ignoredEdges` is an advanced per-wall
  exception list. Scale `outerBoundsPx` is never treated as a physical
  occlusion outline. Legacy manual documents without `wallSegments` hydrate
  their initial physical walls once from their stored outline and room
  geometry. Legacy documents without structured `doors` keep the door pixels
  already baked into their WebP and do not receive a covering wall overlay.
- `geometryVersion` is the Floorplan coordinate-frame revision. Scale changes
  advance it without changing `floorplanId` because they change the mapping
  between Floorplan pixels and physical coordinates.
- Room geometry, wall, outer-boundary, furniture, and room-name edits update
  the Floorplan CONFIG hash without advancing `geometryVersion`. These edits
  do not change an already approved radar-to-Floorplan transform and therefore
  must not invalidate spatial calibration.
- A local radar placement edit advances only that Node's `poseVersion`.
- The current Node's effective placement and `poseVersion` are persisted in
  `deviceConfig.floorplan.radar`. On load, that local value overrides the
  placement embedded in the shared floorplan document.
- Current device configuration writes also persist
  `deviceConfig.floorplan.scaleMmPerPxX` and `scaleMmPerPxY`. The legacy
  `scaleMmPerPx` average remains present for backward compatibility, but
  spatial transforms use the independent X and Y values.
- A direct firmware upgrade may encounter a complete local radar placement
  whose device configuration predates all scale fields. In that case firmware
  hydrates only the missing X/Y scale from the locally stored canonical
  floorplan document. It does not replace the local radar origin, rotation,
  pose revision, calibration, zones, or tuning. The compatibility hydration is
  deterministic on every boot and does not require a browser migration or an
  extra flash rewrite. A later normal device-configuration save persists the
  current axis fields.
- If neither a valid sidecar scale nor a valid stored canonical floorplan
  scale exists, the spatial profile remains unavailable and fusion fails
  closed while standalone sensing continues.
- A legacy device configuration that has a complete local radar placement but
  no `poseVersion` is treated as pose revision `1`. The dashboard persists that
  migration on the next device-configuration save.
- Revisions are positive unsigned 32-bit values and stop at `4294967295`.
- Legacy documents receive a new identity and initial revisions during load;
  the next save must persist the migration with a full document upload.
- Equal local documents do not establish shared Site ownership and must not be
  used by the dashboard to activate paired participants. A participant remains
  quarantined until an authoritative Site floorplan, Space assignment, and
  per-Node pose are committed through the platform configuration flow.

### 7.4 Calibration Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `calibration_ready` | info | 200 | Calibration can start. | none |
| `calibration_started` | info | 200 | Calibration run started. | none |
| `calibration_running` | info | 200 | Calibration is running. | `samples`, `zone` |
| `calibration_pir_active` | warning | 409 | PIR motion prevents calibration start. | `pir` |
| `calibration_no_target` | warning | 409 | Calibration cannot start because no target is detected. | none |
| `calibration_multiple_targets` | warning | 409 | Calibration cannot start because multiple targets are detected. | `targetCount` |
| `calibration_max_zones_reached` | warning | 409 | No more calibration zones can be created. | `maxZones` |
| `calibration_timeout` | error | 409 | Calibration ended without a stable candidate. | `samples`, `score` |
| `calibration_candidate_failed` | error | 500 | Calibration could not create a correction zone candidate. | `samples`, `score` |
| `calibration_saved` | info | 200 | Calibration zone was saved. | `zone`, `count` |

### 7.5 OTA Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `ota_uploading` | info | 200 | OTA upload is in progress. | `bytesWritten`, `totalBytes` |
| `ota_reboot_waiting` | info | 200 | OTA upload completed and device is expected to reboot. | `retryAfterMs` |
| `ota_device_reappeared` | info | 200 | Device responded after OTA/reboot. | `version`, `buildTime` |
| `ota_verify_timeout` | warning | 503 | Device did not respond before timeout. | `timeoutMs` |
| `firmware_trust_unavailable` | error | 503 | Signed OTA trust or its PSRAM workspace is unavailable. Raw OTA fallback is forbidden. | none |
| `firmware_header_invalid` | error | 400 | The preflight header is absent, malformed, or not exactly 320 bytes. | none |
| `alpha_firmware_not_allowed` | warning | 403 | A valid Developer-signed alpha package was presented without a current one-shot alpha installation grant. | none |
| `alpha_firmware_permission_invalid` | warning | 400 | The alpha installation permission request did not contain a strict `enabled` boolean. | none |
| `header_mismatch` | error | 400 | The uploaded envelope differs from the envelope accepted during preflight. | none |
| `trust_rejected` | error | 400 | Signature, signer role, key status, trust revision, or security epoch was rejected. | `trust` |
| `session_mismatch` | warning | 409 | The upload or cancel token does not own the active OTA session. | none |
| `offset_mismatch` | error | 400 | Multipart chunks are missing, repeated, or out of order. | none |
| `size_mismatch` | error | 400 | Uploaded bytes do not match the exact signed package size. | none |
| `expired` | error | 400 | The signed upload session exceeded its inactivity timeout. | none |
| `sink_begin_failed` | error | 400 | The OTA partition could not be opened. | none |
| `sink_write_failed` | error | 400 | The OTA partition write failed. | none |
| `sink_finish_failed` | error | 400 | Image validation, flashed digest verification, or trust commit failed. | none |
| `signed_firmware_required` | warning | 400 | A signed device received a file other than `.namo-ota`. | none |
| `legacy_firmware_bin_required` | warning | 400 | A legacy development firmware received a file other than `.bin`. | none |

#### 7.5.1 Signed OTA Contract

`GET /api/firmware/trust` reports signed OTA capability and the current durable
trust generation:

```json
{
  "ok": true,
  "data": {
    "mode": "signed",
    "ready": true,
    "trustRevision": 1,
    "securityEpoch": 1,
    "generation": 1,
    "releaseKeyId": "32 lowercase hex characters",
    "developerKeyId": "32 lowercase hex characters",
    "sessionActive": false,
    "alphaInstallSupported": true,
    "alphaInstallAllowed": false,
    "alphaInstallRemainingSeconds": 0
  }
}
```

`POST /api/firmware/alpha-permission` changes the volatile alpha installation
grant. The form field `enabled` must be exactly `true`, `false`, `1`, or `0`.
Enabling it permits one valid Developer-signed package for at most 10 minutes.
The grant is held only in RAM, is cleared by reboot, and is consumed only after
the Developer-signed image has passed flash digest verification and activation.
Release and Root Recovery packages do not consult or consume this grant.

The dashboard sends the 320-byte envelope as 640 lowercase hexadecimal
characters in `POST /api/firmware/update/start` form field `header`. A successful
response returns a nonzero `session`, exact `imageSize`, exact `packageSize`, and
`signerRole`. The dashboard then uploads the complete package as multipart form
data to `POST /api/firmware/update?session=<uint32>`. Cancellation uses
`POST /api/firmware/update/cancel?session=<uint32>`.

Firmware verifies the package signature and signer role before applying the
alpha installation grant. A valid Developer package without a current grant is
rejected before the OTA partition is opened. Invalid or unknown signatures are
always rejected as trust failures and cannot be converted into an alpha prompt.

The firmware must verify trust and signature before opening the OTA partition,
compare the uploaded envelope byte-for-byte with preflight, write only the image,
hash the written partition, and activate it only after the signed digest matches.
When a package advances anti-rollback state or rotates a Release or Developer
key, firmware first writes the next Trust Store state to the inactive A/B slot
without publishing it. It commits that staged state only after OTA image
activation succeeds. If staging fails, image activation is aborted. If commit
fails after activation, firmware selects the currently running partition again
and reports failure. A power loss between activation and commit may boot the
new image with the previous committed trust, but never publishes an
uncommitted key revocation. The Root key remains the sole offline authority for
remote Release or Developer key recovery; Root replacement requires controlled
USB recovery.

Multipart firmware bytes must only be consumed by the exact
`POST /api/firmware/update` route. Upload callbacks for every other route are
ignored and must not change the active signed OTA session.

On ESP-IDF, the multipart transport emits a zero-length start marker before
the first file byte and a zero-length end marker after the last file byte. The
start marker is transport metadata and must not be passed to the signed byte
consumer. The end marker completes the session only when its index equals the
exact number of accepted package bytes. The first terminal upload failure is
retained for the HTTP response; later callbacks from the same multipart body
are drained without replacing the failure or repeating error logs.

After final validation, firmware sends HTTP `200` with `Connection: close` and
only then schedules reboot. The dashboard treats an explicit HTTP error response
as a completed upload failure. Browser upload progress is display-only and must
not decide completion, cancellation, or failure. A network error or browser
abort without an HTTP response enters reboot verification regardless of the
last reported progress. The dashboard never cancels such an ambiguous upload;
the cancel endpoint is reserved for an explicit user cancellation. Error state
remains in session storage until the user clears it or starts another update.

Signed release firmware must not expose ESPHome Native OTA or raw ESPHome web
OTA. The dashboard may use legacy `/update` only when the trust endpoint returns
HTTP 404. Any other trust endpoint failure is fail-closed and must never trigger
raw OTA fallback.

### 7.6 Debug Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `debug_snapshot_ready` | info | 200 | Debug snapshot is available. | `timestamp` |
| `debug_events_ready` | info | 200 | Diagnostic event log is available. | `count`, `truncated`, `format` |
| `debug_events_empty` | info | 200 | Diagnostic event log is empty. | `format` |
| `debug_events_unavailable` | warning | 503 | Diagnostic event log is temporarily unavailable. | `resource` |
| `debug_replay_ready` | info | 200 | Presence replay log is available. | `count`, `truncated`, `format` |
| `debug_replay_empty` | info | 200 | Presence replay log is empty. | `format` |
| `debug_replay_unavailable` | warning | 503 | Presence replay log is temporarily unavailable. | `resource` |

---

## 8. Client Response Handling

The dashboard and external clients use `status.code` or `error.code` as the
control value and map it to localized text.

Example:

```ts
const code = response.error?.code ?? response.status?.code ?? "unknown";
const text = messages.status[code] ?? messages.status.unknown;
```

Compatibility fallback order:

1. Use `code` if present.
2. If `code` is missing, use the existing legacy field.
3. If no known field is available, show a generic message.

Clients must not parse Korean or English sentences to decide behavior.

---

## 9. Firmware Response Requirements

Firmware returns compact status codes and bounded detail objects. User-facing
sentences appear in API responses only when a legacy client requires temporary
compatibility.

Normal endpoints do not carry large diagnostic payloads. Rich diagnostics use
a dedicated debug endpoint.

Debug endpoints that can return many records stream or chunk their responses
instead of building one large string in firmware memory.

Shadow diagnostics may include experimental computed fields such as tracker
state, track score, observation counters, and recommended presence. These fields
must not replace the stable device state until the firmware behavior has been
verified in real installations.

`GET /api/state` may expose bounded LD2410C evidence under
`debug.staticRadar`. Its `available`, `presence`, `moving`, `still`, distance,
energy, and short `reason` fields describe sensor input. `detectionDistanceMm`
is the canonical LD2410C target distance stabilized with a three-sample median.
At most one distance sample per second enters the median window. Brief invalid
distance samples do not replace the last stable value; the history resets when
static-radar presence clears or the sensor becomes unavailable.
`movingDistanceMm` and
`stillDistanceMm` are diagnostic raw values and must not be combined by clients
to infer a target position. The nested `assist`
object reports whether presence assistance is armed, active, arming, or vetoed
by confirmed exit evidence. `debug.presenceEvidence` reports the independent
PIR, tracker, and static-assist evidence used by fusion.

Production-fusion builds may also expose a compact top-level `fusion` object:

```json
{
  "fusion": {
    "buildEnabled": true,
    "authoritative": true,
    "state": "active"
  },
  "targets": [
    {
      "id": "target_1",
      "name": "T1",
      "active": true,
      "fused": true,
      "sourceCount": 2
    }
  ]
}
```

`fusion` is bounded runtime metadata, not a second target stream. Coordinates
in `targets` remain the canonical display coordinates. When
`authoritative` is true, those coordinates already come from the production
fusion adapter. A target is marked `fused: true` only when at least two sensor
observations corroborate that output. Older firmware may omit all of these
additive fields.

LD2410C assistance may maintain an already established presence session, but it
must not start a new session by itself. PIR and confirmed LD2450 tracker
evidence remain the only acquisition inputs. LD2410C assistance must not create
motion, target coordinates, target counts, room state, or zone presence. A
confirmed `lost_after_exit` or `lost_after_room_exit` event must prevent a new
static hold for that drop event. Missing LD2410C hardware must behave as
`available: false` and preserve PIR plus LD2450 behavior.

Replay diagnostics may expose newline-delimited samples through a dedicated
debug endpoint. Replay payloads are for offline testing and must not be embedded
inside normal status responses.

Replay diagnostics may append compact optional tuples over time. Consumers must
ignore unknown fields and must provide safe defaults for missing optional fields.
For example, the `ex` tuple reports exit-zone evidence as
`[exitActive, exitZoneMask, exitTargetCount, exitLastSeenAgeMs]`.
The optional `sf` tuple reports fusion evidence and state as
`[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending,
exitVeto, armElapsedMs]`.

### Runtime Connectivity Status

`GET /api/system/status` reports ESPHome runtime ownership separately from
Native API client registration:

- `wifi.connected` is true only when the ESPHome Wi-Fi component owns a
  connected STA. A temporary ESP-IDF provisioning connection does not satisfy
  this field.
- `api.enabled` reports whether the Native API component exists.
- `api.keyConfigured` reports whether a Noise PSK is stored and active.
- `api.serviceReady` requires ESPHome Wi-Fi, the Native API component, and the
  API key to be ready.
- `api.clientConnected` reports whether any Native API client is currently
  connected. `false` is a normal registration/waiting state and must not be
  presented as a Wi-Fi failure.
- `api.connected` remains an alias of `api.clientConnected` for compatibility.
- `api.warning` is transitional ESPHome component state. New UI logic must not
  use it to infer Wi-Fi failure or Home Assistant registration failure.
- After an explicit setup handoff, the dashboard waits only for
  `wifi.connected`. Native API client registration is not a setup-completion
  gate and must not reopen the setup dialog.

### 9.1 Read-Only Platform Status

`GET /api/system/status` may include the following optional `platform` object:

```json
{
  "platform": {
    "schemaVersion": 1,
    "identityReady": true,
    "siteId": "site-00112233445566778899aabbccddeeff",
    "nodeId": "namo-node-246f2801a2b3",
    "mode": "standalone",
    "maxParticipants": 20,
    "registryReady": true,
    "remoteParticipantCount": 0,
    "connectedRemoteParticipantCount": 0,
    "activeRemoteParticipantCount": 0,
    "readinessReady": true,
    "configuredRemoteParticipantCount": 0,
    "fusionEnabled": true,
    "pairingSupported": true,
    "pairingEnabled": false
  }
}
```

The same response may include a boot guard snapshot:

```json
{
  "boot": {
    "initialGuardActive": true,
    "guardSeconds": 60,
    "remainingSeconds": 37
  }
}
```

`remainingSeconds` is calculated by firmware at response time and is the
authoritative countdown for reboot, reset, and OTA controls. Dashboards may
animate that snapshot locally, but must refresh it when the page regains
visibility and must still handle `boot_guard_active` from write endpoints.

Rules:

- The object is optional so dashboards remain compatible with older firmware.
- `siteId` and `nodeId` are present only when `identityReady` is `true`.
- `mode` is `"standalone"` before Site enrollment and `"enrolled"` after a
  joiner has transactionally adopted the leader Site. Neither value means
  fusion is enabled. Rejoining the same `siteId` must still persist
  `"enrolled"` when the previous local mode was `"standalone"`. On boot, that
  stale mode may be repaired only from a committed canonical roster that names
  another configuration owner and contains the local Node as an active member.
- `maxParticipants` is a protocol capacity limit and does not mean fusion is enabled.
- `registryReady` reports only whether the bounded remote-participant registry
  loaded safely. Registry failure disables remote participation but must not
  disable standalone sensing.
- Participant counts describe remote entries only; the local Node is not included.
- `remoteParticipantCount` includes retained revoked records,
  `connectedRemoteParticipantCount` counts quarantined or active relationships,
  and `activeRemoteParticipantCount` counts only activated relationships.
- Counts are `0` whenever `registryReady` is `false`.
- `readinessReady` reports whether the separate capability/assignment readiness
  store loaded safely. Its configured count is `0` whenever it is unavailable.
- `pairingSupported` reports firmware capability. `pairingEnabled` is `true`
  only while the bounded local enrollment window is open.
- `fusionEnabled` reports whether authenticated production fusion is compiled
  into this firmware. It does not grant runtime authority; authoritative state
  is reported by `GET /api/platform/groups` only after every Site, Group,
  readiness, credential, and spatial gate passes.
- Private keys, certificate material, preference records, and initialization
  markers must never be returned.

#### 9.1.1 Device Resource Metrics

`GET /api/system/status` may include bounded resource measurements in its
existing `memory` and `flash` objects:

```json
{
  "memory": {
    "internalFreeBytes": 76328,
    "internalMinFreeBytes": 47124,
    "internalLargestFreeBlockBytes": 32768,
    "httpdStackMinFreeBytes": 6144,
    "httpdStackLowestEndpoint": "/api/platform/groups",
    "httpdStackLowestPhase": "handler",
    "httpdStackLowestEndpointFreeBytes": 6144,
    "internalMinFreeSource": "observation_fusion",
    "internalMinFreePhase": "cycle",
    "internalMinFreeObservedAtMs": 182400,
    "internalMinFreeEventCount": 7,
    "internalMinLargestFreeBlockBytes": 40960,
    "internalThresholdSource": "observation_send",
    "internalThresholdPhase": "socket",
    "internalThresholdFreeBytes": 48672,
    "internalThresholdObservedAtMs": 76190,
    "psramTotal": 8388608,
    "psramFree": 8018604
  },
  "flash": {
    "firmwareUsedBytes": 1887168,
    "firmwareSlotBytes": 3145728,
    "storageUsedBytes": 41781,
    "storageTotalBytes": 2031616
  }
}
```

The response may also include fixed state-publication counters:

```json
{
  "statePublisher": {
    "attempted": 1200,
    "published": 1199,
    "skippedBusyBuffer": 1,
    "failedCommit": 0
  }
}
```

`skippedBusyBuffer` counts update cycles that retained the previous complete
snapshot because an HTTP reader still owned the inactive buffer. It is not a
partial-state publication or a sensor input loss.

Rules:

- All fields are non-negative byte counts and remain optional for compatibility
  with older firmware.
- `internalLargestFreeBlockBytes` is the largest currently allocatable internal
  8-bit heap block. It complements total free memory by exposing fragmentation.
- `httpdStackMinFreeBytes` is the HTTP server task's lifetime low-water mark,
  measured while serving this request. It is not the total task stack size.
- `httpdStackLowestEndpoint` is a bounded route code identifying the request
  during which firmware first observed the recorded HTTP task low-water mark.
  `httpdStackLowestPhase` is `entry` when the minimum was already visible after
  framework dispatch and URL decoding; in that case an earlier non-Radar HTTP
  handler may have established the lifetime minimum. `handler` means the
  selected Radar handler lowered it further.
  `httpdStackLowestEndpointFreeBytes` is the corresponding
  free-stack byte count. A status request may observe a newer raw lifetime
  minimum while it is still being serialized; the attribution is finalized by
  the next status request.
- Endpoint attribution is diagnostic only. It must use fixed codes and fixed
  storage, must classify every Radar API, setup, dashboard, and dashboard-asset
  route, and must not retain request URLs or allocate per-request strings.
- `internalMinFreeSource`, `internalMinFreePhase`,
  `internalMinFreeObservedAtMs`, `internalMinFreeEventCount`, and
  `internalMinLargestFreeBlockBytes` attribute the most recently observed new
  internal-heap lifetime minimum to a bounded firmware operation boundary.
  Attribution is best-effort because other tasks may run concurrently. Sources
  and phases are fixed literals; the diagnostic record is fixed-size and lives
  in the PSRAM platform workspace. Firmware logs only the first crossing of the
  64, 56, 48, and 40 KiB thresholds.
- `internalThresholdSource`, `internalThresholdPhase`,
  `internalThresholdFreeBytes`, and `internalThresholdObservedAtMs` capture the
  first operation boundary that observes the lifetime minimum crossing below
  the 48 KiB production gate. Fine-grained transport checkpoints stop after
  this record is captured. These fields are optional, additive diagnostics;
  they use fixed string literals and fixed storage in the PSRAM platform
  workspace.
- The status response is serialized through a bounded reusable workspace and
  chunked HTTP output. Immutable flash and running-image metadata is captured
  before the HTTP handler is registered; a status request must not parse the
  running image or accumulate the complete JSON document in internal RAM.
- If neither PSRAM nor the bounded internal fallback workspace can be
  allocated, the endpoint returns HTTP `503` with
  `code: "system_status_unavailable"` and
  `detail.resource: "status_workspace"`.
- Clients must not infer a crash from one frontend refresh. Automated resource
  smoke tests evaluate explicit margins across several bounded samples.
- Resource status remains read-only and must not allocate a large serialization
  buffer on the HTTP server stack.

### 9.2 Read-Only Node Descriptor and Discovery

`GET /api/platform/descriptor` returns the local NAMO Node's public protocol
identity and capabilities:

```json
{
  "protocolVersion": 1,
  "participantId": "namo-node-246f2801a2b3",
  "participantType": "namo_node",
  "product": "namo-spatial-sensor",
  "softwareVersion": "0.5.5-d",
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": true,
    "range": true,
    "presence": true,
    "motion": true,
    "zonePresence": false,
    "bridge": false,
    "maxTargets": 3
  },
  "protocolRange": {
    "minimum": 1,
    "maximum": 1
  }
}
```

Rules:

- The descriptor is read-only and does not enroll, approve, or trust a peer.
- `participantId` is the stable Node identity. `siteId` is intentionally absent.
- `zonePresence` remains `false` until the platform Observation wire path exports
  general zone-presence evidence.
- Attestation, private keys, certificates, network addresses, and mutable Site
  membership must not be returned.
- If stable identity is unavailable, the endpoint returns HTTP `503` with code
  `platform_identity_unavailable`; it must not invent a temporary identity.
- The device advertises `_namo._tcp` on port 80. TXT records contain only
  `protocol_min`, `protocol_max`, `participant_id`, `participant_type`, `product`,
  `descriptor_path`, `pairing`, and `identity_ready`.
- `pairing=manual` is static capability metadata. The participants endpoint is
  authoritative for the current open/closed enrollment-window state.
- Clients must support manual access through
  `http://<device-ip>/api/platform/descriptor` when multicast discovery is not
  available.

### 9.3 Local Participant Registry and Enrollment Window

`GET /api/platform/participants` returns the bounded canonical/direct
participant projection and enrollment-window state:

```json
{
  "ok": true,
  "registry": {
    "ready": true,
    "generation": 1,
    "remoteCount": 0,
    "activeRemoteCount": 0,
    "maxRemoteParticipants": 19,
    "maxParticipants": 20
  },
  "pairing": {
    "supported": true,
    "enabled": false,
    "remainingMs": 0,
    "maxDurationMs": 300000
  },
  "siteReconciliation": {
    "state": "complete",
    "result": "ok"
  },
  "readiness": {
    "ready": true,
    "generation": 1,
    "configuredCount": 0
  },
  "credentials": {
    "ready": true,
    "generation": 1,
    "provisionedCount": 0
  },
  "shadowTransport": {
    "supported": true,
    "smokeMode": false,
    "socketReady": false,
    "port": 0,
    "received": 0,
    "authenticated": 0,
    "admitted": 0,
    "rejected": 0,
    "rateLimited": 0,
    "pending": 0,
    "maxProcessUs": 0,
    "lastResult": "idle",
    "lastAdmission": "ok",
    "senderEnabled": false,
    "sent": 0,
    "sendSkipped": 0,
    "maxEncodeUs": 0,
    "maxSendUs": 0,
    "sendTarget": null,
    "fusionSnapshot": {
      "received": 0,
      "admitted": 0,
      "sent": 0,
      "rejected": 0,
      "expired": 0,
      "source": null,
      "lastAdmission": "ok"
    },
    "siteSpaceSummary": {
      "received": 0,
      "admitted": 0,
      "sent": 0,
      "rejected": 0,
      "lastAdmission": "ok"
    },
    "lastSendResult": "idle"
  },
  "heartbeat": {
    "supported": true,
    "socketReady": true,
    "port": 49191,
    "endpoints": 1,
    "onlineEndpoints": 1,
    "received": 12,
    "authenticated": 12,
    "sent": 12,
    "rejected": 0,
    "addressChanges": 1,
    "lastResult": "ok"
  },
  "pendingCandidate": null,
  "participants": []
}
```

`siteReconciliation` reports the bounded cleanup of stale Site ownership after
the final participant relationship is removed. `state` is `not_required`,
`pending`, `complete`, or `failed`; `result` is `idle`, `ok`,
`invalid_state`, `build_failed`, `site_storage_failed`, or
`membership_storage_failed`. The enrollment window remains closed while the
state is `pending` or `failed`.

Each participant item contains only public registry state:

```json
{
  "participantId": "namo-node-example",
  "participantType": "namo_node",
  "trust": "community_unverified",
  "membership": "quarantined",
  "canonicalMembership": "quarantined",
  "directMembership": "active",
  "canonicalMember": true,
  "directPair": true,
  "leaderEligible": false,
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": false,
    "range": true,
    "presence": true,
    "motion": true,
    "zonePresence": false,
    "bridge": false,
    "maxTargets": 3
  },
  "assignment": {
    "floorplanId": "floor-main",
    "spaceId": "room-main",
    "groupId": "fusion-main",
    "geometryVersion": 7,
    "localPoseVersion": 11,
    "poseVersion": 1,
    "calibrationVersion": 1,
    "transform": {
      "originXMillimeters": 1200,
      "originYMillimeters": 800,
      "rotationCentiDegrees": 1500,
      "residualMillimeters": 45,
      "calibrationSamples": 2
    }
  },
  "directAssignment": {
    "floorplanId": "floor-main",
    "spaceId": "room-main",
    "groupId": "fusion-main",
    "geometryVersion": 7,
    "localPoseVersion": 11,
    "poseVersion": 1,
    "calibrationVersion": 1,
    "transform": {
      "originXMillimeters": 1200,
      "originYMillimeters": 800,
      "rotationCentiDegrees": 1500,
      "residualMillimeters": 45,
      "calibrationSamples": 2
    }
  },
  "credential": {
    "provisioned": false,
    "enabled": false,
    "keyVersion": 0
  },
  "endpoint": {
    "address": "192.168.1.73",
    "port": 49191,
    "authenticated": true,
    "online": true,
    "lastSeenAgeMs": 840,
    "siteConfigurationAuthorityEpoch": 1,
    "siteConfigurationGeneration": 4,
    "siteConfigurationDigest": "0123456789abcdef",
    "siteDirectoryRevision": 9,
    "siteDirectoryDigest": 123456789,
    "floorplanGeometryVersion": 7,
    "floorplanConfigHash": 12345,
    "floorplanImageHash": 67890,
    "radarPoseVersion": 1,
    "floorplanReplicaReady": true,
    "heartbeatWireVersion": 8,
    "siteCredentialsComplete": true
  },
  "readiness": {
    "ready": true,
    "missing": []
  }
}
```

`membership` is the compatibility projection and prefers
`canonicalMembership` when a canonical Site member exists. Clients that verify
a mutation must use the matching source field: Configuration Owner roster
mutations use `canonicalMembership`; pair-local mutations use
`directMembership`. Either source field is `null` when that record does not
exist. `canonicalMember` and `directPair` remain the corresponding presence
flags.

`assignment` is the compatibility/canonical projection and prefers the
committed Site spatial profile when one exists. `directAssignment` is the exact
pair-local readiness assignment persisted by `configure` or `reconfigure`, and
is `null` when no local readiness record exists. A client verifying a
Configuration Owner mutation uses `assignment`; a client verifying a pair-local
mutation uses `directAssignment`. It must not compare a pair-local write against
the canonical `assignment` projection. The canonical Site profile persists the
authoritative origin, rotation, pose, and calibration revisions, while its
`residualMillimeters` and `calibrationSamples` diagnostics are normalized to
`0` and `1`. Canonical mutation verification therefore ignores those two
diagnostic fields; direct mutation verification compares them exactly.

Before configuration, `capabilities`, `assignment`, and `directAssignment` are `null`, while
`readiness.missing` contains `capabilities`, `assignment`, `pose`, and
`calibration`. Presence-only Satellites do not require pose or calibration.

`POST /api/platform/participants` accepts form-encoded arguments with a required
`action`:

| Action | Required arguments | Behavior |
|---|---|---|
| `open` | optional `durationSeconds` (`30..300`, default `120`) | Opens a bounded enrollment window and clears any pending candidate. |
| `close` | none | Closes the window and clears the pending candidate. |
| `stage` | `participantId`, `participantType`, `protocolVersion`, `product` | Stages one compatible candidate while the window is open. |
| `approve` | none | Approves the pending candidate using trust derived from its participant type and persists it as quarantined. |
| `configure` | `participantId`, `pointTracks`, `presence`, `motion`, `maxTargets`, `floorplanId`, `spaceId`, `groupId`, `geometryVersion`, `localPoseVersion`, `poseVersion`, `calibrationVersion`; optional capability booleans `velocity`, `positionUncertainty`, `range`, `zonePresence`, `bridge`, `automaticGroup`; optional transform group `originXMillimeters`, `originYMillimeters`, `rotationCentiDegrees`, `residualMillimeters`, `calibrationSamples` | Persists the participant capability and assignment. `localPoseVersion` identifies the receiving Node's radar pose and `poseVersion` identifies the participant's radar pose at calibration. When supplied, all transform fields are required and store the approved effective transform. `automaticGroup=true` resolves the canonical Group only among profiles assigned to the requested Space before persistence. Reconfiguring an active participant first persists quarantine. |
| `reconfigure` | Same as `configure` | Replaces readiness for an existing active participant without changing membership. This action is reserved for recalibration. It rejects non-active or missing participants and restores the previous readiness if persistence or the owning Site roster transaction fails. |
| `activate` | `participantId`; optional `automaticGroup` | Activates an approved, trusted, quarantined participant only when readiness is complete. A NAMO Node must also report an online, exact canonical floorplan replica digest, and its assigned Group must contain fewer than seven active sensing participants. `automaticGroup=true` applies the staged automatic Group decision in the same canonical revision as activation. |
| `quarantine` | `participantId` | Makes an existing participant inactive without deleting it. |
| `revoke` | `participantId` | Revokes an existing participant and disables its stored transport credential when present. If either persistence step fails, the previous registry and credential state is restored. |
| `forget` | `participantId` | Deletes an existing non-active participant and erases its readiness and stored transport credential when present. If any persistence step fails, the previous registry, readiness, and credential state is restored. |
| `set_community_leader` | `participantId`, `eligible` | The configuration owner explicitly changes Leader eligibility for an active community NAMO Node. The change refreshes the canonical roster; a roster persistence failure restores the previous local eligibility. |

Rules:

- This endpoint remains the local registry administration boundary. Secure
  community enrollment is handled by `/api/platform/pairing`; attestation,
  observation authority, and fusion are not granted by this endpoint. No HTTP
  action accepts or returns raw credential material. An opted-in receive/send
  path may expose bounded `shadowTransport` counters on this response. Smoke
  samples never affect standalone presence, zones, Home Assistant entities, or
  fusion; normal authenticated participant samples can reach production only
  through the separately gated `productionFusion` adapter.
  The observation sender reuses the receive socket, emits at most two
  observation datagrams per second, never retransmits, and sends only to the
  authenticated online Group Leader when active membership, readiness, pose,
  calibration, and credential checks all pass. `sendTarget` is a Node ID,
  never an address or credential. Fusion Snapshot fan-out is separately
  bounded to one datagram per eligible receiver per 500 ms and one attempted
  receiver per component cycle.
- `set_community_leader` is accepted only on the current configuration owner.
  It does not change the configuration owner or create production authority.
  Community eligibility remains an explicit local user choice until device
  attestation can distinguish verified NAMO hardware. During initial setup, an
  accepted fallback choice is committed after canonical activation and before
  the final calibration convergence. The same action remains idempotent when
  enabled later from device management.
- Only one pending candidate may exist. The pending candidate is RAM-only and is
  cleared by close, expiry, successful approval, or reboot.
- `stage` accepts protocol version `1`. A NAMO Node must report product
  `namo-spatial-sensor`; a Satellite must provide a non-empty product identifier.
- Callers cannot choose trust. A staged NAMO Node is approved as
  `community_unverified`; a Satellite is approved as `satellite`.
  `official_verified` remains unavailable until attestation verification exists.
- Approval and activation are separate persisted mutations. Approval closes the
  enrollment window but leaves the participant `quarantined`; only `activate`
  changes it to `active` and increases `activeRemoteCount`. Unknown-trust and
  revoked participants cannot be activated. Incomplete readiness returns HTTP
  `409` with code `not_ready`.
- NAMO Node activation additionally fails with a specific HTTP `409` code when
  the canonical roster or floorplan is not committed; the participant is not
  in that roster; the endpoint is offline; the replica is not ready; or its
  generation/version/hash differs. Directory convergence is not duplicated at
  this mutation boundary: a newly active Node with a missing or stale Directory
  remains excluded from role and fusion authority until ordinary authenticated
  Directory replication converges. Credential enablement happens only after
  the activation admission check passes.
- NAMO Node activation returns HTTP `409` with code `group_full` when its
  canonical Group already contains seven active sensing participants. Existing
  active members remain unchanged.
- Spatial participants require point-track capability, `maxTargets` in `1..3`,
  Floorplan/Space/Group assignment, and non-zero geometry, local pose, remote
  pose, and calibration versions.
  Presence-only participants require `maxTargets=0` and do not invent spatial
  placement versions.
- A spatial calibration is separate from the local Floorplan radar placement.
  `configure` stores the approved local-radar-to-Floorplan transform for that
  participant relationship; it never rewrites `radar.originPx`,
  `radar.rotationDeg`, or the Node's local `poseVersion`.
- `spaceId` remains the physical/user-facing room assignment. `groupId` is an
  internal fusion component. With `automaticGroup=true`, the Configuration
  Owner resolves an existing Group only from canonical profiles whose `spaceId`
  equals the requested Space. Multiple legacy Group IDs inside that same Space
  are normalized to the selected existing Group. Profiles in another Space are
  never renamed or counted merely because the two radars completed spatial
  calibration. A same-Space normalization that would exceed seven sensing
  Nodes returns `group_full` before readiness or canonical state is changed.
- Existing readiness and Site-configuration records need no storage migration.
  Room-derived Group IDs written by older firmware remain valid opaque Group
  IDs and remain the stable Group for their Space. Empty assignments use the
  requested Space ID as the initial Group seed. If a legacy Group ID is already
  shared by profiles in different Spaces, only the requested Space is split to
  its Space ID during approved reconfiguration; unrelated Spaces are not
  rewritten. Older clients that omit `automaticGroup` retain the legacy
  explicit `groupId` behavior.
- The dashboard derives each radar's requested Space independently from its
  approved Floorplan placement, probing a bounded distance into its facing
  direction before falling back to the radar origin. The room containing the
  person captured for calibration is display evidence only and is not copied
  into both radar assignments.
- Configuring an already quarantined participant persists readiness only and
  does not publish another canonical Site roster revision. Its subsequent
  activation commits the staged spatial profile and active membership in one
  canonical revision. Reconfiguring an active participant first quarantines
  it, which is a real membership change and therefore does publish a new
  roster revision.
- Active spatial relationships are checked against the canonical Floorplan
  identity and geometry revision, the receiving Node's current radar pose, and
  the authenticated peer heartbeat's radar pose. A mismatch clears the approved
  calibration, persists quarantine, and requests recalibration without
  deleting or disabling the secure control-plane pairing relationship.
  Firmware processes at most one such relationship per loop pass.
- Entering calibration after pairing waits for the first authenticated,
  online heartbeat in both directions before requesting Site synchronization.
  Calibration preparation uses the dashboard convergence fence, while
  activation uses the firmware's roster and replica admission fence:
  authority epoch, generation, configuration digest, configuration consistency,
  geometry version, CONFIG hash, IMAGE hash, and each sender's expected local
  radar `poseVersion` must all match the current canonical Site. A stale
  `floorplanReplicaReady=true` from an older revision is not sufficient.
- Initial enrollment has one monotonic phase order: pair credential commit,
  explicit Floorplan selection when the joining payload is absent or different,
  canonical Site Configuration replication, canonical Site Directory
  replication, Floorplan replication, calibration/readiness, direct
  activation, and canonical activation. Pair confirmation persists the secure
  relationship and quarantined membership, but it does not replace a joining
  device's absent or different stored Floorplan. Automatic authenticated
  enrollment may continue without another choice only when both stored
  Floorplan payload hashes already match exactly. Cancelling at Floorplan
  selection preserves both payloads; reopening the same quarantined
  relationship resumes at Floorplan selection, or proceeds to calibration when
  the payloads already match. Compatibility mirrors are bootstrap
  storage only and are never admitted to role coordination or advertised as a
  Directory authority. A stale same-Site canonical Directory does not turn
  activation into a permanent failure; the existing authority and replication
  layers keep that Node non-authoritative until it converges.
- Initial calibration is a resumable, monotonic transaction. Both direct
  readiness records are staged while the joining canonical member remains
  quarantined. After replica convergence, the joining Node activates its
  direct relationship to the Owner first; the Owner activates the joining
  canonical member last. A failure before that final canonical activation
  leaves the joining member quarantined and is retried from the observed
  direct/canonical state. The dashboard must not add compensating quarantine
  revisions or replace the primary error with a rollback error.
- `rotationCentiDegrees` is bounded to `-18000..18000`,
  `residualMillimeters` to `0..1000`, and `calibrationSamples` to `1..2`.
  The transform group is an optional, backward-compatible extension: if any
  transform field is supplied, all fields must be valid. Current-version
  readiness records without transform data remain valid, and reconfiguring the
  same calibration version without the optional fields preserves an already
  approved transform. Version 1 and 2 readiness records remain readable but
  require recalibration because they do not contain the geometry and both pose
  baselines needed for invalidation.
- Persistent participant actions and pairing commit share one bounded operation
  lease. HTTP and component-loop mutations therefore cannot write registry,
  readiness, credentials, or the owning Site roster concurrently. A colliding
  HTTP action returns HTTP `409` with code `busy`; callers may verify the current
  participant snapshot and retry the same idempotent action.
  Enrollment-window `open`, `close`, and `stage` use the same lease so a pending
  candidate cannot change while pairing commit is persisting it.
- Before its first write, a persistent participant action snapshots the bounded
  registry, readiness, and credential stores in PSRAM. Enrollment, credential,
  or owning Site-roster failure restores each store whose generation advanced.
  A failed restoration returns HTTP `503` with code `rollback_failed` and is
  never reported as success.
- Registry, readiness, credential, and Site-configuration transactions use
  twelve reserved 4 KiB sectors at the end of the radar data partition rather
  than repeated large NVS preference writes. The original nine participant
  sectors retain their exact shipped offsets; the three Site-configuration
  sectors grow toward lower addresses.
  Firmware upgrades migrate a valid legacy NVS transaction before removing only
  its participant keys. This changes no HTTP payload or endpoint.
- When identity, registry, readiness, or credential storage is unavailable,
  affected remote-platform work fails closed; standalone sensing remains
  operational.
- Private keys, certificates, Site credentials, preference blobs, and
  attestation material must never be returned.
- A provisioned peer credential authorizes the low-rate control-plane heartbeat
  before participant activation. Quarantined peers may therefore expose a
  verified endpoint, but only active participants with complete readiness may
  enter Group coordination. Revoked or missing participants cannot send or
  authenticate heartbeats even if credential cleanup is still pending.
- The configuration owner is the credential provisioning authority for an
  active Site. Over the existing authenticated UDP `49192` replication
  transport, a Node may pull an encrypted `credential_bundle` containing only
  its required active peers: the owner, every member of its Fusion Group, and
  active Leader-eligible NAMO Nodes needed by the low-frequency Site control
  plane. The automatic configuration successor may retain every active member
  credential required by the handover contract. This never widens the
  high-frequency fusion input limit of one local plus six remote Nodes.
- A non-owner signs Site heartbeat, observation, Fusion Snapshot, Space summary,
  and replication traffic with its owner-pair credential as its sender identity.
  The owner signs with the destination's direct pair credential. Receivers map
  the authenticated token back to the payload sender; destination credentials
  are never treated as sender identity.
- After a Configuration Owner transition, a follower does not trust the stale
  participant label attached to a stored key. Credential replication probes the
  bounded local registry and binds each request to `HMAC(key, localNodeId)`;
  the new owner serves a bundle only when that proof resolves to the same active
  requester. Until the bundle commits, follower heartbeat and observation
  transmission remain closed so stale keys cannot overwrite peer endpoint
  identity. mDNS may supply only the route for this recovery; the response and
  encrypted bundle still require the pair credential. The received bundle must
  contain every required record and bind the current Owner record to the exact
  credential used by the requester, so a previous Owner cannot complete a
  replacement-Owner session.
- Credential bundles are authenticated before decryption, encrypted with the
  existing 256-bit pair key, generation-bound to the committed Site
  configuration, merged transactionally into the existing bounded registry,
  and never exposed through HTTP. Missing, partial, unrelated, or stale bundles
  fail closed without replacing the previous committed credential snapshot.
- A roster-only revision queues an exact credential-bundle refresh without
  clearing authenticated endpoints, Site/Group leadership, or summary caches.
  An Owner or authority-epoch change cancels only an in-flight credential
  bundle session, preserves the previously committed credential snapshot, and
  starts a new bounded refresh against the canonical authority. It must not
  clear Site/Group coordinators, terms, endpoint authentication, summary
  caches, or production fusion. A previously committed credential snapshot
  that is complete for the new canonical roster continues authenticated
  heartbeat and observation traffic through the bounded handover bootstrap;
  an incomplete snapshot remains fail-closed. High-frequency work remains
  independently fenced by Group credential completeness while the refresh
  converges. A real
  Site identity change, explicit platform reset, or incompatible persisted
  Site scope still uses the destructive Site-runtime reset path.
- Heartbeats use UDP port `49191`, a two-second send interval, a six-second
  lease, and a fixed packet bounded to 320 bytes. Firmware processes at most two
  receive packets and two send targets per loop pass. The socket is created only
  after a network connection exists; a failure is retried every five seconds and
  must not fail device setup or standalone sensing.
- Heartbeat version 3 adds the sender's local radar `poseVersion`. Versions 1
  and 2 remain readable during rolling upgrades. A version 2 peer cannot
  trigger remote-pose invalidation until it reports version 3, while canonical
  geometry and the local radar pose remain enforced.
- The endpoint cache is RAM-only. An authenticated heartbeat source is the
  authoritative address and online signal for a registered peer. mDNS only
  seeds a missing route or retries an endpoint after its heartbeat expires; it
  is not polled continuously while every registered peer is online. A valid
  pairwise HMAC, matching participant identity and Site, and a newer
  boot/sequence pair are required to authenticate the source address. This lets
  DHCP address changes converge without persisting IP addresses in flash.
- `endpoint.online` expires eight seconds after the last authenticated
  heartbeat. A discovered but not yet authenticated route may have an address
  while `authenticated` and `online` are both false.
- Pairing creates pairwise trust only. It does not infer a shared floorplan,
  activate a participant, or make two independently saved local documents a
  Site configuration.
- A directly paired quarantined NAMO Node may use its pairwise Owner credential
  for control-plane heartbeat while calibration is deferred. This bootstrap
  does not admit observations, enable fusion, grant leadership, activate the
  participant, or authorize Site credential bundle replication.

### 9.4 Shadow Group Coordination

`GET /api/platform/groups` returns the bounded, read-only Group coordinator
snapshot:

The `site` object and every local `groups` entry come from one immutable
control-loop publication. HTTP serialization never reads mutable coordinator
objects from different election cycles. Before the first publication, the
endpoint returns HTTP `503` with code `platform_role_snapshot_unavailable`.

```json
{
  "ok": true,
  "supported": true,
  "shadowOnly": false,
  "productionAuthority": true,
  "generation": 2,
  "capacity": {
    "maxGroups": 8,
    "maxGroupParticipants": 7
  },
  "site": {
    "generation": 2,
    "term": 1,
    "leaderNodeId": "namo-node-example",
    "localLeader": true,
    "configurationReady": true,
    "rosterRevision": 2,
    "leaseRemainingMs": 6000,
    "participantsOnline": 1,
    "participantsExpected": 2,
    "quorumRequired": 2,
    "quorumAvailable": false,
    "health": "degraded"
  },
  "groups": [
    {
      "groupId": "fusion-main",
      "configured": true,
      "term": 1,
      "leaderNodeId": "namo-node-example",
      "localLeader": true,
      "assignmentRevision": 1,
      "canonicalAssignment": true,
      "credentialsComplete": true,
      "leaseRemainingMs": 6000,
      "participantsOnline": 1,
      "participantsExpected": 2,
      "quorumRequired": 2,
      "quorumAvailable": false,
      "health": "degraded"
    }
  ],
  "groupRoster": {
    "ready": true,
    "storeResult": "loaded",
    "groupId": "fusion-main",
    "leaderNodeId": "namo-node-example",
    "generation": 3,
    "assignmentRevision": 1,
    "memberCount": 2
  },
  "siteGroupSummary": {
    "localAuthority": true,
    "capacity": 8,
    "configuredGroups": 1,
    "reportingGroups": 1,
    "generation": 9,
    "rejected": 0,
    "expired": 0
  },
  "siteGroups": [
    {
      "groupId": "fusion-main",
      "spaceId": "living-room",
      "floorplanId": "floorplan-main",
      "leaderNodeId": "namo-node-example",
      "assignmentRevision": 1,
      "term": 1,
      "leaseRemainingMs": 6000,
      "participantsOnline": 1,
      "participantsExpected": 2,
      "health": "degraded",
      "route": {
        "schemaVersion": 1,
        "available": true,
        "local": false,
        "address": "192.168.1.42",
        "port": 80,
        "statePath": "/api/state",
        "lastSeenAgeMs": 250
      }
    }
  ],
  "siteSpaceSummary": {
    "shadowOnly": true,
    "localAuthority": true,
    "capacity": 8,
    "reportingSpaces": 1,
    "generation": 9,
    "admitted": 9,
    "rejected": 0,
    "expired": 0
  },
  "siteSpaces": [
    {
      "spaceId": "living-room",
      "groupId": "fusion-main",
      "leaderNodeId": "namo-node-example",
      "term": 1,
      "sequence": 9,
      "sourceSequence": 42,
      "ageMs": 250,
      "presence": true,
      "motion": false,
      "targetCount": 1,
      "health": "healthy"
    }
  ],
  "shadowFusion": {
    "enabled": true,
    "shadowOnly": true,
    "productionAuthority": false,
    "state": "active",
    "groupId": "fusion-main",
    "remoteParticipantId": "namo-node-peer",
    "updates": 42,
    "skipped": 10,
    "maxCycleUs": 380,
    "trackCount": 1,
    "diagnostics": {
      "validInputs": 2,
      "candidatePairs": 1,
      "pairedMeasurements": 1,
      "ambiguousPairs": 0,
      "droppedMeasurements": 0,
      "tentativeTracks": 0,
      "confirmedTracks": 1,
      "coastingTracks": 0
    },
    "tracks": [
      {
        "id": 1,
        "state": "confirmed",
        "sourceCount": 2,
        "confidence": 94,
        "correlatedViews": false,
        "xMm": 1234.0,
        "yMm": 2345.0,
        "vxMmS": 0.0,
        "vyMmS": 0.0,
        "varianceXMillimeters2": 20000.0,
        "varianceYMillimeters2": 20000.0
      }
    ]
  },
  "productionFusion": {
    "buildEnabled": true,
    "authoritative": true,
    "state": "active",
    "sourceSequence": 42,
    "targetCount": 1,
    "tracks": [
      {
        "id": 1,
        "state": "confirmed",
        "sourceCount": 2,
        "confidence": 94,
        "xMm": 210.0,
        "yMm": 1840.0,
        "vxMmS": 0.0,
        "vyMmS": 0.0
      }
    ]
  }
}
```

Rules:

- Top-level `capacity` reports the independent hierarchical limits: at most
  eight Groups per Site directory and seven sensing participants per Group.
  It is not a flat Site-wide participant limit. The participant-management
  endpoint reports its separate local relationship-registry capacity.
- The coordinator remains read-only. A standard NAMO product build includes
  authenticated production fusion and reports `shadowOnly: false` and
  `productionFusion.buildEnabled: true`. `productionAuthority` remains `false`
  unless the local Node is the elected Group Leader and every runtime gate
  passes. Transport-smoke and external component builds may still compile with
  production fusion disabled. Build capability does not change persisted Site,
  readiness, or migration state.
- `shadowFusion` is present in all builds. `enabled: false` means the
  development receive/runtime path is not compiled or could not be allocated.
  An enabled runtime runs only on the elected Group Leader, accepts one local
  source plus at most six active authenticated remote point-track sources,
  and keeps at most six fused tracks. `remoteParticipantId` retains the first
  remote participant for backward-compatible diagnostics;
  `diagnostics.validInputs` reports the actual admitted input count. `state`
  explains why the runtime is idle or active. This serialized `shadowFusion`
  object is read-only and never grants production authority; the separately
  gated `productionFusion` adapter decides whether stable runtime tracks may
  enter production paths.
- `productionFusion` is a bounded adapter over the same authenticated runtime.
  It publishes only `confirmed` and `coasting` tracks, limits output to three
  targets, and converts world coordinates back into the elected Group Leader's
  local radar frame before the existing target, Zone, and state paths consume
  them. `authoritative: true` is possible only while the runtime is `active`
  and the Leader's approved transform is valid.
- `productionFusion.state: degraded` never means unoccupied. The firmware
  immediately preserves the existing local tracker path when authenticated
  transport, readiness, source count, or transforms become unavailable.
  Production presence is local presence OR an authoritative fused target;
  transport loss therefore cannot directly force presence off.
- A configured Group may continue with one online Node after the previous
  authority lease expires and that Node becomes Group Leader. When its
  canonical transform and local observation are valid, the bounded runtime
  admits that one local source and keeps Group automation available. The
  persistent `siteId`, `spaceId`, `groupId`, and expected member count do not
  become standalone; Site directory health is `degraded` while online members
  are below the configured count. Losing the local observation, transform, or
  all eligible Leaders still fails closed as unavailable.
- `local_transform_unavailable` means the elected Group Leader has no approved
  canonical Site spatial profile. The runtime fails closed instead of mixing
  an uncorrected local pose with a calibrated remote pose.
- Runtime inputs are latest-wins, fixed-size, authenticated observations.
  Expired observations disappear from the inbox. Re-reading the same selected
  local and remote sequence set does not advance fusion confirmation.
- Each NAMO Node selects only the Group named by its canonical Site spatial
  profile. Other configured Groups in the same Site do not enter its fusion
  input, observation sender, Group election, or Fusion Snapshot fan-out. A
  version 1 or 2 Site record with no canonical assignment may use the only
  configured Group as a deterministic compatibility fallback; two or more
  Groups without a local assignment fail closed.
- Authenticated transport envelope message type `2` is `Fusion Snapshot V1`.
  The elected Group Leader sends one fixed-size snapshot directly to each
  active, ready participant in its Group over UDP `49190`. It reuses the
  pairwise HMAC credential and carries `siteId`, `leaderNodeId`, `groupId`,
  `bootId`, transport `sequence`, `groupTerm`, fusion `sourceSequence`,
  `ttlMs`, presence, motion, and at most three confirmed/coasting targets in
  canonical world coordinates.
- A receiver accepts a Fusion Snapshot only when the authenticated sender is
  its currently elected Group Leader, Site and Group IDs match, the Group term
  is current, and the sender remains active and spatially ready. The cache is
  latest-wins, rejects duplicate or reordered sequences from the same boot,
  owns no queue, and expires after at most five seconds; the production sender
  currently uses a two-second TTL.
- A non-computing Node converts an admitted world-coordinate snapshot through
  its own committed spatial transform and exposes that result from its local
  `GET /api/state`. The dashboard therefore never fetches another Node's
  `/api/state` directly. The same normalized output now feeds target, Zone,
  and ESPHome entity evaluation on the Group Leader and non-computing Nodes.
- Firmware builds `GET /api/state` in a bounded PSRAM workspace with two
  8192-byte state buffers and one 2048-byte tracker scratch buffer. The 500 ms
  producer writes only the inactive buffer and publishes it atomically; the
  HTTPD task pins the active buffer until synchronous transmission completes.
  It must not create a complete response copy in internal RAM. If a snapshot
  exceeds either fixed capacity, firmware retains the previous complete
  snapshot and logs the bounded-capacity failure.
- The `automation` field of `GET /api/state` is the complete Group automation snapshot
  contract. It is additive to state schema version 1 and contains
  `schemaVersion: 1`, `complete`, `health`, `available`, stable `siteId`,
  `spaceId`, and `groupId`, `leaderNodeId`, `bootId`, `groupTerm`, transport
  `sequence`, fusion `sourceSequence`, `ageMs`, occupancy, motion, target
  count, and the complete configured Zone list. Each Zone uses its persisted
  `zone_N` slot as `zoneId`; standalone-to-fusion expansion and fusion disable
  do not rewrite Site, Space, or Zone identities.
- Consumers identify authority by `siteId`, `groupId`, `leaderNodeId`,
  `bootId`, and `groupTerm`, and order logical automation updates by
  `sourceSequence`. The transport `sequence` reports authenticated delivery
  order. A lower term or an older transport sequence from the same Leader boot
  cannot replace accepted state.
  During a forward Group Leader handoff only, firmware may expose the last
  complete snapshot with `health: recovering`, `available: true`,
  `continuityHeld: true`, `holdAgeMs`, `holdRemainingMs`,
  `continuitySourceLeaderNodeId`, and `continuitySourceGroupTerm`. The fixed
  hold is at most 30 seconds from its first transition and repeated failures
  must not extend it. The held snapshot is non-authoritative and read-only:
  motion is false and no new target, Zone, or area evidence is generated.
  Consumers may accept a source age up to the normal 5-second limit plus the
  reported hold age.
  Once the hold expires, or for stale/backward authority, configuration,
  Floorplan, pose, calibration, membership, or transform mismatch,
  `available` is false and `health` is `degraded` or `unavailable`; occupancy,
  motion, target count, and every Zone value are `null`, never false or zero.
  The `local` field retains local occupancy, motion, target count, and a
  bounded three-slot `targets` array in the local LD2450 radar coordinate
  frame. Each local target contains only integer `x`, integer `y`, and
  `active`; it is never replaced by Group fusion output. The top-level
  `targets` retain the existing normalized device behavior and may contain
  authoritative canonical coordinates while fusion is active. Spatial
  calibration must use `local.targets` from both devices and must not apply a
  radar transform to top-level authoritative fusion targets.
- `GET /api/platform/participants` reports bounded
  `shadowTransport.fusionSnapshot` counters: `received`, `admitted`, `sent`,
  `rejected`, `expired`, `source`, and `lastAdmission`.
- `shadowTransport.siteSpaceSummary` reports the authenticated low-rate Space
  summary path. It carries aggregate presence, motion, and target count only;
  it never contains target coordinates, raw observations, or Zone state.
- `tools/shadow_fusion_compare.py` performs a read-only, PC-side comparison of
  `/api/state` and this Shadow output. It counts only `confirmed` and
  `coasting` Shadow tracks as targets, excludes samples where Shadow is not
  `active`, and can write every sample to JSONL without adding history or
  comparison work to the device. Capture schema version 2 retains active
  production target coordinates, the bounded Shadow track list, per-cycle
  association diagnostics, source counts, and track separation. Its summary
  separates overcounts from undercounts, groups consecutive overcounts into
  episodes, and classifies them as no candidate pair, ambiguous pairing,
  unpaired candidate, or lifecycle overlap. Production and Shadow coordinates
  are recorded separately; the tool does not claim localization error because
  those values may not share the same observation frame.
- `site` is the independent Site Leader snapshot. It uses active participant
  liveness but never receives or forwards target observations. Exactly one
  deterministic, Leader-eligible NAMO Node wins the current Site term; a
  Satellite or quarantined/revoked participant cannot win.
- `groupRoster` reports only the local high-frequency Group's persisted roster
  metadata. Its A/B store admits at most seven NAMO Nodes including the local
  Node, resides in PSRAM at runtime, and never exposes member IDs, credentials,
  or keys through this endpoint. `ready: false` is normal for an unassigned
  standalone Node. `storeResult` describes the last local load, migration, or
  commit result.
- `siteGroupSummary` and `siteGroups` are the bounded, low-frequency Site
  Leader view. `capacity` is fixed at eight Groups. `configuredGroups` and
  every `siteGroups` identity come from the committed Site Directory, while
  `reportingGroups` counts current authenticated Group Leader reports. A
  configured Group remains in `siteGroups` when its report expires, with
  `health: unavailable` and `route.available: false`; a non-Leader has
  `localAuthority: false` and an empty array.
- In each `siteGroups` entry, `assignmentRevision` is the persistent,
  Site-Leader-owned generation of the Group assignment. `term` is the current
  Group Leader's RAM-only runtime lease epoch. They are independent values and
  must not be compared for equality. A Group Leader reboot does not change
  `assignmentRevision`; surviving peers fence its runtime election with the
  observed `term`, while a full RAM-state loss may establish a fresh term.
  Replacing the Leader, Floorplan, or expected member set
  advances `assignmentRevision`; refreshing the same assignment does not.
- Each local `groups` entry exposes `assignmentRevision`,
  `canonicalAssignment`, and `credentialsComplete`. Production observation and
  fusion authority remain closed when a committed Directory assignment exists
  but either the assigned Leader fence or its required credentials are
  incomplete. A configured Site or Group reports `health: degraded` whenever
  its Leader is blank or its role quorum is unavailable, even if every expected
  participant is currently online. A configured Group also remains degraded
  while its canonical assignment or required credential set is incomplete.
- The elected Site Leader is the sole authority for the committed Site
  Directory projection. Every active, Leader-eligible successor keeps that
  bounded directory replica current, even when it is not currently a Group
  Leader. A follower may replace an equal- or older-generation derived cache
  only with the elected Site Leader's authenticated directory. Groups removed
  from canonical Site configuration and Floorplans no longer registered by a
  canonical manifest or remaining Group are pruned from the directory instead
  of remaining as unavailable routes.
- `siteSpaceSummary` and `siteSpaces` are a RAM-only Shadow contract on the
  elected Site Leader. At most eight current Group Leaders may publish one
  aggregate Space summary every two seconds. Admission requires the authenticated
  sender, canonical Group Leader, Group term, Space, and Site to agree. The
  cache expires after six seconds and is cleared after Site Leader handover.
  This contract is diagnostic only and is not an automation authority.
- Each configured `siteGroups` entry contains one bounded route to the current
  Group Leader's complete `/api/state.automation` source when matching live
  evidence exists. A local route has
  local: true and address: null; consumers reuse the Site Leader origin.
  A remote route exposes only the IPv4 address learned from the current
  pairwise-authenticated heartbeat. Firmware never persists that address.
  Missing or expired Group summary or endpoint evidence sets
  `available: false` and `address: null` without removing the configured Group
  identity.
- A Home Assistant client keeps one config entry per stable Site and creates
  one logical device per `spaceId`, not per physical Node or current Leader.
  Multiple live `siteGroups` therefore become multiple Space devices below the
  same Site entry. If a one-member Site is still in `standalone` mode and has
  no configured Group route, the client may expose its local top-level
  `/api/state` as one local Space. It uses the radar's canonical Floorplan room
  ID when available and otherwise a Node-derived standalone identity; it must
  not invent a Group for an enrolled Site whose directory is temporarily empty.
- The Home Assistant client must not expose `automation.zones` as canonical
  Group-level Zone entities. In this contract those entries describe the
  current source Node's local device configuration, so a Group Leader change
  can change their meaning. Shared Space Zones require a separate canonical,
  Floorplan-backed contract.
- Canonical automation areas are stored in the replicated Floorplan CONFIG as
  `automationAreas`. They are owned by that Floorplan's Configuration Owner
  and therefore follow the existing Configuration Owner handover, authority
  epoch, CONFIG hash, and replication rules. No Node-local filter, reduced,
  disabled, exit, or calibration Zone may be copied into this collection.
  The bounded schema is:

  The dashboard does not expose `automationAreas` as a separate product
  concept. In a multi-Node Site, the existing Detection Zone editor reads and
  writes this collection on the Configuration Owner. Other Nodes render those
  Detection Zones read-only and direct editing to the Configuration Owner.
  Exclusion, reduced-sensitivity, disabled, exit, and calibration Zones remain
  local. A one-member standalone Site keeps the legacy local Detection Zone
  behavior.

  A newly added Zone is an in-memory draft with no selected product type. It
  is never serialized until the user selects detection, exclusion, reduced
  sensitivity, disabled, or exit; saving an untyped draft is rejected by the
  dashboard.

  ```json
  {
    "automationAreas": {
      "schemaVersion": 1,
      "revision": 3,
      "areas": [
        {
          "areaId": "desk",
          "name": "Desk",
          "spaceId": "office",
          "enabled": true,
          "pointsPx": [[120, 80], [240, 80], [240, 180], [120, 180]]
        }
      ]
    }
  }
  ```

  `areaId` is stable within a Floorplan and must match
  `[A-Za-z0-9._:-]{1,40}`. `name` is UTF-8 display text of at most 48 bytes.
  `spaceId` uses the canonical spatial assignment identifier. Each polygon
  has 3 through 8 finite Floorplan-pixel points. Firmware admits at most six
  enabled areas for its current Space and fails the area collection closed if
  the document is malformed or exceeds that bound. The product editor may
  store areas for at most eight Spaces in one Floorplan, for an absolute
  document bound of 48 areas.
- A receiving Node never interprets `pointsPx` through the radar pose embedded
  in the replicated Floorplan document. It projects the canonical polygon
  through its own `deviceConfig.floorplan.radar` origin, rotation, scale, and
  pose revision. If that local placement is missing or invalid, the dashboard
  does not render the area on the local radar map and firmware reports the
  canonical area state as unavailable. It must not fall back to the
  Configuration Owner's radar coordinates.
- `automationAreas.revision` is independent from `geometryVersion`. Editing an
  automation area advances only the area revision and Floorplan CONFIG hash;
  it must not invalidate radar calibration. A missing `automationAreas`
  member is a valid legacy document with revision zero and no canonical
  areas.
- `GET /api/state` exposes the canonical results separately from legacy local
  Zones. Top-level `areas` is evaluated from the local tracking output for a
  standalone Space. `automation.areas` is evaluated from the accepted
  authoritative fusion output. Both use this shape:

  ```json
  {
    "floorplanId": "fp-0123456789abcdef0123456789abcdef",
    "areasRevision": 3,
    "areas": [
      {
        "areaId": "desk",
        "name": "Desk",
        "presence": true,
        "targetCount": 1
      }
    ]
  }
  ```

  When the relevant source is unavailable, area `presence` and `targetCount`
  are `null`; consumers must not synthesize an empty area. Home Assistant uses
  `siteId + floorplanId + areaId` as entity identity and ignores legacy
  `zones` for canonical area entities. A standalone device without a saved
  Floorplan reports the explicit empty sentinel `floorplanId: null`,
  `areasRevision: 0`, `areasAvailable: false`, and `areas: []`. This is a valid
  local sensing state and does not make the Space unavailable.
- A Home Assistant client may combine each bounded `siteGroups` entry's
  `participantsOnline`, `participantsExpected`, and `health` with the accepted
  automation snapshot's health and `ageMs` for diagnostic display. It may
  aggregate those bounded records into one Site status, but this diagnostic
  summary is not an automation authority and must not synthesize occupancy.
- spaceId is resolved from active committed canonical spatial profiles. A
  missing Space or one Group mapped to conflicting Spaces makes the route
  unavailable instead of asking a consumer to infer product identity.
- Route identity is the surrounding groupId, leaderNodeId, and term, not the
  address. Consumers refresh the Site Leader directory at low frequency and
  switch a Space source only after accepting that authority fence. An
  unavailable route means unknown/unavailable state; it must never synthesize
  unoccupied, zero targets, or cleared Zones.
- A Group report is accepted from the directory-assigned Group Leader. During
  canonical-directory bootstrap, a previously unknown Group is accepted only
  from the deterministic live Leader selected from the shared Site topology;
  its Group ID, Floorplan ID, assignment revision, term, and configured member
  count must agree. Reports contain health counts and routing metadata only.
  They never contain target observations, fused coordinates, floorplan images,
  or geometry payloads.
- Site heartbeat version 6 adds a Floorplan ID, a bounded owner-advertisement
  flag, and a Site-topology revision/digest that excludes independent
  Floorplan owner and payload revisions. Version 7 additionally advertises the
  committed Site Directory topology revision and semantic payload digest. A
  current Group Leader sends its summary only to the elected Site Leader on
  authenticated UDP `49191`. The Site Leader keeps at most eight live
  Floorplan-owner advertisements in PSRAM-first fixed storage and commits a
  directory change only when owner and Group metadata agree. Version 7 readers
  continue to accept versions 1 through 6 and periodically transmit v6, v5, or
  v4 until each peer proves the corresponding newer fence. Advertisement and
  summary leases expire from RAM and are cleared immediately when the local
  Node is no longer Site Leader.
- Site Leader election prefers a valid v7 Directory fence, then the higher
  Directory topology revision, before normal role and Node-ID tie breaking. A
  multi-candidate Node waits at most one Site lease after boot to discover a
  fresher authority. A committed multi-Node Site grants Site Leader authority
  only while fresh authenticated heartbeats cover a strict majority of its
  active canonical members. A minority partition reports no Site Leader and
  cannot perform authoritative shared writes; local sensing remains available.
  Equal-revision Directory divergence is resolved
  by the deterministic elected Site Leader, whose authenticated replica may
  repair followers with the same revision and a different digest.
- When `configurationReady` is true, expected membership and Leader admission
  are bounded by the committed Site roster rather than by local participant
  records alone. The canonical roster is also the sole source of Leader
  eligibility; pair-local registry trust remains an authentication and
  liveness boundary. `rosterRevision` identifies that election boundary.
- Readiness supplies the proposed `spaceId` and `groupId` during calibration.
  Once approved, Site-configuration storage version 3 persists those values in
  each NAMO Node's canonical spatial profile. Coordinators, observation
  transport, fusion input selection, and Fusion Snapshot fan-out use this
  canonical assignment. Readiness remains the compatibility fallback for
  version 1 and 2 records until the configuration owner publishes version 3.
- One local NAMO Node uses the same term and lease contract and is reported as
  an unconfigured `standalone` Group with `groupId: null`.
- Only an active, Leader-eligible NAMO Node can win deterministic Group
  election. When canonical configuration is available, Site and Group
  coordinators use its identical eligibility set on every Node. The
  participant registry is used only as the fallback before canonical
  configuration exists. Satellites count toward health but can never become
  Leader.
- A configured multi-Node Group grants Group Leader and production fusion
  authority only while fresh authenticated heartbeats cover a strict majority
  of that Group's active canonical members. Resource observations may be used
  as future soft ranking hints, but they must never make an otherwise valid
  candidate ineligible. The current healthy authority is not preempted merely
  because another Node would rank better.
- If no eligible Node is online, the coordinator reports no Leader and degraded
  health. An ineligible local Node must not promote itself merely because it is
  local.
- Heartbeats use fixed-size RAM state and reject expired, duplicate, and
  reordered input. Only the deterministically selected local winner mints a
  newer term. Followers adopt that winner's authenticated term without
  incrementing it again. A lower term from the active Leader is rejected while
  its lease is valid; a restarted candidate cannot replace a surviving cluster
  term with its local reset value. If every Node loses RAM state, the bounded
  election can establish a fresh term without comparing it to the independent
  persistent assignment revision. None of these transitions changes production
  sensing by itself.
- `health` is `standalone`, `healthy`, or `degraded`. Transport loss must not be
  represented as an unoccupied room.
- The authenticated peer heartbeat sender/receiver runs in normal firmware.
  Heartbeats from active participants with complete Group readiness feed this
  shadow coordinator. They still do not grant production fusion authority.
- Heartbeat wire version 2 also carries only a bounded Site-configuration
  generation and floorplan geometry/config/image digest. Version 1 packets are
  accepted for endpoint liveness but cannot prove floorplan replica readiness.
- `siteTerm` carries the current bounded Site coordinator term. Remote
  authority adoption does not increment it a second time. Expired, duplicate,
  and reordered input is rejected, and lower-term input from the active Leader
  is rejected during its valid authority lease. A selected replacement first
  issues a term newer than the terms observed from the surviving cluster;
  followers do not invent their own successor terms. Shared Site-configuration
  commits remain a Phase 5 responsibility. `productionAuthority` becomes
  `true` only while the elected
  Group Leader in a standard product build has an active, admitted production
  output.

### 9.4.1 Site Configuration Foundation

`GET /api/platform/site-configuration` returns the persisted, bounded Site
configuration header and canonical roster. The roster contains at most twenty
participants, including the local NAMO Node. It is configuration state, not a
live endpoint list; addresses and online status remain heartbeat-derived RAM
state.

This implemented public schema represents one Floorplan and one Configuration
Owner. The internal multi-Floorplan directory assigns an independent
Configuration Owner to each Floorplan, but it is not exposed through the current
public API. A versioned public contract and client tests are still required
before clients may use it directly.
An internal bounded directory codec, host-tested single-Floorplan conversion,
and authenticated metadata convergence now exist. The compatibility directory
is also persisted in reserved
transactional ESP32 flash slots at boot, but it is not exposed by this endpoint
and does not change the current schema. The elected local Site Leader may
atomically promote that mirror to an internal single-Floorplan projection only
when the committed manifest, every active member's spatial assignment, bounded
Group membership, and agreed Group Leaders are complete. Explicit provenance
keeps this transitional projection refreshable without allowing it to overwrite
a future native multi-Floorplan directory. Before canonical activation, a
pairing or calibration transaction may name the exact canonical Configuration
Owner while the joining Node remains quarantined, including the bounded
interval after its direct Owner relationship becomes active. After canonical
activation, explicit and automatic synchronization resolve the owner
assigned to the local Group and reject a different owner or a returned Floorplan
manifest that does not match the directory. Site topology fencing is separate
from per-Floorplan generation fencing, so two valid Floorplan owners do not
create a false Site-configuration conflict. CONFIG and IMAGE still move
directly from the assigned Configuration Owner over the existing replication
transport. No new public field or endpoint is introduced by this internal
transition.

```json
{
  "ok": true,
  "schemaVersion": 2,
  "siteId": "site-example",
  "authorityEpoch": 1,
  "generation": 2,
  "configurationDigest": "0123456789abcdef",
  "configurationConsistent": true,
  "rosterRevision": 2,
  "rosterCommitted": true,
  "configurationOwnerNodeId": "namo-node-owner",
  "localOwner": true,
  "sharedWritesAllowed": true,
  "memberCount": 2,
  "activeMemberCount": 1,
  "floorplan": {
    "committed": false,
    "floorplanId": null,
    "geometryVersion": 0,
    "configHash": 0,
    "imageHash": 0
  },
  "members": [
    {
      "participantId": "namo-node-owner",
      "participantType": "namo_node",
      "trust": "official_verified",
      "membership": "active",
      "leaderEligible": true,
      "spatialProfile": {
        "spaceId": "room-main",
        "groupId": "room-main",
        "poseVersion": 3,
        "calibrationVersion": 7,
        "transform": {
          "originXMillimeters": 1000,
          "originYMillimeters": 800,
          "rotationCentiDegrees": 0
        }
      }
    }
  ],
  "replication": {
    "supported": true,
    "socketReady": true,
    "state": "idle",
    "result": "idle",
    "validationResult": "not_evaluated",
    "mode": "strict",
    "operation": "floorplan",
    "ownerNodeId": null,
    "remoteGeneration": 0,
    "transferredBytes": 0,
    "totalBytes": 0,
    "requests": 0,
    "responses": 0,
    "retries": 0,
    "rejected": 0,
    "replayed": 0,
    "staleRequests": 0,
    "autoPendingAuthorityEpoch": 0,
    "autoPendingGeneration": 0,
    "autoStartInMs": 0,
    "autoRetryCount": 0,
    "directoryAutoActive": false,
    "directoryAutoStartInMs": 0,
    "directoryAutoRetryCount": 0,
    "source": {
      "active": false,
      "participantId": null,
      "leaseRemainingMs": 0,
      "accepted": 0,
      "busy": 0,
      "completed": 0,
      "expired": 0
    }
  },
  "automaticHandover": {
    "state": "idle",
    "condition": "owner_online",
    "successorNodeId": "namo-node-successor",
    "waitRemainingMs": 0
  }
}
```

`replayed` counts authenticated duplicate requests answered from the bounded
PSRAM response cache. `staleRequests` counts old or request-ID-reuse packets
discarded during the short source lease. Exact retries remain idempotent and do
not extend the source lease.

`replication.mode` is `strict`, `automatic_owner_handover`, or
`authenticated_enrollment`. `validationResult` exposes the final canonical
configuration decision. It is reset to `not_evaluated` when a transfer starts
and is updated both after decoding and immediately before transactional commit.

The response is serialized directly through one reusable 768-byte PSRAM chunk
workspace; firmware does not accumulate the complete roster JSON in internal
heap. If that workspace is unavailable, ESP32 firmware returns HTTP `503` with
`code: "site_configuration_response_unavailable"` and
`detail.resource: "site_configuration_workspace"`. It does not fall back to a
persistent internal-RAM response workspace.

`POST /api/platform/site-configuration` accepts form-encoded actions:

| Action | Parameters | Behavior |
|---|---|---|
| `claimOwnership` | none | Only when the persisted owner is unresolved and the roster is uncommitted, explicitly makes the local NAMO Node the configuration owner and atomically commits the local non-revoked participant registry as the canonical roster. |
| `commitRoster` | none | The configuration owner commits the local Node and all non-revoked local participant records as the sorted canonical roster. |
| `commitFloorplan` | `floorplanId`, `geometryVersion`, `coordinateWidthMm`, `coordinateHeightMm` | The configuration owner commits the hashes of the currently stored floorplan CONFIG and IMAGE as the authoritative manifest. The coordinate dimensions describe the full image frame in millimeters. It does not upload either payload again. |
| `sync` | optional `ownerNodeId` | Queues an authenticated pull. During pairing/calibration, an explicit ID that exactly matches the canonical configuration owner may use the authenticated direct relationship while the joining canonical member remains quarantined, including after the direct relationship becomes active. Otherwise omission uses the local Group's assigned Floorplan owner when a canonical directory exists, or the persisted legacy owner ID. An explicit owner cannot replace a different canonical assignment. |
| `cancelSync` | none | Cancels the local pull session without changing the active floorplan. |
| `resolveConflict` | `confirm=1`, `expectedDigest` | Recovery-only action. On the locally owning device selected by the operator, preserves that complete roster, manifest, and spatial profiles while advancing the authority epoch. It is rejected unless an authenticated peer currently proves a configuration conflict and the supplied 16-digit hexadecimal digest still matches the selected local snapshot. |

Rules:

- An authenticated canonical NAMO member may read the Site Configuration while
  quarantined because that payload is what lets it converge back to the active
  roster. While its local Directory is still the compatibility mirror created
  during initial enrollment, it reads a committed, non-compatibility Site
  Directory through its direct Configuration Owner credential. For Sites with
  more than two active members, a non-Site-Leader Owner may serve this copy only
  when the elected Site Leader is online under quorum and advertises the exact
  same revision and digest. This read-only bootstrap never replaces a nonzero
  canonical Directory. Revoked members remain rejected, and neither exception
  grants Floorplan, credential, election, or mutation authority.
- Shared writes and NAMO activation return `site_configuration_conflict` only
  when an online authenticated peer advertises different content for the same
  fenced revision. A peer that is behind, ahead, or not fenced yet is a bounded
  convergence condition and returns `site_configuration_continuity_unavailable`.
  A consistent quorum with no non-owner member opted into Leader eligibility
  remains writable in emergency concentrated-role mode. If an opted-in
  successor exists but is offline, stale, credential-incomplete, or missing the
  committed Floorplan replica, the continuity fence still blocks shared writes.
- `commitFloorplan` is idempotent. When the requested Floorplan ID, geometry
  version, CONFIG hash, and IMAGE hash already match the committed manifest,
  firmware returns the current Site configuration before applying the shared
  write continuity gate. Any changed manifest remains subject to the gate.
  When its coordinate frame changes, committed radar transforms that remain
  inside the new frame are preserved. Each transform outside the frame is
  moved to its center and receives a newer calibration revision; the dashboard
  then lists every affected device and links to its floorplan placement page.
  `resolveConflict` returns `site_configuration_conflict_changed` if the peer
  conflict disappeared or the selected local digest changed before commit.

- A standalone Node bootstraps a committed one-member roster and owns its Site
  configuration without requiring HACS or another device.
- A Node that is the only active member of its previous Site and has no other
  non-revoked participant may adopt the inviting Site during confirmed
  pairing. Stale revoked records do not cause `site_conflict`. A Node that
  still manages another non-revoked participant remains blocked so pairing
  cannot silently orphan that existing relationship.
- Pairing persists pairwise trust before shared configuration. The Leader must
  first commit itself as the single configuration owner. A Joiner replaces its
  local Site record with an unresolved, uncommitted roster and accepts the
  owner only from the authenticated canonical configuration it pulls from that
  Leader. A Joiner never writes the peer as owner by inference.
- An unresolved one-member Joiner may accept a mature Site's higher authority
  epoch only in `authenticated_enrollment` mode. The source must be the exact
  enabled pairwise credential left by confirmed pairing, the remote canonical
  owner must be that source, and the remote roster must retain the local Node.
  Normal replication and automatic owner handover keep their existing strict
  epoch fences. The enrollment exception closes permanently after convergence.
- Adding another Node to an established Site always keeps the current
  Configuration Owner's committed Floorplan canonical. A peer Floorplan can be
  selected only while creating the initial relationship for a one-member Site.
- Reversing the roles of the same two devices is allowed only during a
  user-confirmed pairing commit and only when neither device has another
  non-revoked relationship. The new Leader may then reclaim authority from that
  exact peer while preserving its local floorplan manifest. A different
  existing owner or any third participant fails with `site_conflict`.
- Removing the final non-revoked relationship clears the RAM-only pairing
  session and converges the device to a one-member standalone configuration.
  Recovery retains the existing `siteId`, makes the local NAMO Node the
  configuration owner, removes stale roster members, and changes the local
  membership mode to `standalone`. It never invents a remote owner from network
  visibility. Local floorplan payload and manifest, radar pose, zones, tuning,
  and calibration remain intact.
- An already-enrolled Node upgrading from firmware without this record starts
  with an unresolved owner and an uncommitted local roster. It must not guess
  ownership from whichever peer happens to be visible. The user may explicitly
  resolve that state with `claimOwnership`; confirming a new pairing while the
  local Node acts as Leader performs the same guarded claim. Once an owner or a
  committed roster exists, the claim fails closed and cannot replace it.
- The storage uses two data slots and one commit marker. A failed update leaves
  the previous committed roster readable after reboot.
- Site-configuration storage version 2 adds an optional canonical
  `spatialProfile` for each rostered NAMO Node. Version 3 extends that profile
  with the approved `spaceId` and `groupId`, raises the bounded codec workspace
  to one 4 KiB flash sector, and still fits twenty maximum-length assignments.
  Version 1 and 2 records remain readable. During the next successful owner
  reconciliation, a single unambiguous readiness Group is promoted into the
  canonical profiles. The configuration owner also runs this reconciliation
  during boot when an upgraded record still has empty assignment IDs. Matching
  pose and calibration revisions fill only `spaceId` and `groupId`; approved
  transforms and revisions remain unchanged. Multiple possible local Groups
  are never guessed. A missing profile or ambiguous assignment fails closed
  for spatial admission.
- Storage version 4 adds `authorityEpoch`. Versions 1 through 3 remain
  readable as epoch 1. Replacing authority compares `(authorityEpoch,
  generation)` lexicographically, so an old owner cannot regain authority by
  publishing a larger generation from an earlier epoch.
- The configuration owner publishes both sides of an approved calibration into
  the canonical Site record. Roster refresh and automatic configuration-owner
  handover preserve those profiles byte-for-byte. A newer approved calibration
  may replace them; a same-version ownership change may not silently re-anchor
  the new owner to its uncalibrated local placement. A successful automatic
  handover immediately runs the same assignment-only reconciliation used at
  boot so a newly promoted owner cannot leave legacy profiles unassigned.
- Site configuration and floorplan payload transfer use authenticated HMAC
  packets on UDP 49192. Each payload body is at most 384 bytes. The follower
  writes CONFIG and IMAGE directly to inactive flash slots, verifies both
  hashes, and switches both active slots with one header commit. No complete
  floorplan image is buffered in RAM or processed on the HTTP task.
- The same UDP 49192 transport carries the bounded Site directory as a
  separate `site_directory` operation. Normally only the elected Site Leader
  serves a committed canonical directory, and every active NAMO Node keeps a
  local copy. Initial enrollment instead uses the already authenticated
  Configuration Owner as a read-only source before activation. With more than
  two active members, that Owner must either be the elected Site Leader or be
  confirmed by a live quorum and an elected Site Leader advertising the exact
  same Directory. The one- and two-active-member reset fallback remains
  bounded to the Owner or sole canonical peer.
- A follower checks the elected Site Leader at a low-frequency 30-second
  interval. Bootstrap and ordinary requests use deterministic 2.5-second
  roster slots and bounded backoff. Bootstrap accepts only a committed
  non-compatibility Directory into a local compatibility mirror; a nonzero
  equal-revision conflict still fails closed. The mirror's topology revision is
  derived from the Site Configuration roster and is never compared as a
  canonical Directory authority revision. This convergence is automatic and
  has no POST action, selector, or setup wizard.
- Site-directory metadata compares `topologyRevision` and a deterministic
  semantic payload hash. The per-device A/B persistence `generation` is local
  bookkeeping and is never used as cross-device authority. A newer topology
  is committed transactionally through the existing two-slot directory store;
  equal canonical revisions with different payloads fail closed as
  `revision_conflict`. An equal-revision canonical record may replace a local
  compatibility mirror.
- While `operation` is `site_directory`, `remoteGeneration` carries the remote
  topology revision. `directoryAutoActive`, `directoryAutoStartInMs`, and
  `directoryAutoRetryCount` are optional runtime diagnostics only. They are
  not persisted authority and are not intended as user controls.
- A Configuration Owner serves at most one authenticated Floorplan follower
  lease at a time. Other followers receive a bodyless, authenticated busy
  response and retry the whole session through the existing deterministic
  slots and bounded backoff; the owner stores no waiting queue. The `source`
  object reports the active requester, remaining 2.5-second lease, and bounded
  lifetime counters. Wire version 1 preserves rolling-update parsing by using
  the existing `storage_failed` status with the authenticated reserved payload
  hash `0x42555359` for this retryable busy response. Site-directory metadata
  requests do not take the Floorplan source lease.
- Storage-header version 1 migrates its legacy image into the non-overlapping
  second image slot before publishing version 2. An interrupted migration keeps
  the version 1 header authoritative.
- Successful local participant membership changes refresh the canonical roster
  automatically on the configuration owner. If the follow-up roster commit
  fails after activation, the participant is quarantined again while its
  authenticated control-plane credential remains available for recovery.
- An authenticated canonical replica may keep the receiving NAMO Node
  quarantined while readiness or calibration is being replaced. A non-revoked
  local roster member may receive that control-plane update, but remains
  excluded from heartbeat leadership, observation admission, and fusion until
  the final active roster is replicated. Missing and revoked members are
  rejected.
- Each Node pulls directly from the configuration owner. A Site Leader does not
  relay image chunks unless it is also that owner.
- The automatic configuration-owner successor is never random. Every Node
  computes the same candidate as the lexicographically smallest active,
  Leader-eligible NAMO Node other than the current owner. An owner loss does
  not immediately write flash: the designated successor must observe the same
  strict canonical Site majority used by Leader authority, hold a complete
  local floorplan replica, retain enabled
  pairwise credentials for every active member, and remain eligible for two
  complete Site lease windows and remain stable for 30 seconds.
- A successful automatic handover advances `authorityEpoch` and changes only
  the configuration owner in one transactional Site-configuration update. It
  retains the complete canonical roster, `rosterRevision`, topology digest,
  spatial profiles, and floorplan manifest. Control-plane writes remain fenced
  until the new authority revision converges, while a matching topology fence
  keeps the spatial data plane active and does not itself force Site or Group
  Leader terms to change. Normal role-separation rules may still reassign a
  Leader when the new owner already holds that role. A failed persistence
  attempt enters a failed state and does not retry flash writes on every
  component loop.
- `sharedWritesAllowed` is `false` on a multi-Node owner while its designated
  automatic successor is offline, behind the current generation, or missing
  the current floorplan replica or required active-member credentials. Existing
  sensing and reads continue, but
  shared floorplan and non-removal roster mutations return
  `site_configuration_continuity_unavailable`. This prevents the old and new
  owners from accepting concurrent shared writes during a two-way network
  partition. Removing, revoking, or quarantining a broken relationship remains
  available as an explicit recovery path. A Site with no eligible automatic
  successor keeps its single-owner behavior and does not enable automatic
  handover.
- Before a dashboard writes canonical Floorplan data, it flushes pending
  Node-local device configuration first. A transient `sharedWritesAllowed`
  false caused by an online successor applying the previous revision is
  handled as bounded convergence, not as an immediate failed save. Repeated
  user saves during that interval are coalesced to the latest canonical
  document. If convergence does not complete within the bounded wait, the
  dirty edit remains available and the existing
  `site_configuration_continuity_unavailable` result is shown. A different
  Node's browser-local draft is neither observable nor writable by the owner
  and never participates in this gate.
- Spatial calibration treats `site_configuration_continuity_unavailable` as a
  bounded convergence condition around Configuration Owner mutations. It
  waits for `sharedWritesAllowed`, retries the raced mutation once, and does
  not add another quarantine revision solely because continuity is still
  converging. Initial calibration checks the joining pair against the unchanged
  canonical snapshot after staged configure, activates both direct and
  canonical membership, nudges existing online active peers to pull the one
  activation revision, and then waits for final canonical convergence.
- When a receiving Node observes a newer canonical Floorplan revision while
  its own dashboard has pending local Zone edits, that dashboard saves the
  serializable local Zones before applying the canonical areas. An untyped
  placeholder is transient and is discarded rather than uploaded or allowed
  to block the canonical refresh.
- Nodes that still carry the previous owner accept a newer owner only while an
  automatic pull is authenticated directly from that deterministic successor.
  Manual `sync` and arbitrary peers cannot change ownership. Pulls from the
  successor keep the existing deterministic 2.5-second member slots.
- `automaticHandover` is read-only diagnostic state. `state` is `idle`,
  `blocked`, `waiting`, `ready`, `complete`, or `failed`; `condition` explains
  whether the owner is still online, no successor exists, the local Node is
  not the designated successor or Site Leader, the replica or credentials are
  incomplete, or the stability window is active. There is no handover POST
  action or manual successor selector.
- A candidate-construction or persistent-store failure schedules a bounded,
  jittered retry under the same semantic authority fingerprint. Retry delay is
  capped, identical evaluations before the deadline do not write again, and an
  eligibility or fingerprint change restarts the stability fence. Clients must
  not simulate recovery by replaying an unrelated mutation.
- After a successful owner-side save of shared image, room geometry, scale,
  walls, names, occlusion, or furniture, the dashboard commits the stored
  CONFIG/IMAGE hashes as the authoritative manifest. Radar pose, zones,
  LD2410C tuning, and calibration remain local and do not publish a Site
  generation. Repeating an unchanged manifest commit is idempotent.
- A newer owner generation schedules an automatic local pull by canonical
  follower order in 2.5-second slots. Nineteen followers therefore spread
  their first request across at most 47.5 seconds instead of hitting the owner
  together. A Node absent from the canonical roster does not auto-sync.
- A failed whole transfer retries after 2.5, 5, 10, 20, and 30 seconds, then
  stops retrying that generation. A newer generation or explicit `sync` action
  opens a new attempt. Packet-level retries remain bounded independently.
- `autoPendingGeneration`, `autoStartInMs`, and `autoRetryCount` are optional
  runtime diagnostics and are not persisted authority.
- Heartbeat wire version 5 carries the authority epoch, generation, and a
  deterministic 64-bit semantic configuration digest. An online peer must
  report the exact tuple before it may participate in Site/Group election,
  shared writes, directory reconciliation, or fusion. A mismatch reports
  `configuration_conflict` and those control paths fail closed while local
  sensing continues.
- During a rolling firmware update, a v5 Node periodically emits the established
  v4 heartbeat until the peer proves v5 support. This keeps older peers from
  treating the upgraded device as offline. A later v4 compatibility packet may
  refresh liveness and replica metadata but cannot erase a previously learned
  v5 authority fence.
- Heartbeat wire version 6 preserves the v5 configuration fence and adds a
  roster-revision Site-topology digest plus bounded Floorplan routing metadata.
  The topology digest excludes the independent Configuration Owner and
  Floorplan manifest, preventing valid Floorplan-specific generations from
  being treated as a Site split. A v6 Node periodically emits v5, or v4 to an
  unfenced peer, until the peer proves v6 support.
- Heartbeat wire version 7 preserves the v6 topology fence and adds the
  committed Site Directory topology revision and semantic digest. Site Leader
  election uses this fence to prevent a rebooted stale Directory holder from
  becoming authority before a fresher online candidate. A v7 Node periodically
  emits v6 until the peer proves v7 support.
- Heartbeat wire version 8 preserves the v7 Directory fence and adds the local
  canonical credential-readiness result. The current Configuration Owner keeps
  shared writes closed until its deterministic successor authenticates a v8
  heartbeat with complete required credentials. A v8 Node periodically emits
  v7 until the peer proves v8 support.
- Heartbeat wire version 9 preserves v8 and adds bounded Site and Group
  election-view digests. Its legacy Group digest includes each member's
  credential-readiness bit.
- Heartbeat wire version 10 keeps credential readiness as a hard Group Leader
  candidate and authority-admission gate, but removes that transient bit from
  the Group election-view identity. The v10 Group digest still includes the
  live canonical member set and Directory assignment qualifier, so an
  asymmetric partition or stale assignment cannot gain authority. During a
  rolling update, a v10 Node emits the v9-compatible digest until the peer
  proves v10 support. A short mismatched-view interval clears public Group
  authority; if the exact same non-rebooted Leader returns, its suspended term
  is restored, while an actual Leader replacement advances the term.
- Heartbeat wire version 11 binds each Site and Group election-view digest to
  the deterministic selected winner. Only that selected local winner may mint
  a newer role term; followers relay the cluster term but may adopt a term only
  from the selected winner. Re-electing the same suspended Leader preserves its
  term, while a real replacement advances it exactly once. This prevents every
  follower from independently incrementing the term or endorsing the same
  member view while selecting different Leaders. During a rolling update, a
  v11 Node emits the v10-compatible digest until the peer proves v11 support.
- Heartbeat wire version 12 stabilizes the winner-bound Site and Group election
  identity on the complete configured canonical roster and its roster revision.
  Heartbeat arrival order, temporary liveness, local Directory replication, and
  credential readiness no longer alter that identity; they remain independent
  authority-admission fences. A v12 Node emits the exact v11 winner-bound
  compatibility digest until its peer proves v12 support, so one-to-seven-Node
  rolling updates do not reinterpret two contracts as the same wire version.
- An active current-wire Node exactly one generation and roster revision behind
  or ahead in the same authority epoch may retain heartbeat and existing fusion
  only when its credentials are complete and its Floorplan geometry, hashes,
  and radar pose exactly match the canonical spatial profile. During this
  transition `configurationConsistent` and shared writes remain closed, and
  owner advertisements and Group summaries are not admitted. Epoch changes,
  non-adjacent revisions, legacy wire versions, and spatial mismatches remain
  `configuration_conflict`.
- Revisions compare authority epoch before generation. Equal fenced revisions
  with different content are reported as `revision_conflict`; older fenced
  revisions are rejected. `configurationConsistent` is false while any online
  active peer advertises a different fence or has not yet advertised a v5
  fence.
- `resolveConflict` is intentionally absent from the dashboard. The recovery
  tool `tools/site_configuration_conflict_repair.py` backs up both devices,
  requires an explicit source, applies the guarded epoch change, and verifies
  exact owner, revision, digest, and Floorplan convergence.
- Local radar placement remains a per-Node sidecar. Loading replicated shared
  geometry overlays the local radar pose instead of copying the owner's pose.
- A NAMO Node cannot be activated until an authenticated, online heartbeat
  reports the exact canonical generation, geometry version, CONFIG hash, and
  IMAGE hash. Presence-only Satellites keep their existing readiness contract.
- This endpoint alone cannot bypass production gates. Spatial authority requires
  the canonical Floorplan replica, current radar pose and calibration,
  readiness, active roster membership, credentials, and current Group authority.

### 9.5 Secure Community Pairing

`GET /api/platform/pairing` returns one bounded, RAM-only pairing session. It
contains public offers and proofs only. Private ECDH material, the derived master
key, transport key, and participant token are never returned.

```json
{
  "ok": true,
  "supported": true,
  "busy": false,
  "acceptedOperationId": 4,
  "completedOperationId": 4,
  "lastOperation": "verify",
  "operationResult": "ok",
  "commitResult": "ok",
  "state": "verified",
  "role": "leader",
  "remainingMs": 164200,
  "prepareRateLimited": 0,
  "peerVerified": true,
  "peerParticipantId": "namo-node-peer",
  "leaderSiteId": "site-example",
  "invitation": {
    "state": "pending",
    "requesterAddress": "192.168.1.40",
    "remainingMs": 54200
  },
  "offer": {
    "role": "leader",
    "participantId": "namo-node-local",
    "siteId": "site-example",
    "sessionId": "32 hex characters",
    "nonce": "64 hex characters",
    "publicKey": "130 hex characters"
  },
  "localProof": "64 hex characters",
  "confirmationCode": "042731"
}
```

`POST /api/platform/pairing` accepts form-encoded operations:

| Action | Required arguments | Behavior |
|---|---|---|
| `invite` | `requesterAddress`; optional `durationSeconds=30..180` | Stores one bounded RAM-only connection prompt. The dashboard uses 180 seconds. It does not open enrollment or start cryptography. |
| `rejectInvite` | none | Marks the current prompt rejected for a short bounded period so the requesting wizard can stop waiting. |
| `dismissInvite` | none | Clears the RAM-only prompt without changing pairing or enrollment state. |
| `prepare` | `role=leader|joiner`; optional `durationSeconds=30..300` | Creates a fresh P-256 offer while the local enrollment window is open. |
| `exchange` | all fields from the peer `offer` | Derives a transcript-bound pair key using P-256 ECDH and HKDF-SHA256. |
| `verify` | peer `proof` | Verifies HMAC possession of the transcript key. |
| `confirm` | six-digit `confirmationCode` | Persists the peer as `community_unverified`, enables its pairwise credential for authenticated control-plane traffic, and leaves it quarantined. |
| `cancel` | none | Wipes the RAM-only session and its secret material. |

Accepted cryptographic pairing operations return HTTP `200` with `accepted: true` and
`operationId`. This intentionally uses `200` because the current ESPHome IDF web
server wrapper maps unsupported response codes to `500`. Callers poll the GET
endpoint until `completedOperationId` matches and `busy` is false. Expensive
ECDH and persistence work runs from the component loop rather than the ESP-IDF
`httpd` task.

After a mutation has been accepted, a transient GET failure does not prove that
the operation failed. Callers retry status reads with bounded backoff and then
reconcile the final committed state. They must not replay the mutation solely
because one status read failed. This resilience rule applies to pairing and to
other accepted-operation APIs that expose an operation ID.

Repeated `prepare` requests are accepted only when the pairing state is idle or
committed and are limited to one accepted request per second. Rejected bursts
return the existing `pairing_operation_busy` response and increment
`prepareRateLimited`; they do not run P-256 work.

`commitResult` distinguishes transition policy from storage failure.
`site_conflict` means another live relationship or unrelated configuration
owner prevents the requested move. `site_state_invalid` means the persisted
Site state could not produce a valid final roster. `persistence_failed` is
reserved for an actual transactional storage write failure.
`site_configuration_owner_required` means the request reached a Node that is
not the current Configuration Owner; dashboards direct the user to the online
Owner instead of continuing cryptographic pairing on the wrong Node.

When standalone Site reconciliation is still pending or has failed, actions
that begin or advance pairing return HTTP `503` with
`site_reconciliation_pending` or `site_reconciliation_failed`. `cancel`,
`rejectInvite`, and `dismissInvite` remain available so RAM-only state can
always be cleared. Reconciliation is a one-shot component-loop transaction; it
does not repeatedly write flash after a failure.

The invitation actions are immediate bounded state changes and do not return an
operation ID. At most one prompt is retained, it expires after at most 180
seconds, and it is never persisted. A prompt only asks the local user for
permission; accepting it starts the existing enrollment and pairing flow, while
rejecting or expiry grants no trust.

Both devices must open an enrollment window, take opposite roles, exchange and
verify proofs, display the same six-digit code, and receive a local user
confirmation. A successful confirmation on each side stores the same pairwise
token/key and the other Node ID. The joining Node adopts the Leader Site ID only
after its registry and credential writes succeed. Interrupted work remains
quarantined and can be retried without granting observation authority.
If the peer committed but its HTTP response was lost, the initiating dashboard
verifies the peer's committed session before continuing. If the peer cannot be
queried, the local quarantine anchor is retained and the result is reported as
uncertain; it is never deleted speculatively. Repeating confirmation for an
already converged Joiner preserves its canonical Site configuration.

After both pairing confirmations, the dashboard requests replication of the
current Site revision and waits for the Configuration Owner's
`sharedWritesAllowed` gate before committing the selected Floorplan manifest.
If that exact commit races one further continuity transition, the dashboard
waits and retries the commit once. Other errors are never retried implicitly.
The newly committed revision is then synchronized to the Joiner and verified
with the existing bounded convergence check.

This flow establishes `community_unverified` trust. It does not prove
manufacturer provenance and cannot produce `official_verified`. The six-digit
comparison detects an ordinary key-exchange mismatch, but the dashboards are
still served over local HTTP; users compare the code on independently
opened device pages on a trusted LAN. Attestation and recovery/rotation remain a
later Phase 3D boundary.

Activation remains separate. A paired spatial Node still requires capability,
assignment, pose, and calibration readiness. Pairing credential validity is an
authentication property; quarantined membership remains the authorization gate.
`activate` makes registry membership active only after readiness and replica
checks pass. Reconfiguration and quarantine preserve the credential so bounded
control-plane synchronization can continue. Revoke disables it and forget erases it.
Normal standalone sensing and Home Assistant entities do not depend on this API.
Pairing alone never grants production fusion authority; `fusionEnabled` only
reports the compiled product capability. The NAMO Home Assistant integration may
read the existing top-level local `/api/state` fields for a one-member
standalone Site; this does not grant Group authority or alter Site storage.

The dashboard's disconnect command is bilateral even though `revoke` and
`forget` remain local registry primitives. It resolves or accepts the peer
address, verifies `/api/platform/descriptor` against the selected participant
ID, and removes an existing direct relationship from the peer registry first.
A canonical Site member is not removed through that direct-pair transaction.
The Configuration Owner verifies the target descriptor and shared Site identity,
then sends `POST /api/system/reset` with `connections=true` before changing the
Owner roster. A dropped reset response is never replayed blindly: the dashboard
waits until the target reports a new single-member Site owned by itself. A
definite HTTP reset error is preserved instead of being replaced by a generic
confirmation timeout. If the target cannot be confirmed as standalone, the
Owner roster and credential remain active so the same operation can be retried.

After standalone confirmation, the Owner revokes the managed record. When a
direct record exists, the current firmware rebuilds the canonical roster from
non-revoked direct records; this mutation disables the credential, retains a
revoked direct retry anchor, and makes the canonical member absent. After Owner
handover, a canonical-only record instead remains canonically revoked until the
final forget. In a Site with three or more configured members, the dashboard
confirms the exact authority epoch, generation, roster revision, digest, and the
matching absent or revoked state on a strict majority of the pre-removal roster
before forgetting the retry anchor. A two-member Site uses the Owner's durable
write because no non-target replica remains.

The Owner UI exposes an explicit force-removal action only for a revoked entry.
It forgets the local canonical/direct record without claiming that the target
was reset. The warning must state that stale connection state can remain on the
target and that its device-to-device connections must be reset before pairing
it again. Browser reload and repeated commands derive their next step from the
Owner's canonical membership, revoked direct record, credential state, and the
target's shared or standalone Site state; browser storage is not transaction
authority. For compatibility with an interrupted older removal, a disabled
revoked direct record whose canonical member is already absent resumes by
resetting and confirming the target before forgetting the retry anchor.
Removing the final relationship also queues the RAM-only pairing session back
to `idle` and runs the standalone normalization rules above.

An enrolled non-owner Node exposes one separate **leave Site** operation for
its canonical Configuration Owner relationship. The dashboard verifies the
owner descriptor and Site identity, asks that Owner to revoke and forget the
local Node from the canonical roster, and only after the Owner confirms absence
does it run the existing local `connections` reset transaction. If the Owner
already confirms that the Node is absent, this is treated as stale-membership
recovery and the local reset may proceed. The operation never removes an
arbitrary peer from a non-owner Node and never reports success while the Owner
still lists the Node. Local floorplan payload, radar pose, zones, and tuning are
preserved, while peer credentials, readiness, Site directory, and enrolled Site
membership are reset to a fresh one-member standalone Site so the Node can pair
again.

### 9.6 On-Demand Local Discovery

`GET /api/platform/discovery` returns the latest completed, bounded
`_namo._tcp` query snapshot. `POST /api/platform/discovery` accepts the
form-encoded action `scan` and starts one asynchronous PTR query with a bounded
three-second lifecycle. Accepted requests return HTTP `200` with
`accepted: true`; callers poll GET until `scanning` is false. Repeating `scan`
while that query is active is idempotent and does not leave discovery
permanently busy.

```json
{
  "ok": true,
  "supported": true,
  "scanning": false,
  "generation": 3,
  "result": "ok",
  "candidates": [
    {
      "participantId": "namo-node-peer",
      "participantType": "namo_node",
      "product": "namo-spatial-sensor",
      "address": "192.168.1.50",
      "hostname": "presence-sensor-peer",
      "port": 80,
      "protocolMinimum": 1,
      "protocolMaximum": 1
    }
  ]
}
```

Rules:

- The firmware advertises `_namo._tcp` after its stable identity is ready, but
  advertisement registration is not a prerequisite for starting a local query.
- The component loop, not the HTTP task, starts and polls the asynchronous mDNS
  query. A failed start is retried for a bounded interval, and the scan always
  releases its busy state.
- A user-visible scan may join an endpoint-recovery query that is already in
  progress. The existing query is promoted to visible `scanning` state rather
  than returning an internal `idle` result or starting a second mDNS query.
- Completed results are copied into fixed-size runtime storage before the mDNS
  query and its result list are released. At most 19 remote candidates remain.
- Results are deduplicated by stable `participantId`. The local Node, malformed
  TXT records, incompatible protocol ranges, non-NAMO participant types,
  wrong products, unresolved IPv4 addresses, and identities that are not ready
  are excluded.
- Discovery reports addresses only. It does not open a pairing window, approve
  trust, exchange secrets, enroll a participant, or activate fusion.
- The dashboard may pass a selected candidate address into the existing secure
  pairing flow. Manual address entry remains available when multicast discovery
  is unavailable.
- Each completed query replaces the previous fixed-size RAM snapshot. Results
  are not persisted to flash, and discovery retries are bounded in frequency.

The host-side readiness and admission tests run as part of `npm test`. A complete
device lifecycle can be checked without manual console calls:

```powershell
python tools/participant_readiness_smoke.py --device http://192.168.29.104
```

This stages a temporary Satellite, verifies both activation gates, activates it,
then quarantines and forgets the temporary record. The tool never reboots the
device. Reboot persistence is a separate manual target check after ESPHome has
marked the boot successful. On failure, the tool closes the pairing window,
attempts bounded cleanup, and reports the HTTP status plus structured API error
code.

The authenticated UDP shadow transport is opt-in and must never be shipped. Build
that diagnostic image in the development workspace, then run the same lifecycle
smoke with transport checks:

```powershell
esphome -s shadow_transport_smoke true compile namo.yaml
python tools/participant_readiness_smoke.py --device http://192.168.29.104 --transport
```

`--transport` uses a RAM-only `satellite-smoke-transport` identity and public
test key, generates a fresh boot ID for every invocation, and verifies two
accepted observations, authentication rejection, the two-packets-per-second
limit, and duplicate-sequence rejection. It neither creates a persistent
participant nor reboots the device, so the same boot may run the smoke
repeatedly.

---

## 10. Compatibility and Legacy Migration

Existing response fields remain available until both firmware and dashboard
support the structured replacement.

When converting a legacy endpoint:

1. Add `code`, `severity`, and `detail` beside the existing response.
2. Update mock/demo API to return the same fields.
3. Update frontend to prefer code-based messages.
4. Keep legacy fields until the new path is stable.
5. Remove legacy dependencies only after dashboard and firmware are verified.

---

## 11. Legacy Endpoint Migration Order

For endpoints that still depend on legacy text fields, use this rollout order:

1. Diagnostics/debug APIs
2. Calibration status
3. Floorplan upload status and errors
4. Setup and Wi-Fi handoff status
5. OTA upload and reboot confirmation status
6. `/api/system/status`

This order limits compatibility risk:

- Debug and calibration behavior is comparatively isolated.
- Upload, setup, and OTA flows have longer state transitions and require the
  structured pattern to be verified first.
- `/api/system/status` has the widest client surface and is migrated last.

---

## 12. Configuration Value Reference

Software zone types:

| Value | Meaning |
| --- | --- |
| `detection` | Count targets inside the zone as zone presence. |
| `filter` | Treat targets inside the zone as false-positive candidates and block them. |
| `reduced` | Keep new candidates tentative until PIR evidence appears or the same track leaves the zone. Existing confirmed tracks remain valid. |
| `disabled` | Keep the zone in configuration but do not use it for detection. |
| `exit` | Mark a likely path where a person leaves the monitored area. This is exit evidence, not a filter. |

Calibration zone types:

| Value | Meaning |
| --- | --- |
| `filter` | Block repeated false-positive candidates. |
| `reduced` | Keep new candidates tentative until PIR evidence appears or the same track leaves the zone. Existing confirmed tracks remain valid. |
| `disabled` | Keep the calibration zone in configuration but do not use it. |

`exit` is only a software zone type. It must not be treated as a calibration
zone type and must not remove targets by itself.

When zones overlap, blocking always takes precedence. A software `reduced` zone
then takes precedence over a calibration `reduced` zone so diagnostics retain
the user-selected source while both use the same tentative-admission behavior.

Presence engine flags:

| Field | Default | Meaning |
| --- | --- | --- |
| `legacyPresenceFallback` | `false` | Use the old lambda-based presence path instead of the tracker engine. This is a diagnostic fallback and must be explicitly enabled by the user. |

Older `trackerAssistPresence` values must not disable the tracker engine during
normal operation. Firmware treats the tracker engine as the default unless
`legacyPresenceFallback` is explicitly `true`.

---

## 13. Incompatible Implementation Patterns

The following changes violate this contract:

- Removing or breaking existing response fields without a versioned migration.
- Client logic that depends on display strings.
- Translated user-facing firmware messages in API responses.
- Sentences used as status codes.
- Large logs embedded in normal status responses.
- Endpoint semantic changes bundled with localization work.
- User-facing sentences exchanged between peer devices.
- Multi-sensor coordinate fusion without explicit geometry setup.
- Exit zones interpreted as filter zones.

---

## 14. Default Status Response

Unless an endpoint-specific section defines a different schema, a successful
state response uses this envelope:

```json
{
  "ok": true,
  "status": {
    "code": "feature_state",
    "severity": "info",
    "detail": {}
  }
}
```

Legacy string fields may remain for compatibility. New clients use `code` as
the control value.

---

## 15. LD2410C Presence Assistance Contract

`GET /api/state` exposes bounded LD2410C input and presence-assistance state
under `debug.staticRadar`. The `assist` object contains `armed`, `active`,
`armPending`, `armElapsedMs`, `exitVeto`, and the optional `heldTarget` snapshot.
Clients that display the LD2410C target position must use `presence` together
with the single `detectionDistanceMm` value.
`debug.presenceEvidence` reports PIR, LD2450 tracker, and LD2410C assistance as
independent evidence.

LD2410C may only maintain a presence session already acquired by PIR or a
confirmed LD2450 track. It must not acquire presence by itself and must not
create motion, active target coordinates or counts, room state, zone presence,
or heatmap samples. A current `lost_after_exit` or `lost_after_room_exit` drop
must veto a new static hold for that loss. Missing hardware is represented by
`available: false` and must preserve existing PIR plus LD2450 behavior.

While assistance is active, `assist.heldTarget` may preserve the last coordinate
of exactly one confirmed or coasting LD2450 track for display only. It is `null`
when no unambiguous prior spatial target exists. The snapshot does not make the
target active and consumers must not include it in target counts, zones, rooms,
calibration, or heatmaps.

```json
{
  "heldTarget": {
    "id": "target_1",
    "x": 600,
    "y": 1800,
    "lastDistanceMm": 1897,
    "staticDistanceMm": 1950,
    "distanceDeltaMm": 53,
    "distanceMatched": true
  }
}
```

`staticDistanceMm` and `distanceDeltaMm` are `null` when the stabilized
`detectionDistanceMm` is unavailable or invalid. `distanceMatched` is then
`false`. The dashboard may show a matched snapshot with the normal target
appearance and an unmatched snapshot as position-uncertain.

The replay `tg` tuple records tracker input in this order:

```text
[valid, xMm, yMm, speedCmS, distanceMm, reducedZone, filterBlocked]
```

`filterBlocked=1` preserves a filtered observation for association with an
existing track, but the observation cannot create, update, or publish a track.
Readers must also accept legacy five- and six-item tuples and treat missing
flags as `false`.

The optional replay `sr` tuple records static-radar evidence in this order:

```text
[available, presence, moving, still, movingDistanceMm, stillDistanceMm, movingEnergy, stillEnergy, detectionDistanceMm]
```

Readers must also accept legacy eight-item tuples and treat the missing
`detectionDistanceMm` as `0`.

The optional replay `sf` tuple records fusion state in this order:

```text
[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending, exitVeto, armElapsedMs]
```
