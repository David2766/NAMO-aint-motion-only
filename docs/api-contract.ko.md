# API 계약

이 문서는 기기 API가 상태, 오류, 사용자 표시 문구를 어떤 방식으로 표현해야 하는지
정리한다. 목적은 펌웨어, 대시보드 UI, 데모 모드, 향후 외부 연동이 늘어나도 서로
덜 꼬이게 만드는 것이다.

이 문서는 완전한 OpenAPI 스펙이 아니다. 이 프로젝트에서 실제로 지킬 수 있는
실용적인 API 계약이다.

---

## 1. 적용 범위

이 계약은 우선 상태나 오류 정보를 반환하는 API부터 적용한다. 기존 API를 한 번에
전부 바꾸지 않는다.

초기 적용 대상:

- `/api/system/status`
- `/api/setup/*`
- `/api/state`의 `debug.staticRadar`는 제한된 크기의 LD2410C 입력과 재실 보조 상태를 제공한다.
  `available`, `presence`, `moving`, `still`, 거리, 에너지 및 짧은 `reason`은 센서 입력이며,
  `assist`는 이미 시작된 재실을 유지하는 융합 상태를 제공한다.
- 평면도 업로드 API
- 통계 업로드 및 백업/복원 API
- 보정 관련 API
- OTA 업로드 및 재응답 확인 흐름
- 진단 및 디버그 API

---

## 2. 핵심 원칙

API는 프로그램이 판단할 수 있는 안정적인 코드를 반환하고, 실제 표시 문구는
프론트엔드가 언어 설정에 맞춰 만든다.

권장:

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

새 API에서는 피할 것:

```json
{
  "ok": false,
  "status": "PIR 움직임이 감지되어 시작할 수 없습니다."
}
```

---

## 3. 응답 구조

### 3.1 성공 응답

성공 응답은 반드시 `ok: true`를 사용한다.

권장 형태:

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

규칙:

- `ok`는 반드시 `true`다.
- `data`에는 실제 도메인 데이터를 넣는다.
- 전달할 상태가 없다면 `status`는 생략해도 된다.
- `status`가 있다면 `status.code`는 안정적인 기계 판독용 값이어야 한다.
- 실패 원인을 `status`에 넣지 않는다.

### 3.2 실패 응답

실패 응답은 반드시 `ok: false`를 사용한다.

권장 형태:

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

규칙:

- `ok`는 반드시 `false`다.
- 실패 원인은 `error.code`에 넣는다.
- `error.severity`는 `warning` 또는 `error`를 사용한다.
- `error.detail`에는 작고 구조화된 데이터만 넣는다.
- UI 로직은 `error.message`에 의존하면 안 된다.

### 3.3 진행 중 응답

시간이 걸리거나 나중에 완료되는 작업은 `ok: true`와 status code를 반환한다.

예시:

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

요청은 정상 접수되었지만 완료까지 시간이 필요한 작업, 재부팅 대기, deferred action,
polling 흐름에 사용한다.

---

## 4. HTTP 상태 코드 규칙

HTTP 상태 코드는 요청/전송 레벨의 결과를 나타낸다. JSON의 `code` 값은 기기 내부
또는 도메인 상태를 설명한다.

권장 매핑:

| HTTP | 의미 | 사용 예 |
|---:|---|---|
| 200 | OK | 일반적인 동기 성공 또는 상태 응답 |
| 202 | Accepted | OTA, 재부팅, deferred work처럼 요청을 받았고 나중에 완료되는 경우 |
| 400 | Bad Request | 파라미터 누락, 잘못된 JSON, 잘못된 target, 잘못된 chunk header |
| 401 | Unauthorized | API key 또는 PSK가 필요하지만 없거나 잘못된 경우 |
| 403 | Forbidden | 요청은 이해했지만 현재 기기 상태 때문에 허용되지 않는 경우 |
| 404 | Not Found | 없는 endpoint 또는 resource |
| 409 | Conflict | 세션 충돌, upload mismatch, 이미 진행 중인 작업 |
| 413 | Payload Too Large | 업로드 또는 요청 body 크기 초과 |
| 415 | Unsupported Media Type | 지원하지 않는 `Content-Type` |
| 500 | Internal Server Error | 예상하지 못한 펌웨어, 저장소, 내부 오류 |
| 503 | Service Unavailable | 재부팅 중, 네트워크 전환 중, 일시적으로 준비되지 않은 상태 |

