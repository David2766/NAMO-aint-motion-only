# API Contract

This document defines how device APIs should report machine-readable status,
errors, and user-facing messages. The goal is to keep firmware, dashboard UI,
demo mode, and future integrations stable as the project grows.

This is not a full OpenAPI specification. It is a practical contract for this
project.

---

## 1. Scope

This contract applies first to APIs that return status or error information.
Existing APIs do not need to be rewritten all at once.

Initial targets:

- `/api/system/status`
- `/api/setup/*`
- Device config upload APIs
- Floorplan upload APIs
- Stats upload and backup/restore APIs
- Calibration-related APIs
- OTA upload and reboot confirmation flow
- Diagnostic/debug APIs

---

## 2. Core Rule

APIs should return stable codes for software and let the frontend decide the
display language.

Do this:

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

Avoid this for new APIs:

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

Recommended shape:

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

Recommended shape:

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
- `error.detail` should contain only small structured data.
- UI logic must not depend on `error.message`.

### 3.3 In-Progress Response

Long-running or deferred operations should return `ok: true` with a status code.

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

Recommended mapping:

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
- Boot guard blocks reset: HTTP `403`, error code `boot_guard_active`.
- Oversized floorplan image: HTTP `413`, error code `upload_payload_too_large`.
- SPIFFS write failure: HTTP `500`, error code `storage_write_failed`.

---

## 5. Status and Error Object

`code`

- Stable machine-readable identifier.
- Use lowercase `snake_case`.
- Do not use a full sentence.
- Frontend logic should depend on this, not on display text.

`severity`

- One of `info`, `warning`, or `error`.
- Used for UI tone, logs, and diagnostics.

`detail`

- Structured data needed to explain the state.
- Keep it small because firmware memory is limited.
- Prefer numbers, booleans, ids, and short enum strings.

`message`

- Optional transitional field.
- May be used for logs or temporary backward compatibility.
- UI logic must not depend on this field.
- New UI work should prefer i18n text generated from `code`.

---

## 6. Code Naming Rules

Use domain prefixes when possible.

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

Do not encode UI text into the code.

Bad:

- `pir_motion_detected_cannot_start_calibration`
- `please_wait_for_home_assistant_connection`

Better:

- `calibration_pir_active`
- `api_client_waiting`

---

## 7. Code Registry

This registry starts small on purpose. Add codes only when they are used by
firmware, dashboard, mock mode, or documented external behavior.

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
| `ha_handoff_started` | info | 200 | User-confirmed native API handoff has started. | `waitSeconds` |
| `api_client_waiting` | warning | 200 | Native API client has not connected yet. | `uptimeSeconds` |
| `api_client_connected` | info | 200 | Native API client is connected. | none |
| `api_client_idle` | info | 200 | Native API has no active client warning. | none |
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
| `boot_guard_active` | warning | 403 | Reset or OTA is blocked during initial boot guard. | `remainingSeconds` |

### 7.2.1 System Management Codes

| Code | Severity | HTTP | Meaning | Detail |
|---|---|---:|---|---|
| `nothing_selected` | warning | 400 | Reset request did not select any reset target. | `target` |
| `settings_reset_failed` | error | 500 | Settings reset failed. | `target` |
| `stats_reset_failed` | error | 500 | Statistics reset failed. | `target` |

### 7.2.2 Control API Codes

Control endpoints should use the common `invalid_request` code for invalid
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

- `GET /api/control/static-radar-tuning` always returns `available` and
  `active`. While inactive, `gates` is empty. While active, `gates` contains
  G0 through G8 with `startMm`, `endMm`, `sensitivity`, `moveEnergy`,
  and `stillEnergy`. Gate energy is tuning telemetry and must not be treated
  as a detected target position.
- `POST /api/control/static-radar-tuning/session` requires boolean `active`.
  An active request is also a keepalive. The firmware must leave engineering
  mode automatically when no active keepalive arrives for 15 seconds.
- Starting a session when LD2410C is unavailable returns HTTP `409` with code
  `static_radar_unavailable`.
- `POST /api/control/static-radar-tuning/gate` requires integer `gate` in
  `[0, 8]` and integer `sensitivity` in `[0, 100]`. The product-level
  sensitivity is translated to both LD2410C move and still thresholds as
  `threshold = 100 - sensitivity`.