예시:

- 업로드 세션 불일치: HTTP `409`, error code `upload_session_mismatch`
- boot guard 중 초기화 차단: HTTP `403`, error code `boot_guard_active`
- 평면도 이미지 크기 초과: HTTP `413`, error code `upload_payload_too_large`
- SPIFFS 쓰기 실패: HTTP `500`, error code `storage_write_failed`

---

## 5. 상태 및 오류 객체

`code`

- 프로그램이 읽는 안정적인 식별자다.
- 소문자 `snake_case`를 사용한다.
- 문장처럼 만들지 않는다.
- 프론트엔드 로직은 표시 문구가 아니라 이 값을 기준으로 판단한다.

`severity`

- `info`, `warning`, `error` 중 하나를 사용한다.
- UI 강조 수준, 로그, 진단 화면에 활용한다.

`detail`

- 상태 설명에 필요한 구조화 데이터다.
- 펌웨어 메모리 제약이 있으므로 작게 유지한다.
- 숫자, boolean, id, 짧은 enum 문자열을 우선 사용한다.

`message`

- 전환 기간에만 사용하는 선택 필드다.
- 로그나 임시 하위 호환용으로 사용할 수 있다.
- UI 로직은 이 값에 의존하면 안 된다.
- 새 UI 작업은 `code`에서 생성한 i18n 문구를 우선 사용한다.

---

## 6. 코드 이름 규칙

가능하면 기능 영역 prefix를 붙인다.

예시:

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

code 안에 사용자 표시 문장을 넣지 않는다.

나쁜 예:

- `pir_motion_detected_cannot_start_calibration`
- `please_wait_for_home_assistant_connection`

좋은 예:

- `calibration_pir_active`
- `api_client_waiting`

---

## 7. 코드 Registry

이 registry는 일부러 작게 시작한다. 펌웨어, 대시보드, mock 모드, 문서화된 외부 동작에서
실제로 사용하는 코드만 추가한다.

### 7.1 공통 오류 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `invalid_request` | error | 400 | 요청 형태 또는 파라미터가 잘못됨 | `field`, `reason` |
| `unauthorized` | error | 401 | 인증 정보가 없거나 잘못됨 | 없음 |
| `forbidden` | error | 403 | 현재 상태에서 요청이 허용되지 않음 | `reason` |
| `not_found` | error | 404 | endpoint 또는 resource가 없음 | `resource` |
| `conflict` | warning | 409 | 현재 활성 상태와 요청이 충돌함 | `reason` |
| `payload_too_large` | error | 413 | 요청 body가 허용 크기를 초과함 | `maxBytes`, `receivedBytes` |
| `unsupported_media_type` | error | 415 | 지원하지 않는 content type | `contentType` |
| `internal_error` | error | 500 | 예상하지 못한 내부 오류 | `where` |
| `service_unavailable` | warning | 503 | 기기가 일시적으로 사용 불가 | `reason`, `retryAfterMs` |

### 7.1.1 장치 설정 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `config_not_found` | error | 404 | 저장된 장치 설정이 없음 | `target` |
| `config_write_failed` | error | 500 | 장치 설정을 저장하지 못함 | `target`, `where` |

### 7.2 Setup 및 Wi-Fi 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `setup_wifi_connecting` | info | 200 | Wi-Fi 연결 시도 중 | `ssid` |
| `setup_wifi_connected` | info | 200 | Wi-Fi 연결 성공 | `ssid`, `ip` |
| `setup_wifi_failed` | warning | 200 | Wi-Fi 연결 실패 | `ssid`, `reason` |
| `setup_ready` | info | 200 | setup 모드가 사용자 동작을 받을 준비가 됨 | `stage` |
| `setup_scan_started` | info | 200 | Wi-Fi 스캔 시작됨 | 없음 |
| `setup_scan_failed` | error | 500 | Wi-Fi 스캔을 시작하지 못함 | 없음 |
| `setup_finish_pending` | info | 202 | setup finish 요청이 접수되어 적용 중 | 없음 |
| `setup_ap_close_scheduled` | info | 200 | 유예 시간 이후 setup AP 종료 예약됨 | `delayMs` |
| `setup_ap_closing` | info | 202 | setup AP 종료 중 | `delayMs` |
| `ha_handoff_started` | info | 200 | 사용자가 확인한 native API handoff 시작됨 | `waitSeconds` |
| `api_client_waiting` | warning | 200 | Native API client가 아직 연결되지 않음 | `uptimeSeconds` |
| `api_client_connected` | info | 200 | Native API client 연결됨 | 없음 |
| `api_client_idle` | info | 200 | Native API client 경고가 없는 대기 상태 | 없음 |
| `api_key_ready` | info | 200 | API 키가 이미 사용자 키로 준비됨 | 없음 |
| `api_key_rotated` | info | 200 | 기본/demo API 키가 교체됨 | 없음 |
| `api_key_unavailable` | error | 503 | Native API 키 서비스를 사용할 수 없음 | 없음 |
| `api_key_not_set` | error | 404 | Native API 키가 설정되어 있지 않음 | 없음 |
| `api_key_random_failed` | error | 500 | API 키 난수 생성 실패 | 없음 |
| `api_key_save_failed` | error | 500 | API 키 저장 실패 | 없음 |
| `api_key_unsupported` | error | 503 | 이 빌드에서 API 키 작업을 지원하지 않음 | 없음 |
| `wifi_unavailable` | error | 503 | Wi-Fi 서비스를 사용할 수 없음 | 없음 |
| `wifi_not_ready` | warning | 409 | Wi-Fi STA 설정이 아직 준비되지 않음 | 없음 |
| `wifi_unsupported` | error | 503 | 이 빌드에서 Wi-Fi handoff를 지원하지 않음 | 없음 |
| `open_wifi_unsupported` | error | 400 | 오픈 Wi-Fi는 지원하지 않음 | 없음 |
| `invalid_password_length` | error | 400 | Wi-Fi 비밀번호 길이가 WPA 범위를 벗어남 | `min`, `max` |
| `boot_guard_active` | warning | 403 | 초기 boot guard 중 초기화 또는 OTA 차단 | `remainingSeconds` |

### 7.2.1 시스템 관리 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `nothing_selected` | warning | 400 | 초기화할 대상이 선택되지 않음 | `target` |
| `settings_reset_failed` | error | 500 | 설정 초기화 실패 | `target` |
| `stats_reset_failed` | error | 500 | 통계 초기화 실패 | `target` |

### 7.2.2 제어 API 코드

제어 API는 잘못된 요청 필드에 공통 `invalid_request` 코드를 사용한다.
마이그레이션 중에는 기존 legacy error 문자열을 유지하고, `detail`에는 잘못된
`field`와 영향을 받은 `target`을 넣는다.

| Endpoint group | Code | Severity | HTTP | Detail |
|---|---|---|---:|---|
| 제어 설정 | `invalid_request` | error | 400 | `field`, `target` |

시간대 제어는 기존의 크기가 제한된 제어 요청 패턴을 사용한다.

Endpoints:

- `GET /api/control/status`
- `POST /api/control/timezone`
- `GET /api/control/static-radar-tuning`
- `POST /api/control/static-radar-tuning/session`
- `POST /api/control/static-radar-tuning/gate`

Rules:

- `POST /api/control/timezone`은 지원하는 IANA `timezone` 식별자를 가진
  JSON object를 form field `data`로 받는다.
- 실제 시간대가 변경되면 HTTP `200`과 `changed: true`,
  `todayStatsReset: true`를 반환한다. 현재 시간대를 다시 요청해도 HTTP
  `200`을 반환하지만 `changed: false`이며 통계를 초기화하지 않는다.
  변경 적용은 비동기로 진행되며 pending 상태는 status API에서 제공한다.
- 다른 시간대를 적용할 때는 당일 카운터와 당일 히트맵만 초기화한다.
  완료된 과거 일별 기록은 유지해야 한다.