- Gate writes are accepted only while the engineering session is active.
  Otherwise the endpoint returns HTTP `409` with code
  `static_radar_tuning_inactive`.
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

Device configuration writes should use the same chunked upload pattern as
floorplan and stats data because zone, floorplan, and future multi-sensor
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
  `originPx`, `rotationDeg`, and `scale`.
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
| `ota_reboot_waiting` | info | 202 | OTA upload completed and device is expected to reboot. | `retryAfterMs` |
| `ota_device_reappeared` | info | 200 | Device responded after OTA/reboot. | `version`, `buildTime` |
| `ota_verify_timeout` | warning | 503 | Device did not respond before timeout. | `timeoutMs` |

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

## 8. Frontend Rules

The dashboard should prefer `status.code` or `error.code` and map it to i18n
messages.

Example:

```ts
const code = response.error?.code ?? response.status?.code ?? "unknown";
const text = messages.status[code] ?? messages.status.unknown;
```

Fallback policy:

1. Use `code` if present.
2. If `code` is missing, use the existing legacy field.
3. If no known field is available, show a generic message.

The frontend should not parse Korean or English sentences to decide behavior.

---

## 9. Firmware Rules

Firmware should provide compact status codes and details. It should avoid adding
more user-facing sentences to API responses unless needed for temporary
compatibility.

Firmware should avoid large diagnostic payloads in normal endpoints. Use a
dedicated debug endpoint when richer diagnostics are needed.

Debug endpoints that can return many records should stream or chunk their
responses instead of building one large string in firmware memory.

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

---

## 10. Compatibility Policy

Existing fields must not be removed immediately.

Migration should follow this pattern:

1. Add `code`, `severity`, and `detail` beside the existing response.
2. Update mock/demo API to return the same fields.
3. Update frontend to prefer code-based messages.
4. Keep legacy fields until the new path is stable.
5. Remove legacy dependencies only after dashboard and firmware are verified.

---

## 11. Migration Priority

Recommended order:

1. Diagnostics/debug APIs
2. Calibration status
3. Floorplan upload status and errors
4. Setup and Wi-Fi handoff status
5. OTA upload and reboot confirmation status
6. `/api/system/status`

Reason:

- Debug and calibration are easiest to isolate.
- Upload/setup/OTA flows are more sensitive and should be changed after the
  pattern is proven.
- `/api/system/status` is widely consumed, so it should be changed carefully.

---

## 12. Configuration Enums

Software zone types:

| Value | Meaning |
| --- | --- |
| `detection` | Count targets inside the zone as zone presence. |
| `filter` | Treat targets inside the zone as false-positive candidates and block them. |
| `disabled` | Keep the zone in configuration but do not use it for detection. |
| `exit` | Mark a likely path where a person leaves the monitored area. This is exit evidence, not a filter. |

Calibration zone types:

| Value | Meaning |
| --- | --- |
| `filter` | Block repeated false-positive candidates. |
| `reduced` | Delay filtering before accepting a repeated candidate. |
| `disabled` | Keep the calibration zone in configuration but do not use it. |

`exit` is only a software zone type. It must not be treated as a calibration
zone type and must not remove targets by itself.

Presence engine flags:

| Field | Default | Meaning |
| --- | --- | --- |
| `legacyPresenceFallback` | `false` | Use the old lambda-based presence path instead of the tracker engine. This is a diagnostic fallback and must be explicitly enabled by the user. |

Older `trackerAssistPresence` values must not disable the tracker engine during
normal operation. Firmware should treat the tracker engine as the default unless
`legacyPresenceFallback` is explicitly `true`.

---

## 13. Do Not

Do not:

- Break existing API responses in one large change.
- Add new frontend logic that depends on display strings.
- Translate firmware messages directly in the API.
- Use long sentences as status codes.
- Put large logs into normal status responses.
- Change endpoint semantics while doing i18n work.
- Make peer devices exchange user-facing sentences.
- Merge coordinates from multiple sensors without explicit geometry setup.
- Use exit zones as filter zones.

---

## 14. Practical Rule for This Project

For now, new status-producing work should follow this rule:

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

Legacy string fields may remain, but new UI work should use `code` first.

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