- `GET /api/control/status`는 비동기 기기 적용 완료를 확인할 수 있도록
  `timezone`, `timezoneKnown`, `timezoneApplyPending`을 제공한다.
- 지원하지 않는 시간대 식별자는 `field: timezone`, `target: timezone`을
  담은 표준 `invalid_request` envelope으로 응답한다.

LD2410C 튜닝은 평상시 거리별 계측을 실행하지 않도록 수명이 짧은
engineering 세션을 사용한다.

- `GET /api/control/static-radar-tuning`은 항상 `available`과 `active`를
  반환한다. 비활성 상태에서는 `gates`가 비어 있고, 활성 상태에서는
  G0부터 G8까지의 `startMm`, `endMm`, `sensitivity`, `moveEnergy`,
  `stillEnergy`를 반환한다. 게이트 에너지는 튜닝용 계측값이며 감지된
  타깃 위치로 해석하면 안 된다.
- `POST /api/control/static-radar-tuning/session`은 boolean `active`가
  필요하다. 활성 요청은 keepalive 역할도 하며, 15초 동안 활성
  keepalive가 없으면 펌웨어가 engineering mode를 자동으로 종료해야 한다.
- LD2410C가 없을 때 세션을 시작하면 HTTP `409`와
  `static_radar_unavailable` 코드를 반환한다.
- `POST /api/control/static-radar-tuning/gate`는 `[0, 8]` 범위의 정수
  `gate`와 `[0, 100]` 범위의 정수 `sensitivity`가 필요하다. 제품 UI의
  민감도는 `threshold = 100 - sensitivity`로 변환하여 LD2410C의 이동 및
  정지 threshold에 함께 적용한다.
- gate 변경은 engineering 세션이 활성 상태일 때만 허용한다. 그렇지
  않으면 HTTP `409`와 `static_radar_tuning_inactive` 코드를 반환한다.
- Engineering 계측은 진단 용도일 뿐이며 재실, 움직임, 타깃 좌표와 수,
  구역, 방 또는 히트맵을 변경하면 안 된다.

### 7.3 업로드 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `upload_started` | info | 200 | 업로드 세션 시작 | `target`, `session`, `size` |
| `upload_committed` | info | 200 | 업로드 commit 성공 | `target`, `session`, `size`, `hash` |
| `upload_session_mismatch` | error | 409 | chunk가 활성 업로드 세션과 맞지 않음 | `expected`, `received` |
| `upload_offset_mismatch` | error | 409 | chunk offset이 예상 offset과 맞지 않음 | `expected`, `received` |
| `upload_incomplete` | error | 409 | 모든 byte를 받기 전에 commit이 요청됨 | `expected`, `received` |
| `upload_payload_too_large` | error | 413 | 업로드 payload 크기 초과 | `maxBytes`, `receivedBytes` |
| `upload_storage_failed` | error | 500 | 업로드 write 또는 commit 실패 | `target`, `where` |

### 7.3.1 기기 설정 업로드

기기 설정 저장은 floorplan 및 stats 데이터와 같은 chunked upload 패턴을 사용한다.
zone, floorplan, future multi-sensor 설정은 안전한 단일 form 요청 크기를 넘을 수 있다.

Endpoints:

- `POST /api/config/upload/start`
- `POST /api/config/upload/chunk`
- `POST /api/config/upload/commit`

Rules:

- `start`에는 `session`과 `size`가 필요하다.
- `chunk`에는 `session`, `offset`, hex-encoded `data`가 필요하다.
- `commit`에는 `session`이 필요하다.
- `commit`은 성공 응답을 반환하기 전에 저장된 device config를 firmware cache에 다시 로드해야 한다.
- `POST /api/config`, `POST /api/stats`, `POST /api/floorplan`,
  `POST /api/floorplan/image` 같은 단일 form legacy write는 embedded API에서 제거한다.
  대시보드와 복원 흐름은 chunked upload endpoint를 사용해야 한다.

### 7.3.2 평면도 부분 저장

작은 평면도 편집은 전체 평면도 문서를 다시 업로드하지 않고 전용 단일 form patch endpoint를 사용할 수 있다.
이 endpoint들은 크기가 제한된 저메모리 필드에만 사용하며, 이미지 업로드, 방 geometry 전체 교체,
wall segment 전체 교체, object 목록 전체 교체, 복원 flow에는 사용하지 않는다.

Endpoints:

- `POST /api/floorplan/radar`
- `POST /api/floorplan/room-name`
- `POST /api/floorplan/occlusion`
- `POST /api/floorplan/objects`

Rules:

- `POST /api/floorplan/radar`는 `originPx`, `rotationDeg`, `scale`을 가진 JSON object를 form field `data`로 받는다.
- `POST /api/floorplan/room-name`은 form field `id`, `name`을 받는다.
- `POST /api/floorplan/occlusion`은 `ignoredEdges`를 가진 JSON object를 form field `data`로 받는다.
- `POST /api/floorplan/objects`는 평면도 가구 object JSON array를 form field `data`로 받는다.
- 기기는 저장된 floorplan config payload의 해당 필드만 갱신하고 raw-partition metadata를 authoritative source로 유지한다.
- patch 대상이 없거나, 잘못되었거나, 너무 크면 표준 error envelope으로 `invalid_request`, `not_found`,
  또는 `upload_storage_failed`를 반환한다.
- 전체 평면도 문서 저장, 최초 저장, 복원 flow, 방 geometry 편집, scale 편집, wall segment 편집, 이미지 저장은 계속
  `POST /api/floorplan/upload/start|chunk|commit`을 사용해야 한다.

### 7.4 보정 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `calibration_ready` | info | 200 | 보정 시작 가능 | 없음 |
| `calibration_started` | info | 200 | 보정 실행 시작됨 | 없음 |
| `calibration_running` | info | 200 | 보정 진행 중 | `samples`, `zone` |
| `calibration_pir_active` | warning | 409 | PIR 움직임 때문에 보정 시작 불가 | `pir` |
| `calibration_no_target` | warning | 409 | 감지된 타겟이 없어 보정 시작 불가 | 없음 |
| `calibration_multiple_targets` | warning | 409 | 타겟이 여러 개 감지되어 보정 시작 불가 | `targetCount` |
| `calibration_max_zones_reached` | warning | 409 | 더 이상 보정 구역을 만들 수 없음 | `maxZones` |
| `calibration_timeout` | error | 409 | 안정적인 보정 후보 없이 보정 종료 | `samples`, `score` |
| `calibration_candidate_failed` | error | 500 | 보정 구역 후보 생성 실패 | `samples`, `score` |
| `calibration_saved` | info | 200 | 보정 구역 저장됨 | `zone`, `count` |

### 7.5 OTA 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `ota_uploading` | info | 200 | OTA 업로드 진행 중 | `bytesWritten`, `totalBytes` |
| `ota_reboot_waiting` | info | 202 | OTA 업로드 완료 후 재부팅 대기 | `retryAfterMs` |
| `ota_device_reappeared` | info | 200 | OTA 또는 재부팅 후 기기가 다시 응답함 | `version`, `buildTime` |
| `ota_verify_timeout` | warning | 503 | 제한 시간 안에 기기 응답 확인 실패 | `timeoutMs` |

### 7.6 디버그 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `debug_snapshot_ready` | info | 200 | 디버그 snapshot 사용 가능 | `timestamp` |
| `debug_events_ready` | info | 200 | 진단 이벤트 로그 사용 가능 | `count`, `truncated`, `format` |
| `debug_events_empty` | info | 200 | 진단 이벤트 로그가 비어 있음 | `format` |
| `debug_events_unavailable` | warning | 503 | 진단 이벤트 로그를 일시적으로 사용할 수 없음 | `resource` |
| `debug_replay_ready` | info | 200 | 재실 리플레이 로그 사용 가능 | `count`, `truncated`, `format` |
| `debug_replay_empty` | info | 200 | 재실 리플레이 로그가 비어 있음 | `format` |
| `debug_replay_unavailable` | warning | 503 | 재실 리플레이 로그를 일시적으로 사용할 수 없음 | `resource` |

---

## 8. 프론트엔드 규칙

대시보드는 `status.code` 또는 `error.code`를 우선 사용하고, 이를 i18n 문구로
매핑한다.

예시:

```ts
const code = response.error?.code ?? response.status?.code ?? "unknown";
const text = messages.status[code] ?? messages.status.unknown;
```

Fallback 규칙:

1. `code`가 있으면 우선 사용한다.
2. `code`가 없으면 기존 legacy 필드를 사용한다.
3. 둘 다 없으면 일반적인 오류 문구를 표시한다.

프론트엔드는 한국어 또는 영어 문장을 파싱해서 동작을 판단하면 안 된다.

---

## 9. 펌웨어 규칙

펌웨어는 짧은 상태 코드와 detail을 제공한다. 임시 호환이 필요한 경우를 제외하면
API 응답에 사용자 표시용 문장을 계속 추가하지 않는다.

일반 API에 큰 진단 payload를 넣지 않는다. 자세한 진단 정보가 필요하면 별도의
debug endpoint를 사용한다.

많은 record를 반환할 수 있는 debug endpoint는 firmware memory 안에서 하나의 큰
문자열을 만들지 말고 streaming 또는 chunk 응답으로 내보낸다.

리플레이 진단 정보는 전용 debug endpoint를 통해 newline-delimited sample로
제공할 수 있다. 리플레이 payload는 오프라인 테스트용이며 일반 status 응답에
넣지 않는다.

리플레이 진단에는 시간이 지나면서 작은 선택 tuple이 추가될 수 있다. 소비자는 알 수 없는
필드를 무시해야 하며, 선택 필드가 없을 때는 안전한 기본값을 사용해야 한다.
예를 들어 `ex` tuple은 exit zone 근거를
`[exitActive, exitZoneMask, exitTargetCount, exitLastSeenAgeMs]` 형태로 보고한다.

---

## 10. 호환성 정책

기존 필드는 즉시 제거하지 않는다.

마이그레이션은 다음 순서를 따른다.

1. 기존 응답 옆에 `code`, `severity`, `detail`을 추가한다.
2. mock/demo API도 같은 필드를 반환하게 한다.
3. 프론트엔드는 code 기반 문구를 우선 사용하게 바꾼다.
4. 새 경로가 안정화될 때까지 legacy 필드를 유지한다.
5. 대시보드와 펌웨어 검증 후 legacy 의존을 제거한다.

---

## 11. 전환 우선순위

권장 순서:

1. 진단 및 디버그 API
2. 보정 상태
3. 평면도 업로드 상태 및 오류
4. setup 및 Wi-Fi handoff 상태
5. OTA 업로드 및 재응답 확인 상태
6. `/api/system/status`

이유:

- 디버그와 보정은 비교적 독립적이라 먼저 적용하기 쉽다.
- 업로드, setup, OTA 흐름은 민감하므로 패턴 검증 후 바꾼다.
- `/api/system/status`는 여러 곳에서 쓰이므로 신중하게 바꾼다.

---

## 12. 설정 Enum

Software zone type:

| 값 | 의미 |
| --- | --- |
| `detection` | 구역 안의 target을 zone presence로 계산한다. |
| `filter` | 구역 안의 target을 오탐 후보로 보고 차단한다. |
| `disabled` | 구역을 설정에는 보존하지만 감지에는 사용하지 않는다. |
| `exit` | 사람이 감지 영역을 나가는 경로 후보를 표시한다. 이것은 exit evidence이며 filter가 아니다. |

Calibration zone type:

| 값 | 의미 |
| --- | --- |
| `filter` | 반복되는 오탐 후보를 차단한다. |
| `reduced` | 반복 후보를 바로 막지 않고 일정 시간 지연 후 통과시킨다. |
| `disabled` | 보정 구역을 설정에는 보존하지만 사용하지 않는다. |

`exit`은 software zone type 전용이다. calibration zone type으로 취급하면 안 되며
그 자체로 target을 제거하면 안 된다.

---

## 13. 금지 사항

하지 말 것:

- 기존 API 응답을 한 번에 크게 깨지 말 것.
- 표시 문자열에 의존하는 프론트 로직을 새로 만들지 말 것.
- 펌웨어 API 문구를 직접 번역해서 내려주지 말 것.
- 긴 문장을 status code처럼 쓰지 말 것.
- 일반 status 응답에 큰 로그를 넣지 말 것.
- 다국어 작업 중 endpoint 의미를 같이 바꾸지 말 것.
- peer 기기끼리 사용자 표시 문장을 주고받게 만들지 말 것.
- 명시적인 geometry 설정 없이 여러 센서의 좌표를 합치지 말 것.
- exit zone을 filter zone처럼 사용하지 말 것.

---

## 14. 이 프로젝트의 실무 규칙

당분간 새 상태 응답을 만드는 작업은 아래 규칙을 따른다.

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

기존 문자열 필드는 남겨도 되지만, 새 UI 작업은 `code`를 우선 사용한다.

---

## 15. LD2410C 재실 보조 규약

`GET /api/state`의 `debug.staticRadar`는 LD2410C 입력과 재실 보조 상태를 제공한다.
`detectionDistanceMm`은 LD2410C가 보고한 단일 최종 감지 거리에 최근
3개 중앙값 필터를 적용한 값이다. 1초에 최대 한 개의 거리만 중앙값 이력에
넣으며, 짧은 유효하지 않은 거리값은 마지막 안정값을 덮어쓰지 않는다.
보조 레이더 재실이 해제되거나 센서가 없어지면 필터 상태를 초기화한다.
`movingDistanceMm`과 `stillDistanceMm`은 진단용 원시값이며 클라이언트가
두 값을 조합해 타깃 위치를 추론하면 안 된다.
`assist` 객체는 `armed`, `active`, `armPending`, `armElapsedMs`, `exitVeto`와 선택적인
`heldTarget` 스냅샷을 포함한다.
LD2410C 타깃 위치를 표시하는 클라이언트는 `presence`와 단일
`detectionDistanceMm`만 사용해야 한다.
`debug.presenceEvidence`는 PIR, LD2450 트래커, LD2410C 보조 증거를 각각 표시한다.

LD2410C는 PIR 또는 확정된 LD2450 트랙으로 이미 시작된 재실 세션만 유지할 수 있다.
LD2410C 단독으로 재실을 새로 켜면 안 되며 움직임, 활성 타깃 좌표와 개수, 방 상태,
구역 재실 또는 히트맵 샘플을 만들면 안 된다.
현재 drop 이벤트가 `lost_after_exit` 또는 `lost_after_room_exit`이면 해당 소실에 대해 새로운 정적 레이더 hold를 시작하지 않는다.
LD2410C가 없거나 통신할 수 없으면 `available: false`로 처리하고 기존 PIR 및 LD2450 동작을 그대로 유지한다.

보조 유지가 활성화된 동안 `assist.heldTarget`은 정확히 하나였던 마지막 확정 또는 코스팅
LD2450 트랙의 좌표를 표시 목적으로만 보존할 수 있다. 명확한 이전 공간 타깃이 없으면
`null`이다. 이 스냅샷은 활성 타깃이 아니며 타깃 수, 구역, 방, 보정 또는 히트맵 계산에
포함하면 안 된다.

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

안정화된 `detectionDistanceMm`을 사용할 수 없거나 값이 유효하지 않으면
`staticDistanceMm`과 `distanceDeltaMm`은 `null`이고 `distanceMatched`는 `false`다. 대시보드는 거리 일치
스냅샷을 기존 타깃 모습으로, 불일치 스냅샷을 위치 불확실 상태로 표시할 수 있다.

리플레이의 선택적 `sr` 튜플은 다음 순서로 정적 레이더 근거를 기록한다.

```text
[available, presence, moving, still, movingDistanceMm, stillDistanceMm, movingEnergy, stillEnergy, detectionDistanceMm]
```

리더는 기존 8개짜리 튜플도 받아야 하며 빠진 `detectionDistanceMm`은 `0`으로
처리한다.

리플레이의 선택적 `sf` 튜플은 다음 순서로 융합 상태를 기록한다.

```text
[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending, exitVeto, armElapsedMs]
```
