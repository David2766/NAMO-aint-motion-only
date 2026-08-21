# API 계약

이 문서는 NAMO 펌웨어, 내장 대시보드, 데모 및 mock 구현, 외부 연동이 공유하는
HTTP 인터페이스를 정의합니다. 공통 응답 구조, 상태 및 오류 코드, endpoint 동작,
호환성, 기기 상태와 사용자 표시 문구의 경계를 다룹니다.

펌웨어, 프론트엔드, 외부 연동 및 테스트 개발자가 함께 참고하는 규약입니다. 자동 생성된
OpenAPI 명세는 아니며, 2장부터 6장까지는 공통 응답 규칙, 7장은 코드 registry,
8장과 9장은 endpoint별 계약을 설명합니다. 필드나 값이 필수라고 명시된 경우를 제외하면
예시는 응답 구조를 설명하기 위한 것입니다.

빠른 탐색:

- [공통 응답 규칙](#2-핵심-원칙)
- [상태 및 오류 코드 목록](#7-상태-및-오류-코드-목록)
- [클라이언트 응답 처리](#8-클라이언트-응답-처리)
- [펌웨어 및 endpoint 요구사항](#9-펌웨어-응답-요구사항)
- [호환성과 설정값](#10-호환성과-기존-응답-전환)
- [LD2410C 재실 보조](#15-ld2410c-재실-보조-규약)

규약 본문에서 **반드시**와 **하면 안 됩니다**라는 표현은 호환성을 위해 지켜야 하는
요구사항을 뜻합니다. **할 수 있습니다**라는 표현은 선택 가능한 동작을 뜻합니다.

---

## 1. 적용 범위

이 계약은 기기 상태를 제공하거나, 설정 및 제어 요청을 받거나, 클라이언트에 오류를
보고하는 API에 적용합니다.

적용 대상에는 다음 인터페이스가 포함됩니다.

- `/api/system/status`
- `/api/platform/descriptor`
- `/api/platform/participants`
- `/api/platform/groups`
- `/api/platform/site-configuration`
- `/api/setup/*`
- 기기 설정 업로드 API
- `/api/state`의 `debug.staticRadar`는 제한된 크기의 LD2410C 입력과 재실 보조 상태를 제공합니다.
  `available`, `presence`, `moving`, `still`, 거리, 에너지 및 짧은 `reason`은 센서 입력이며,
  `assist`는 이미 시작된 재실을 유지하는 융합 상태를 제공합니다.
- 평면도 업로드 API
- 통계 업로드 및 백업/복원 API
- 보정 관련 API
- OTA 업로드 및 재응답 확인 흐름
- 진단 및 디버그 API

이미 구현된 분산 공간 플랫폼 동작은 아래 endpoint별 절에서 설명합니다. 아직 구현하지 않은
cluster 및 개방형 Satellite 인터페이스는 이 계약에 포함하지 않으며, 해당 설계는
[공간 플랫폼 아키텍처](platform/architecture.ko.md)와
[Satellite 규약](platform/satellite-protocol.ko.md)에서 확인할 수 있습니다. 새 endpoint는
펌웨어 동작과 관련 테스트가 함께 추가된 시점부터 이 호환성 계약에 포함됩니다.

---

## 2. 핵심 원칙

API 응답은 프로그램이 판단할 수 있는 안정적인 코드를 사용합니다. 클라이언트는 이 코드를
사용자의 언어에 맞는 표시 문구로 변환합니다.

계약에 맞는 예시:

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

새 API에서 허용하지 않는 예시:

```json
{
  "ok": false,
  "status": "PIR 움직임이 감지되어 시작할 수 없습니다."
}
```

---

## 3. 응답 구조

### 3.1 성공 응답

성공 응답은 반드시 `ok: true`를 사용합니다.

표준 형태:

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

- `ok`는 반드시 `true`입니다.
- `data`에는 실제 도메인 데이터를 넣습니다.
- 전달할 상태가 없다면 `status`는 생략해도 됩니다.
- `status`가 있다면 `status.code`는 안정적인 기계 판독용 값이어야 합니다.
- 실패 원인을 `status`에 넣지 않습니다.

### 3.2 실패 응답

실패 응답은 반드시 `ok: false`를 사용합니다.

표준 형태:

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

- `ok`는 반드시 `false`입니다.
- 실패 원인은 `error.code`에 넣습니다.
- `error.severity`는 `warning` 또는 `error`를 사용합니다.
- `error.detail`에는 작고 구조화된 데이터만 넣습니다.
- UI 로직은 `error.message`에 의존하면 안 됩니다.

### 3.3 진행 중 응답

시간이 걸리거나 나중에 완료되는 작업은 `ok: true`와 status code를 반환합니다.

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
polling 흐름에 사용합니다.

---

## 4. HTTP 상태 코드 규칙

HTTP 상태 코드는 요청/전송 레벨의 결과를 나타냅니다. JSON의 `code` 값은 기기 내부
또는 도메인 상태를 설명합니다.

표준 매핑:

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
- boot guard 중 재부팅을 일으키는 작업 차단: HTTP `403`, error code `boot_guard_active`
- 평면도 이미지 크기 초과: HTTP `413`, error code `upload_payload_too_large`
- SPIFFS 쓰기 실패: HTTP `500`, error code `storage_write_failed`

---

## 5. 상태 및 오류 객체

`code`

- 클라이언트 로직이 사용하는 안정적인 식별자입니다.
- 번역 문구나 문장이 아닌 소문자 `snake_case` 값을 사용합니다.

`severity`

- `info`, `warning`, `error` 중 하나를 사용합니다.
- UI 강조 수준, 로그, 진단 화면에 활용합니다.

`detail`

- 상태 설명에 필요한 구조화 데이터입니다.
- 펌웨어 메모리 제약이 있으므로 작게 유지합니다.
- 숫자, boolean, id, 짧은 enum 문자열을 우선 사용합니다.

`message`

- 로그 또는 구형 클라이언트 호환을 위한 선택 필드입니다.
- 제어용 필드가 아닙니다. 클라이언트 동작과 새 UI 문구는 `code`를 기준으로 결정합니다.

---

## 6. 코드 이름 규칙

사용 가능한 기능 영역 prefix를 붙입니다.

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

코드는 상태를 식별하며 사용자 표시 문장을 포함하지 않습니다.

허용하지 않는 이름:

- `pir_motion_detected_cannot_start_calibration`
- `please_wait_for_home_assistant_connection`

허용하는 이름:

- `calibration_pir_active`
- `api_client_waiting`

---

## 7. 상태 및 오류 코드 목록

아래 코드는 호환성 계약에 포함됩니다. 펌웨어 또는 문서화된 mock 구현이 반환하거나,
대시보드 또는 외부 인터페이스가 사용하는 코드만 목록에 추가합니다.

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
| `ha_handoff_started` | info | 200 | 사용자가 확인한 전환을 접수했으며 ESPHome Wi-Fi 소유권으로 재부팅 예정 | `waitSeconds`, `rebootExpected` |
| `api_client_waiting` | info | 200 | Native API는 준비됐지만 client가 아직 연결되지 않음. Wi-Fi 장애가 아님 | 없음 |
| `api_client_connected` | info | 200 | Native API client 연결됨 | 없음 |
| `api_client_idle` | info | 200 | Native API client 경고가 없는 대기 상태 | 없음 |
| `api_service_not_ready` | info | 200 | ESPHome Wi-Fi 또는 Native API 서비스가 아직 준비되지 않음 | 없음 |
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
| `boot_guard_active` | warning | 403 | 초기 boot guard 중 재부팅, 재시작이 필요한 초기화 또는 OTA 차단 | `remainingSeconds` |

### 7.2.1 시스템 관리 코드

| Code | Severity | HTTP | 의미 | Detail |
|---|---|---:|---|---|
| `nothing_selected` | warning | 400 | 초기화할 대상이 선택되지 않음 | `target` |
| `space_reset_failed` | error | 500 | 평면도와 배치 초기화 실패 | `target` |
| `detection_reset_failed` | error | 500 | 감지 설정 초기화 실패 | `target` |
| `connections_reset_failed` | error | 500 | 연결된 기기 정보 초기화 실패 | `target` |
| `stats_reset_failed` | error | 500 | 통계 초기화 실패 | `target` |

`GET /api/system/api-key`는 현재 사용 중인 Native API 키와
`visibleSeconds: 30`을 반환합니다. `POST /api/system/api-key/rotate`는 ESPHome의
Native API 서비스를 통해 새 키를 생성하고 저장한 뒤 그 키를 한 번만
`visibleSeconds: 30`과 함께 반환합니다. 기존 키를 사용하는 연결 종료도
ESPHome이 처리합니다. 시스템 초기화는 API 키를 표시하거나 삭제하거나
교체하지 않습니다.

`POST /api/system/reset`은 서로 독립적인 다섯 개의 boolean 필드를 받습니다.

| 필드 | 초기화 범위 |
|---|---|
| `space` | 평면도 이미지, 방 구조, 가구 배치, 현재 기기의 레이더 위치 |
| `detection` | 사용자·보정 영역, 환경 보정과 일반 설정 |
| `connections` | 연결된 기기 목록, 기기 사이의 보안 연결, 준비·보정 정보, 자동 대체, 공유 공간 상태. 고정 Node 식별 정보는 유지 |
| `stats` | 통계와 히트맵 기록 |
| `wifi` | 저장된 Wi-Fi 정보 |

기존 `settings` 필드는 호환성을 위해 계속 받으며 `space`와 `detection`을
함께 선택한 것으로 처리합니다. 새 클라이언트는 다섯 개의 명시적 필드를
사용해야 합니다.

### 7.2.2 제어 API 코드

제어 API는 잘못된 요청 필드에 공통 `invalid_request` 코드를 사용합니다.
마이그레이션 중에는 기존 legacy error 문자열을 유지하고, `detail`에는 잘못된
`field`와 영향을 받은 `target`을 넣습니다.

| Endpoint group | Code | Severity | HTTP | Detail |
|---|---|---|---:|---|
| 제어 설정 | `invalid_request` | error | 400 | `field`, `target` |

시간대 제어는 기존의 크기가 제한된 제어 요청 패턴을 사용합니다.

Endpoints:

- `GET /api/control/status`
- `POST /api/control/timezone`
- `GET /api/control/static-radar-tuning`
- `POST /api/control/static-radar-tuning/session`
- `POST /api/control/static-radar-tuning/gate`

Rules:

- `POST /api/control/timezone`은 지원하는 IANA `timezone` 식별자를 가진
  JSON object를 form field `data`로 받습니다.
- 실제 시간대가 변경되면 HTTP `200`과 `changed: true`,
  `todayStatsReset: true`를 반환합니다. 현재 시간대를 다시 요청해도 HTTP
  `200`을 반환하지만 `changed: false`이며 통계를 초기화하지 않습니다.
  변경 적용은 비동기로 진행되며 pending 상태는 status API에서 제공합니다.
- 다른 시간대를 적용할 때는 당일 카운터와 당일 히트맵만 초기화합니다.
  완료된 과거 일별 기록은 유지해야 합니다.
- `GET /api/control/status`는 비동기 기기 적용 완료를 확인할 수 있도록
  `timezone`, `timezoneKnown`, `timezoneApplyPending`을 제공합니다.
- 지원하지 않는 시간대 식별자는 `field: timezone`, `target: timezone`을
  담은 표준 `invalid_request` envelope으로 응답합니다.

LD2410C 튜닝은 평상시 거리별 계측을 실행하지 않도록 수명이 짧은
engineering 세션을 사용합니다.

- `GET /api/control/static-radar-tuning`은 항상 `available`, `recovering`, `active`,
  `phase`, `result`를 반환합니다. `phase`는 `idle`, `starting`, `active`,
  `stopping`, `failed` 중 하나이며, 실제 engineering 주기 프레임을 받은
  뒤에만 `active`가 true가 됩니다. 비활성 상태에서는 `gates`가 비어 있고, 활성 상태에서는
  G0부터 G8까지의 `startMm`, `endMm`, `sensitivity`, `moveEnergy`,
  `stillEnergy`를 반환합니다. 게이트 에너지는 튜닝용 계측값이며 감지된
  타깃 위치로 해석하면 안 됩니다. 응답에는 최근 게이트 작업의 `id`,
  `state`, `result`, 요청한 게이트와 민감도, 모듈에서 다시 읽은 이동 및
  정지 threshold를 담은 `operation`도 포함됩니다.
- `POST /api/control/static-radar-tuning/session`은 boolean `active`가
  필요합니다. 활성 요청은 keepalive 역할도 하며, 15초 동안 활성
  keepalive가 없으면 펌웨어가 engineering mode를 자동으로 종료해야 합니다.
- LD2410C가 없을 때 세션을 시작하면 HTTP `409`와
  `static_radar_unavailable` 코드를 반환합니다.
- 명령 복구 중 들어온 세션 및 게이트 요청은 HTTP `409`와
  `static_radar_recovering`으로 거부하며 나중에 다시 실행하도록 저장하지 않습니다.
- `POST /api/control/static-radar-tuning/gate`는 `[0, 8]` 범위의 정수
  `gate`와 `[0, 100]` 범위의 정수 `sensitivity`가 필요합니다. 제품 UI의
  민감도는 `threshold = 100 - sensitivity`로 변환하여 LD2410C의 이동 및
  정지 threshold에 함께 적용합니다. 접수된 요청은 `operationId`를
  반환하며, 접수는 모듈 적용 완료를 의미하지 않습니다.
- 한 번에 하나의 게이트 작업만 진행합니다. 작업 중 들어온 요청은 대기
  요청을 덮어쓰지 않고 HTTP `409`와 `static_radar_tuning_busy`로
  거부합니다.
- 쓰기 후 펌웨어는 로컬 값을 비우고 LD2410C에서 설정을 다시 조회합니다.
  이동 및 정지 threshold가 모두 요청값과 일치할 때만 작업을 `applied`로
  완료합니다. 값이 다르면 `readback_mismatch`, 응답이 없으면
  `readback_timeout`으로 실패합니다.
- gate 변경은 engineering 세션이 활성 상태일 때만 허용합니다. 그렇지
  않으면 HTTP `409`와 `static_radar_tuning_inactive` 코드를 반환합니다.
- `POST /api/control/static-radar-tuning/factory-reset`은 LD2410C 모듈의
  자체 초기화 명령으로 전체 설정을 공장 기본값으로 복원합니다. 진행 중인
  engineering 세션과 대기 중인 gate 변경을 취소한 뒤 모듈을 재시작하며,
  응답에는 `restartExpected: true`가 포함됩니다.
- LD2410C를 사용할 수 없으면 `static_radar_unavailable`, 이미 승인된 초기화
  요청이 기기 제어 루프에서 아직 소비되지 않았으면
  `static_radar_factory_reset_pending`으로 거부합니다.
- 부팅 메타데이터 동기화, engineering 세션 변경, 게이트 쓰기, 공장 초기화는
  하나씩 직렬로 처리합니다. 각 명령은 일치하는 ACK를 기다리고, 모드 변경은
  예상한 주기 프레임이 다시 들어온 뒤에만 완료됩니다. 최근 ACK는 크기가 제한된
  순서 기록에 보관하며 다른 명령의 ACK는 현재 작업을 실패시키지 않고 건너뜁니다.
- 활성 engineering 세션 요청은 부팅 메타데이터 동기화보다 우선합니다. 명령
  프로토콜이 한 번이라도 실패하면 대기 중인 감도 조정과 게이트 요청을 취소하고
  `sensor_recovering`을 보고합니다. 복구 뒤 오래된 UI 요청을 다시 실행하면 안 됩니다.
- 로컬 LD2410 컴포넌트만 ACK를 판정하고 명령을 전송합니다. 원시 UART 진단 경로는
  두 번째 ACK 이벤트나 별도의 명령 전송 주체를 만들면 안 됩니다.
- 스트림 watchdog 복구는 횟수가 제한된 최후 수단이며 진행 중인 명령
  트랜잭션과 겹치면 안 됩니다. 설정 모드 진입과 모듈 재시작 뒤 새 주기 프레임
  10개 이상 및 1.5초 안정화 시간을 모두 확인해야 복구가 완료됩니다. 그동안 부팅
  동기화와 감도 조정은 차단합니다.
- Engineering 계측은 진단 용도일 뿐이며 재실, 움직임, 타깃 좌표와 수,
  구역, 방 또는 히트맵을 변경하면 안 됩니다.

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

기기 설정 저장은 floorplan 및 stats 데이터와 같은 chunked upload 패턴을 사용합니다.
zone, floorplan, future spatial-platform 설정은 안전한 단일 form 요청 크기를 넘을 수 있습니다.

Endpoints:

- `POST /api/config/upload/start`
- `POST /api/config/upload/chunk`
- `POST /api/config/upload/commit`

Rules:

- `start`에는 `session`과 `size`가 필요합니다.
- `chunk`에는 `session`, `offset`, hex-encoded `data`가 필요합니다.
- `commit`에는 `session`이 필요합니다.
- `commit`은 성공 응답을 반환하기 전에 저장된 device config를 firmware cache에 다시 로드해야 합니다.
- `POST /api/config`, `POST /api/stats`, `POST /api/floorplan`,
  `POST /api/floorplan/image` 같은 단일 form legacy write는 embedded API에서 제거합니다.
  대시보드와 복원 흐름은 chunked upload endpoint를 사용해야 합니다.

`GET /api/floorplan/status`는 로컬 평면도 저장 상태의 읽기 전용 원본입니다.
payload 존재 여부와 byte 수에 더해 `configHash`, `imageHash`를 반환합니다.
클라이언트는 이 hash로 로컬 저장 또는 인증된 Site 복제가 완료된 시점을
감지하며, Node별 장치 설정, 레이더 위치, 구역 설정을 공유 상태로 취급하지 않습니다.

`POST /api/floorplan/delete`는 form field `confirm=1`을 요구합니다. Configuration Owner에서
revoked되지 않은 원격 Site 구성원이 하나도 남아 있지 않을 때만 실행할 수 있습니다. 연결된
Site에서는 HTTP `409`와 `floorplan_delete_requires_standalone`을 반환하며 평면도 교체만
허용합니다. follower에서는 `site_configuration_owner_required`를 반환합니다. 단독 기기에서
삭제가 성공하면 로컬 CONFIG와 IMAGE를 지우고 비어 있는 평면도 manifest를 배포합니다.
각 기기의 레이더 위치, 구역 및 튜닝 설정은 로컬에 남습니다.

전체 문서 업로드와 공유 patch 쓰기는 Configuration Owner이고 공유 설정 연속성이 쓰기
가능할 때만 허용합니다. 대시보드는 교체를 시작할 때 기존 payload를 먼저 삭제하지 않으며,
새 payload와 manifest가 확정될 때까지 기존 평면도를 authoritative 상태로 유지합니다.

### 7.3.2 평면도 부분 저장

작은 평면도 편집은 전체 평면도 문서를 다시 업로드하지 않고 전용 단일 form patch endpoint를 사용할 수 있습니다.
이 endpoint들은 크기가 제한된 저메모리 필드에만 사용하며, 이미지 업로드, 방 geometry 전체 교체,
wall segment 전체 교체, object 목록 전체 교체, 복원 flow에는 사용하지 않습니다.

Endpoints:

- `POST /api/floorplan/radar`
- `POST /api/floorplan/room-name`
- `POST /api/floorplan/occlusion`
- `POST /api/floorplan/objects`

Rules:

- `POST /api/floorplan/radar`는 `originPx`, `rotationDeg`, `scale`을 가진 JSON object를 form field `data`로 받습니다.
  저장 문서가 평면도 schema version 2이면 현재 `poseVersion`도 함께 받습니다.
- `POST /api/floorplan/radar`는 이전 대시보드 호환용으로 유지합니다. 현재 대시보드는 일반적인 레이더 위치 편집을
  `deviceConfig.floorplan.radar`에 저장하여 Node별 로컬 pose 변경이 공유 평면도 문서를 다시 쓰지 않게 합니다.
- `POST /api/floorplan/room-name`은 form field `id`, `name`을 받습니다.
- `POST /api/floorplan/occlusion`은 `ignoredEdges`를 가진 JSON object를 form field `data`로 받습니다.
- `POST /api/floorplan/objects`는 평면도 가구 object JSON array를 form field `data`로 받습니다.
- 기기는 저장된 floorplan config payload의 해당 필드만 갱신하고 raw-partition metadata를 authoritative source로 유지합니다.
- patch 대상이 없거나, 잘못되었거나, 너무 크면 표준 error envelope으로 `invalid_request`, `not_found`,
  또는 `upload_storage_failed`를 반환합니다.
- 전체 평면도 문서 저장, 최초 저장, 복원 flow, 방 geometry 편집, scale 편집, wall segment 편집, 이미지 저장은 계속
  `POST /api/floorplan/upload/start|chunk|commit`을 사용해야 합니다.

### 7.3.3 평면도 식별자와 revision

평면도 문서 schema version 2는 영속 식별자와 geometry/Node pose의 독립적인 revision을 추가합니다.

```json
{
  "version": 2,
  "floorplanId": "fp-00112233445566778899aabbccddeeff",
  "geometryVersion": 7,
  "source": "manual",
  "outline": {
    "pointsPx": [[120, 120], [760, 120], [760, 560], [120, 560]]
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

- `floorplanId`는 무작위로 생성한 영속 식별자입니다. 이미지 byte, 방 ID, geometry 또는 문서 hash에서
  파생하지 않습니다.
- `source`는 선택적인 추가 필드입니다. `image`는 업로드한 평면도, `manual`은 대시보드에서 직접 그린
  평면도를 뜻합니다. 수동 문서는 정확한 최외곽 다각형을 `outline.pointsPx`에 저장하며, 기존 좌표
  변환에서 사용하는 `outerBoundsPx`는 이 다각형의 바운딩 좌표계로 유지합니다.
- 대시보드는 수동 도면을 호환용 WebP 스냅샷으로 렌더링하고 업로드 평면도와 동일한 CONFIG + IMAGE
  묶음으로 저장합니다. 현재 구조화된 수동 평면도는 생성 화면, 저장된 평면도 편집 화면, 대시보드에서
  방, 물리 벽, 문을 CONFIG로부터 렌더링하며 WebP에 있는 같은 요소를 중복 표시하지 않습니다.
  업로드, 복제, 저장 endpoint 계약은 바뀌지 않습니다. 최초 저장 시 외곽선과 물리 벽
  geometry를 `wallSegments`에 고정하며, 이를 잘못 그린 경우 평면도를 삭제하고 다시 만듭니다.
  `rooms[].pointsPx`는 고정된 물리 벽과 별개인 논리 방 구역으로서 저장 후에도 편집할 수 있습니다.
  수동 문은 선택적 `doors` 배열에 `id`, `kind`, `centerPx`, `widthPx`, `rotationDeg`를 저장하며,
  생성 편집 화면, 저장된 평면도 편집 화면, 대시보드가 같은 geometry와 같은 문 overlay를 사용합니다.
  저장 후 문을 추가, 이동, 길이 변경 또는 삭제할 때에도 고정된 `wallSegments`에 스냅하며, 해당 문이
  자동 생성했던 퇴실 구역만 함께 갱신하고 다른 구역은 유지합니다.
- 직접 그리기는 고정된 3000 x 2000 좌표 캔버스를 사용합니다. 외곽선을 확정하기 전 기존
  확대·축소 조작으로 작업 축척을 25% 단위로 조용히 선택합니다. 기본 100%는 30m x 20m,
  25%는 120m x 80m를 담으며, 100%보다 확대할 때도 같은 배경 리사이징 경로를 사용합니다.
  선택한 물리 축척은 외곽선 확정 시 고정됩니다. 도형은
  0.5m 격자에 맞춰지고 각 변의 길이는 0.1m 단위로 표시됩니다. 별도 이미지 축척 입력은
  받지 않습니다.
- 외곽선과 방 다각형은 별도로 작성합니다. 외곽선은 교차하지 않는 다각형이어야 하고 모든 방은
  외곽선 안에 있어야 하며, 레이더 배치 전에 방을 하나 이상 추가해야 합니다. 직접 그리기에서
  확정한 방의 변은 초기 물리 벽인 동시에 초기 논리 방 구역이 됩니다. 작성 중인 도형을 취소해도
  작성 세션은 유지되어 같은 단계에서 다시 시작할 수 있습니다.
- 처음 저장하기 전에는 외곽선과 방의 꼭짓점을 편집할 수 있습니다. 최종 WebP,
  `outline.pointsPx`, `wallSegments`는 같은 좌표계를 사용하며, 축척은 방들의 합집합이 아니라
  명시적인 외곽선을 기준으로 계산합니다. WebP에는 고정된 물리 벽과 문만 포함하고 논리 방의
  채움과 이름은 대시보드 overlay로 표시합니다.
- 레이더 차폐는 논리 방 구역이 아니라 물리 `wallSegments`를 기준으로 계산합니다. 외곽선의 변도 내부
  벽과 같은 차폐 벽이며, 각 가시선은 가장 가까운 물리 벽에서 멈추고 그 벽 너머의 레이더 도달 영역을
  회색으로 표시합니다. 차폐 표시는 `outline.pointsPx` 안으로 다시 잘라내지 않습니다.
  `occlusion.ignoredEdges`는 고급 설정의
  벽별 차폐 예외 목록입니다. 축척용 `outerBoundsPx`는 물리 차폐 외곽선으로 사용하지 않습니다.
  `wallSegments`가 없는 legacy 수동 문서는 저장된 외곽선과 방 geometry에서 최초 물리 벽을
  한 번 복원합니다. 구조화된 `doors`가 없는 legacy 문서는 WebP에 이미 그려진 문을 유지하고
  그 위를 검은 벽 overlay로 다시 덮지 않습니다.
- `geometryVersion`은 Floorplan 좌표계 revision입니다. 축척 변경처럼 픽셀과 실제 좌표의 대응을
  바꾸는 경우에만 증가합니다. 방 구역, 물리 벽, 외곽선, 가구 또는 방 이름 편집은 CONFIG hash만
  갱신하며 이미 승인된 레이더 변환과 공간 보정을 무효화하지 않습니다.
- 로컬 레이더 배치 편집은 해당 Node의 `poseVersion`만 증가시킵니다.
- 현재 Node의 유효한 배치와 `poseVersion`은 `deviceConfig.floorplan.radar`에 저장합니다. 로드할 때 이 로컬 값이
  공유 평면도 문서에 포함된 배치를 덮어씁니다.
- 완전한 로컬 레이더 배치가 있지만 `poseVersion`이 없는 legacy 기기 설정은 pose revision `1`로
  해석합니다. 대시보드는 다음 기기 설정 저장 시 이 migration을 영속화합니다.
- revision은 양의 32-bit unsigned 값이며 `4294967295`에서 더 증가하지 않습니다.
- legacy 문서는 load 시 새 식별자와 초기 revision을 받으며, 다음 저장은 전체 문서 업로드로
  migration을 영속화해야 합니다.
- 로컬 문서가 우연히 같다는 사실은 공유 Site 소유권을 만들지 않으며 대시보드가 연결된 participant를
  활성화하는 근거로 사용해서는 안 됩니다. authoritative Site floorplan, Space assignment와 Node별 pose가
  platform 설정 흐름에서 commit될 때까지 participant는 quarantine 상태를 유지합니다.

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
| `ota_reboot_waiting` | info | 200 | OTA 업로드 완료 후 재부팅 대기 | `retryAfterMs` |
| `ota_device_reappeared` | info | 200 | OTA 또는 재부팅 후 기기가 다시 응답함 | `version`, `buildTime` |
| `ota_verify_timeout` | warning | 503 | 제한 시간 안에 기기 응답 확인 실패 | `timeoutMs` |
| `firmware_trust_unavailable` | error | 503 | signed OTA trust 또는 PSRAM 작업공간을 사용할 수 없음. raw OTA fallback 금지 | 없음 |
| `firmware_header_invalid` | error | 400 | 사전 검증 header가 없거나 잘못되었거나 정확히 320바이트가 아님 | 없음 |
| `alpha_firmware_not_allowed` | warning | 403 | 유효한 Developer 서명 알파 패키지지만 현재 1회성 알파 설치 허용이 없음 | 없음 |
| `alpha_firmware_permission_invalid` | warning | 400 | 알파 설치 허용 요청에 엄격한 `enabled` 불리언 값이 없음 | 없음 |
| `header_mismatch` | error | 400 | 실제 업로드 envelope가 사전 승인한 envelope와 다름 | 없음 |
| `trust_rejected` | error | 400 | 서명, 키 역할, 키 상태, trust revision 또는 security epoch가 거부됨 | `trust` |
| `session_mismatch` | warning | 409 | upload 또는 cancel token이 활성 OTA session의 소유자가 아님 | 없음 |
| `offset_mismatch` | error | 400 | multipart chunk가 누락, 중복 또는 순서 변경됨 | 없음 |
| `size_mismatch` | error | 400 | 업로드 크기가 서명된 정확한 패키지 크기와 다름 | 없음 |
| `expired` | error | 400 | signed upload session의 비활성 제한 시간이 지남 | 없음 |
| `sink_begin_failed` | error | 400 | OTA 파티션을 열지 못함 | 없음 |
| `sink_write_failed` | error | 400 | OTA 파티션 기록 실패 | 없음 |
| `sink_finish_failed` | error | 400 | 이미지 검증, 기록 digest 검증 또는 trust commit 실패 | 없음 |
| `signed_firmware_required` | warning | 400 | signed 기기에 `.namo-ota`가 아닌 파일을 선택함 | 없음 |
| `legacy_firmware_bin_required` | warning | 400 | 구형 개발 펌웨어에 `.bin`이 아닌 파일을 선택함 | 없음 |

#### 7.5.1 Signed OTA 계약

`GET /api/firmware/trust`는 signed OTA 지원과 현재 영속 trust generation을 반환합니다.

```json
{
  "ok": true,
  "data": {
    "mode": "signed",
    "ready": true,
    "trustRevision": 1,
    "securityEpoch": 1,
    "generation": 1,
    "releaseKeyId": "소문자 16진수 32자",
    "developerKeyId": "소문자 16진수 32자",
    "sessionActive": false,
    "alphaInstallSupported": true,
    "alphaInstallAllowed": false,
    "alphaInstallRemainingSeconds": 0
  }
}
```

`POST /api/firmware/alpha-permission`은 휘발성 알파 설치 허용을 변경합니다.
폼 필드 `enabled`는 정확히 `true`, `false`, `1`, `0` 중 하나여야 합니다.
활성화하면 최대 10분 동안 유효한 Developer 서명 패키지 1회 설치를 허용합니다.
이 허용은 RAM에만 보관되어 재부팅하면 사라지고, Developer 서명 이미지가
플래시 다이제스트 검증과 활성화까지 통과한 뒤에만 소모됩니다. Release와
Root Recovery 패키지는 이 허용을 확인하거나 소모하지 않습니다.

대시보드는 320바이트 envelope를 소문자 16진수 640자로 바꾸어
`POST /api/firmware/update/start`의 `header` form 필드로 보냅니다. 성공 응답은 0이 아닌
`session`, 정확한 `imageSize`, `packageSize`, `signerRole`을 반환합니다. 그다음 전체 패키지를
multipart form으로 `POST /api/firmware/update?session=<uint32>`에 올립니다. 취소는
`POST /api/firmware/update/cancel?session=<uint32>`를 사용합니다.

펌웨어는 알파 설치 허용을 확인하기 전에 패키지 서명과 signer 역할을 먼저
검증합니다. 현재 허용이 없는 유효한 Developer 패키지는 OTA 파티션을 열기 전에
거부합니다. 유효하지 않거나 알 수 없는 서명은 항상 trust 실패로 거부되며 알파
확인창으로 전환되지 않습니다.

펌웨어는 OTA 파티션을 열기 전에 trust와 서명을 검증하고, 실제 envelope를 사전 검증 값과
바이트 단위로 비교하고, image만 파티션에 기록하고, 기록된 파티션의 hash가 서명된 digest와
같을 때만 활성화해야 합니다. 패키지가 anti-rollback 상태를 올리거나 Release 또는 Developer 키를
교체하면 다음 Trust Store를 비활성 A/B 슬롯에 먼저 기록하되 아직 공개하지 않습니다. OTA image
활성화가 성공한 뒤에만 staged trust를 commit합니다. staging 실패 시 image 활성화를 중단하고,
활성화 뒤 commit이 실패하면 현재 실행 파티션을 다시 선택하고 실패를 반환합니다. 활성화와 commit
사이에서 전원이 끊기면 새 image가 이전의 commit된 trust로 부팅할 수는 있지만, commit되지 않은
키 폐기를 공개하지는 않습니다. Root는 Release 또는 Developer 키 원격 복구를 승인하는 유일한
오프라인 키이며 Root 자체 교체는 통제된 USB 복구로 처리합니다.

multipart 펌웨어 바이트는 정확히 `POST /api/firmware/update` 경로에서만 소비해야 합니다.
다른 경로의 upload callback은 무시하며 활성 signed OTA session의 상태를 바꾸면 안 됩니다.

최종 검증이 끝나면 펌웨어는 `Connection: close`가 포함된 HTTP `200`을 먼저 보내고 그 뒤에
재부팅을 예약합니다. 대시보드는 업로드 진행률이 100%여도 명시적인 HTTP 오류 응답은 실패로
처리합니다. 브라우저 업로드 진행률은 표시 용도로만 사용하며 완료, 취소, 실패 판정에는 사용하지
않습니다. HTTP 응답 없이 network error 또는 browser abort가 발생하면 마지막 진행률과 관계없이
재부팅 확인으로 넘어갑니다. 완료 여부가 불확실한 이 상태에서는 cancel을 보내지 않으며, cancel은
사용자가 명시적으로 취소한 경우에만 사용합니다. 오류 상태는 사용자가
지우거나 새 업데이트를 시작할 때까지 session storage에 유지합니다.

signed 릴리즈 펌웨어는 ESPHome Native OTA와 raw ESPHome web OTA를 노출하지 않습니다.
대시보드는 trust endpoint가 HTTP 404인 경우에만 구형 `/update`를 사용할 수 있습니다. 그 밖의
trust endpoint 오류는 fail-closed이며 raw OTA fallback으로 이어지면 안 됩니다.

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

## 8. 클라이언트 응답 처리

대시보드와 외부 클라이언트는 `status.code` 또는 `error.code`를 제어 값으로 사용하고,
이를 현지화된 문구로 변환합니다.

예시:

```ts
const code = response.error?.code ?? response.status?.code ?? "unknown";
const text = messages.status[code] ?? messages.status.unknown;
```

호환성 fallback 순서:

1. `code`가 있으면 우선 사용합니다.
2. `code`가 없으면 기존 legacy 필드를 사용합니다.
3. 둘 다 없으면 일반적인 오류 문구를 표시합니다.

클라이언트는 한국어 또는 영어 문장을 파싱해서 동작을 판단하면 안 됩니다.

---

## 9. 펌웨어 응답 요구사항

펌웨어는 짧은 상태 코드와 크기가 제한된 detail 객체를 반환합니다. 구형 클라이언트와의
임시 호환에 필요한 경우에만 API 응답에 사용자 표시용 문장을 포함합니다.

일반 endpoint에는 큰 진단 payload를 넣지 않습니다. 자세한 진단 정보는 별도의 debug
endpoint에서 제공합니다.

Production fusion 빌드는 `GET /api/state`에 작은 최상위 `fusion` 객체와
타깃별 융합 근거를 추가할 수 있습니다.

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

`fusion`은 별도의 좌표 스트림이 아니라 크기가 제한된 실행 상태 정보입니다.
`authoritative`가 `true`이면 기존 `targets` 좌표가 이미 production fusion 결과입니다.
두 센서 이상의 관측이 같은 타깃을 뒷받침할 때만 `fused`가 `true`가 됩니다.
구형 펌웨어는 이 선택 필드를 모두 생략할 수 있습니다.

많은 record를 반환할 수 있는 debug endpoint는 firmware memory에서 하나의 큰 문자열을
만들지 않고 streaming 또는 chunk 응답을 사용합니다.

리플레이 진단 정보는 전용 debug endpoint를 통해 newline-delimited sample로
제공할 수 있습니다. 리플레이 payload는 오프라인 테스트용이며 일반 status 응답에
넣지 않습니다.

리플레이 진단에는 시간이 지나면서 작은 선택 tuple이 추가될 수 있습니다. 소비자는 알 수 없는
필드를 무시해야 하며, 선택 필드가 없을 때는 안전한 기본값을 사용해야 합니다.
예를 들어 `ex` tuple은 exit zone 근거를
`[exitActive, exitZoneMask, exitTargetCount, exitLastSeenAgeMs]` 형태로 보고합니다.

### 런타임 연결 상태

`GET /api/system/status`는 ESPHome 런타임 소유권과 Native API client 등록
상태를 분리해서 보고합니다.

- `wifi.connected`는 ESPHome Wi-Fi 컴포넌트가 연결된 STA를 실제로 소유할
  때만 `true`입니다. 초기 설정 중 ESP-IDF가 임시로 만든 연결은 포함하지 않습니다.
- `api.enabled`는 Native API 컴포넌트 존재 여부입니다.
- `api.keyConfigured`는 Noise PSK 저장 및 활성화 여부입니다.
- `api.serviceReady`는 ESPHome Wi-Fi, Native API 컴포넌트, API 키가 모두
  준비된 경우에만 `true`입니다.
- `api.clientConnected`는 Native API client의 현재 연결 여부입니다. `false`는
  정상적인 미등록 또는 대기 상태이며 Wi-Fi 장애로 표시하면 안 됩니다.
- `api.connected`는 호환성을 위해 유지하는 `api.clientConnected` 별칭입니다.
- `api.warning`은 과도기적인 ESPHome 컴포넌트 상태입니다. 새 UI는 이를 Wi-Fi
  장애나 Home Assistant 등록 실패 판정에 사용하면 안 됩니다.
- 명시적인 초기 설정 전환 후 대시보드는 `wifi.connected`만 기다립니다.
  Native API client 등록은 설정 완료 조건이 아니며 설정 창을 다시 열면 안 됩니다.

### 9.1 읽기 전용 플랫폼 상태

`GET /api/system/status`는 다음 선택 `platform` 객체를 포함할 수 있습니다.

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

같은 응답은 부팅 보호 상태를 포함할 수 있습니다.

```json
{
  "boot": {
    "initialGuardActive": true,
    "guardSeconds": 60,
    "remainingSeconds": 37
  }
}
```

`remainingSeconds`는 응답 시점에 펌웨어가 계산한 값이며 재부팅, 초기화, OTA
제어의 기준 카운트다운입니다. 대시보드는 이 값을 로컬에서 갱신할 수 있지만 화면이
다시 보일 때 서버 상태를 새로 확인해야 하며, 쓰기 API의 `boot_guard_active`도 계속
처리해야 합니다.

규칙:

- 구형 펌웨어와 대시보드 호환성을 위해 객체 전체가 선택 필드입니다.
- `siteId`와 `nodeId`는 `identityReady`가 `true`일 때만 제공합니다.
- `mode`는 Site 가입 전에는 `"standalone"`, joiner가 leader Site를 transaction으로
  채택한 뒤에는 `"enrolled"`입니다. 어느 값도 fusion 활성화를 의미하지 않습니다. 같은
  `siteId`에 다시 가입하더라도 이전 로컬 mode가 `"standalone"`이면 `"enrolled"`를
  저장해야 합니다. 부팅 시에는 확정된 canonical roster가 다른 설정 원본을 가리키고
  로컬 Node를 active 구성원으로 포함할 때만 이 오래된 mode를 복구할 수 있습니다.
- `maxParticipants`는 protocol 용량 제한이며 fusion 활성화를 의미하지 않습니다.
- `registryReady`는 크기가 제한된 원격 participant registry를 안전하게 불러왔는지만 나타냅니다.
  Registry가 실패하면 원격 참여만 비활성화하며 standalone 감지는 계속 동작해야 합니다.
- Participant count는 원격 항목만 세며 로컬 Node는 포함하지 않습니다.
- `remoteParticipantCount`는 보존된 revoked record까지 포함하고,
  `connectedRemoteParticipantCount`는 quarantined 또는 active 관계를 세며,
  `activeRemoteParticipantCount`는 활성화된 관계만 셉니다.
- `registryReady`가 `false`이면 count는 모두 `0`입니다.
- `readinessReady`는 별도 capability/assignment readiness 저장소를 안전하게
  불러왔는지 나타냅니다. 사용할 수 없으면 configured count는 `0`입니다.
- `pairingSupported`는 펌웨어 기능 지원 여부입니다. `pairingEnabled`는 제한된 로컬 등록 창이
  열려 있는 동안에만 `true`입니다.
- `fusionEnabled`는 이 firmware에 인증 production fusion이 포함되었는지를 나타냅니다.
  runtime 권한을 부여하는 값은 아니며 Site, Group, readiness, credential 및 공간 gate를
  모두 통과한 authoritative 상태는 `GET /api/platform/groups`에서만 확인합니다.
- Private key, certificate 자료, preference record 및 초기화 marker는 절대 반환하지 않습니다.

#### 9.1.1 기기 자원 계측

`GET /api/system/status`는 기존 `memory`, `flash` 객체에 크기가 제한된 자원
계측값을 선택적으로 포함할 수 있습니다.

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

규칙:

- 모든 값은 음수가 아닌 byte 수이며 구형 펌웨어 호환성을 위해 선택 필드로 유지합니다.
- `internalLargestFreeBlockBytes`는 현재 할당 가능한 가장 큰 내부 8-bit heap
  block입니다. 전체 여유량만으로 알기 어려운 단편화를 함께 확인합니다.
- `httpdStackMinFreeBytes`는 이 요청을 처리하는 HTTP server task가 시작된 뒤
  기록한 최소 stack 여유량입니다. 전체 task stack 크기가 아닙니다.
- `httpdStackLowestEndpoint`는 firmware가 기록한 HTTP task 최소 여유량을 처음
  관측한 요청의 제한된 route code입니다. `httpdStackLowestPhase`가 `entry`이면
  framework dispatch와 URL 해석 직후 이미 최저값이 관측된 것이며, 이 경우 앞선
  비 Radar HTTP handler가 lifetime 최저값을 만들었을 수도 있습니다. `handler`이면
  선택된 Radar handler가 그보다 더 낮춘 것입니다. `httpdStackLowestEndpointFreeBytes`는 해당
  stack 여유 byte 수입니다. status 응답을 직렬화하는 도중 더 낮은 값이 생기면 endpoint
  귀속은 다음 status 요청에서 확정될 수 있습니다.
- endpoint 귀속은 진단 전용입니다. 모든 Radar API, setup, dashboard 및 dashboard
  asset route를 고정 code로 분류하고, 고정 저장공간만 사용하며 요청 URL을 보관하거나
  요청마다 문자열을 할당하면 안 됩니다.
- status 응답은 크기가 제한된 재사용 workspace와 chunked HTTP 출력으로 직렬화합니다.
  변하지 않는 flash 및 실행 image metadata는 HTTP handler를 등록하기 전에 수집하며,
  status 요청에서 실행 image를 다시 분석하거나 전체 JSON 문서를 내부 RAM에 누적하면
  안 됩니다.
- PSRAM과 제한된 내부 fallback workspace를 모두 할당하지 못하면 HTTP `503`과
  `code: "system_status_unavailable"`,
  `detail.resource: "status_workspace"`를 반환합니다.
- 프론트엔드 새로고침 한 번의 값만으로 crash를 판정하지 않습니다. 자동 자원 smoke는
  제한된 여러 sample과 명시적인 safety margin을 사용합니다.
- 자원 상태는 읽기 전용이며 HTTP server stack에 큰 직렬화 buffer를 만들면 안 됩니다.

### 9.2 읽기 전용 Node Descriptor와 발견

`GET /api/platform/descriptor`는 로컬 NAMO Node의 공개 protocol identity와 capability를 반환합니다.

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

규칙:

- Descriptor는 읽기 전용이며 peer를 가입, 승인 또는 신뢰하지 않습니다.
- `participantId`는 안정적인 Node identity입니다. `siteId`는 의도적으로 넣지 않습니다.
- 일반 zone presence 근거를 platform Observation wire 경로에서 내보내기 전까지
  `zonePresence`는 `false`를 유지합니다.
- Attestation, private key, certificate, network address 및 변경 가능한 Site membership은
  반환하지 않습니다.
- 안정적인 identity를 사용할 수 없으면 HTTP `503`과
  `platform_identity_unavailable` code를 반환하며 임시 identity를 만들지 않습니다.
- 기기는 80번 port에서 `_namo._tcp`를 광고합니다. TXT record는 `protocol_min`,
  `protocol_max`, `participant_id`, `participant_type`, `product`, `descriptor_path`,
  `pairing`, `identity_ready`만 포함합니다.
- `pairing=manual`은 정적인 기능 메타데이터입니다. 현재 등록 창의 열림/닫힘 상태는 participants
  endpoint를 기준으로 판단합니다.
- Multicast discovery를 사용할 수 없으면
  `http://<device-ip>/api/platform/descriptor`로 직접 접근할 수 있어야 합니다.

### 9.3 로컬 참가자 레지스트리와 등록 창

`GET /api/platform/participants`는 제한된 로컬 참가자 레지스트리와 등록 창 상태를 반환합니다.

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

`siteReconciliation`은 마지막 기기 관계가 제거된 뒤 남아 있는 Site 소유권을
정리하는 제한된 복구 작업의 상태입니다. `state`는 `not_required`, `pending`,
`complete`, `failed` 중 하나이며, `result`는 `idle`, `ok`, `invalid_state`,
`build_failed`, `site_storage_failed`, `membership_storage_failed` 중 하나입니다.
상태가 `pending` 또는 `failed`인 동안에는 연결 허용 창을 열지 않습니다.

각 참가자 항목에는 공개 가능한 레지스트리 상태만 포함합니다.

```json
{
  "participantId": "namo-node-example",
  "participantType": "namo_node",
  "trust": "community_unverified",
  "membership": "quarantined",
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
    "floorplanReplicaReady": true
  },
  "readiness": {
    "ready": true,
    "missing": []
  }
}
```

설정 전에는 `capabilities`와 `assignment`가 `null`이며,
`readiness.missing`에는 `capabilities`, `assignment`, `pose`, `calibration`이
표시됩니다. Presence-only Satellite는 pose와 calibration을 요구하지 않습니다.

`POST /api/platform/participants`는 필수 `action`을 포함한 form encoded 인자를 받습니다.

| Action | 필수 인자 | 동작 |
|---|---|---|
| `configure` | `participantId`, `pointTracks`, `presence`, `motion`, `maxTargets`, `floorplanId`, `spaceId`, `groupId`, `geometryVersion`, `localPoseVersion`, `poseVersion`, `calibrationVersion`; 선택 capability boolean `velocity`, `positionUncertainty`, `range`, `zonePresence`, `bridge`; 선택 transform 묶음 `originXMillimeters`, `originYMillimeters`, `rotationCentiDegrees`, `residualMillimeters`, `calibrationSamples` | 참여자의 capability와 배정을 저장합니다. `localPoseVersion`은 요청을 받는 Node의 레이더 pose를, `poseVersion`은 보정 대상 참여자의 레이더 pose를 나타냅니다. transform 필드를 하나라도 보내면 전체 필드가 필요하며 승인된 유효 변환을 함께 저장합니다. active 참여자를 다시 설정하면 먼저 quarantine을 저장합니다. |
| `reconfigure` | `configure`와 동일 | 기존 active 참여자의 membership을 유지한 채 readiness만 교체합니다. 재보정 전용 동작이며 active가 아니거나 참여자가 없으면 거부합니다. readiness 저장 또는 설정 원본의 Site roster 트랜잭션이 실패하면 이전 readiness를 복원합니다. |
| `open` | 선택 `durationSeconds` (`30..300`, 기본 `120`) | 제한된 등록 창을 열고 대기 중 후보를 지웁니다. |
| `close` | 없음 | 등록 창을 닫고 대기 중 후보를 지웁니다. |
| `stage` | `participantId`, `participantType`, `protocolVersion`, `product` | 등록 창이 열려 있을 때 호환 후보 하나를 대기 상태로 둡니다. |
| `approve` | 없음 | 대기 중 후보의 참가자 유형으로 trust를 결정해 승인하고 quarantine 상태로 저장합니다. |
| `activate` | `participantId` | 승인되고 trust가 확인된 quarantine 참가자를 readiness 완료 후 활성화합니다. NAMO Node는 online 상태에서 canonical 평면도 replica digest도 정확히 일치해야 하며, 배정된 Group의 active 감지 기기가 7대 미만이어야 합니다. |
| `quarantine` | `participantId` | 기존 참가자를 삭제하지 않고 비활성화합니다. |
| `revoke` | `participantId` | 기존 참가자를 폐기 상태로 바꾸고 저장된 전송 credential을 비활성화합니다. 어느 한 저장 단계라도 실패하면 이전 registry와 credential 상태를 복원합니다. |
| `forget` | `participantId` | active가 아닌 기존 참가자와 readiness 및 저장된 전송 credential을 삭제합니다. 어느 한 저장 단계라도 실패하면 이전 registry, readiness, credential 상태를 복원합니다. |
| `set_community_leader` | `participantId`, `eligible` | 설정 원본 기기에서 active community NAMO Node의 Leader 자격을 명시적으로 변경합니다. 변경 후 canonical roster를 갱신하며 roster 저장이 실패하면 기존 로컬 자격으로 복원합니다. |

규칙:

- `activate`는 trust 승인만으로 성공하지 않습니다. Spatial 참여자는 point-track
  capability, `1..3` 범위의 `maxTargets`, Floorplan/Space/Group 배정, 0이 아닌
  pose/calibration version이 모두 필요합니다. 미완성 상태는 HTTP `409`,
  `not_ready`로 거부합니다.
- NAMO Node 활성화는 canonical roster 또는 평면도가 확정되지 않았거나, 해당 participant가
  roster에 없거나, endpoint가 offline이거나, replica가 준비되지 않았거나,
  generation/version/hash가 다르면 원인별 HTTP `409` code로 거부합니다. Directory 수렴은 이
  변경 경계에서 중복 검사하지 않습니다. Directory가 없거나 오래된 상태에서 활성화된 Node는
  인증된 일반 Directory 복제가 끝날 때까지 역할 및 융합 권위에서 제외됩니다. 이 activation
  admission을 통과한 뒤에만 credential을 활성화합니다.
- NAMO Node의 canonical Group에 active 감지 기기가 이미 7대라면 HTTP `409`,
  `group_full`로 거부하며 기존 active 기기에는 영향을 주지 않습니다.
- `set_community_leader`는 현재 설정 원본 기기에서만 허용합니다. 이 작업은 설정
  원본을 바꾸거나 production 권한을 만들지 않습니다. 제조 attestation으로 공식
  NAMO 하드웨어를 구분할 수 있기 전까지 community 기기의 Leader 자격은 사용자가
  로컬에서 명시적으로 선택합니다. 초기 설정에서 자동 대체를 선택하면 canonical
  활성화 후 마지막 보정 수렴 전에 자격을 저장합니다. 이후 기기 관리에서 같은 작업을
  다시 실행해도 멱등하게 처리합니다.
- Presence-only 참여자는 `maxTargets=0`이어야 하며 가짜 spatial pose나
  calibration version을 만들 필요가 없습니다.
- Spatial 보정은 로컬 평면도의 레이더 배치와 별도로 승인합니다. `configure`는 해당
  참여자 관계에 사용할 로컬 레이더 좌표계에서 평면도 좌표계로의 유효 변환을 저장하며,
  `radar.originPx`, `radar.rotationDeg` 또는 Node의 로컬 `poseVersion`을 변경하지 않습니다.
- 이미 격리 상태인 참여자를 설정해도 canonical Site roster revision을 다시 발행하지
  않습니다. active 참여자를 재설정하면 먼저 격리 상태로 바뀌며, 이는 실제 membership
  변경이므로 새 roster revision을 발행합니다.
- 활성 spatial 관계는 canonical 평면도 식별자와 geometry revision, 요청을 받는 Node의
  현재 레이더 pose, 인증된 상대 heartbeat의 레이더 pose를 기준으로 검사합니다. 불일치하면
  승인된 보정을 지우고 quarantine을 저장한 뒤 재보정을 요청합니다. 인증된 제어 경로에
  사용하는 credential은 유지합니다. 펌웨어는 loop 한 번에 관계 하나만
  처리합니다.
- 페어링 직후 보정에 진입할 때에는 Site 동기화를 요청하기 전에 양방향에서 첫 인증·온라인
  heartbeat가 확인될 때까지 기다립니다. 보정 준비는 대시보드 수렴 fence를 사용하고 활성화는
  펌웨어의 roster 및 replica admission fence를 사용합니다. authority epoch, generation,
  configuration digest, configuration consistency,
  geometry version, CONFIG hash, IMAGE hash와 각 송신자의 예상 로컬 레이더 `poseVersion`이
  현재 canonical Site와 모두 일치해야 합니다. 이전 revision의
  `floorplanReplicaReady=true`만으로는 보정을 활성화하지 않습니다.
- 초기 등록은 pair credential commit, canonical Site Configuration 복제, canonical Site
  Directory 복제, 평면도 복제, 보정/readiness, direct 활성화, canonical 활성화의 단조로운
  한 가지 단계 순서를 따릅니다. compatibility mirror는 bootstrap 저장 상태일 뿐 역할 조정에
  입력되거나 Directory authority로 광고되지 않습니다. 같은 Site의 오래된 canonical
  Directory는 활성화를 영구 차단하지 않으며, 기존 권위 및 복제 계층이 수렴 전까지 해당
  Node를 권위 없는 상태로 유지합니다.
- `rotationCentiDegrees`는 `-18000..18000`, `residualMillimeters`는 `0..1000`,
  `calibrationSamples`는 `1..2` 범위입니다. transform 묶음은 하위 호환되는 선택 확장입니다.
  필드를 하나라도 보내면 전체 필드가 유효해야 합니다. transform이 없던 기존 readiness
  기록은 pose, calibration version, activation 상태를 그대로 유지합니다. 같은 calibration
  version을 선택 필드 없이 다시 저장하면 이미 승인된 transform도 보존합니다.
- 영속 participant action과 pairing commit은 하나의 제한된 operation lease를 함께 사용합니다.
  따라서 HTTP task와 component loop가 registry, readiness, credential 또는 설정 원본의
  Site roster를 동시에 쓸 수 없습니다. 충돌한 HTTP action은 HTTP `409`, code `busy`를 반환하며,
  caller는 현재 participant snapshot을 확인한 뒤 같은 멱등 action을 다시 시도할 수 있습니다.
  등록 창의 `open`, `close`, `stage`도 같은 lease를 사용하므로 pairing commit 저장 중에는
  대기 candidate가 바뀌지 않습니다.
- 영속 participant action은 첫 저장 전에 제한된 registry, readiness, credential을 PSRAM에
  snapshot으로 보관합니다. 이후 enrollment, credential 또는 설정 원본 Site roster 저장이
  실패하면 실제 generation이 바뀐 저장소만 이전 내용으로 복원합니다. 복원 자체가 실패하면
  성공으로 처리하지 않고 HTTP `503`, code `rollback_failed`를 반환합니다.
- registry, readiness, credential, Site configuration transaction은 큰 preference blob을
  NVS에 반복 기록하지 않고 radar data 파티션 끝에 예약된 4 KiB sector 12개를 사용합니다.
  기존 participant sector 9개의 절대 주소는 유지하고 Site configuration sector 3개를
  그 앞에 추가합니다. 펌웨어 업데이트 시 유효한
  legacy NVS transaction을 먼저 이관한 뒤 participant 관련 key만 제거합니다. HTTP payload와
  endpoint는 바뀌지 않습니다.
- `readiness` 또는 `credential` 저장소를 사용할 수 없으면 관련 원격 기능은 닫힌 채 실패하지만
  단독 재실 감지는 계속 동작합니다.
- 기기 연결은 pairwise trust만 만듭니다. 공유 평면도를 추측하거나 participant를 활성화하거나,
  서로 독립적으로 저장된 두 로컬 문서를 Site 설정으로 취급하지 않습니다.
- 직접 연결된 격리 상태 NAMO Node는 위치 보정을 미룬 동안 pairwise Owner credential로
  제어 영역 heartbeat만 보낼 수 있습니다. 이 bootstrap은 관측 전송, 융합, leader 자격,
  participant 활성화 또는 Site credential bundle 복제를 허용하지 않습니다.

호스트 readiness/admission 검증은 `npm test`에 포함됩니다. 실제 기기에서는 아래
명령 하나로 임시 Satellite의 등록, 준비 상태 gate, 활성화, 재부팅 복원, 정리까지
확인할 수 있습니다.

```powershell
python tools/participant_readiness_smoke.py --device http://192.168.29.104
```

이 도구는 임시 Satellite의 HTTP 흐름과 정리만 확인하며 기기를 재부팅하지 않습니다.
재부팅 저장 복원은 ESPHome이 정상 부팅을 확정한 뒤 별도로 수행하는 수동 실기기
검증으로 남깁니다. 실패하면 pairing window를 닫고 제한된 정리를 시도한 뒤 HTTP 상태와
구조화된 API 오류 code를 함께 출력합니다.

공개 테스트 키를 사용하는 인증 UDP shadow transport smoke는 명시적으로 켜는 개발 전용
기능이며 배포하면 안 됩니다. 개발 workspace에서 진단 이미지를 만든 뒤 같은 lifecycle
smoke에 transport 검증을 추가합니다.

```powershell
esphome -s shadow_transport_smoke true compile namo.yaml
python tools/participant_readiness_smoke.py --device http://192.168.29.104 --transport
```

`--transport`는 RAM 전용 `satellite-smoke-transport` identity와 공개 테스트 키를 사용하고
실행할 때마다 새로운 boot ID를 만듭니다. 정상 observation 2개 수락, 인증 실패 거부,
초당 2패킷 제한, 중복 sequence 거부를 확인합니다. 영구 participant를 만들거나 기기를
재부팅하지 않으므로 같은 부팅에서도 반복 실행할 수 있습니다.

- 이 endpoint는 로컬 레지스트리 관리 기반입니다. 이 endpoint 자체가 peer 탐색, Site
  credential 교환, attestation 검증 또는 production fusion 권한을 부여하지 않습니다.
  Credential transaction 저장은 구현됐지만 raw key를 받거나 반환하는 HTTP action은 없습니다.
  opt-in 송수신 경로를 켜면 이 응답에 제한된 `shadowTransport` 카운터가 표시됩니다.
  Smoke sample은 단독 재실, 구역, Home Assistant 엔티티 또는 fusion에 영향을 주지 않으며,
  정상 인증 participant sample은 별도로 gate된 `productionFusion` adapter를 통해서만
  production 경로에 들어갈 수 있습니다.
  observation 송신기는 수신 소켓을 재사용하고 초당 최대 2개의 observation만 보내며
  재전송하지 않습니다. Active 소속, readiness, pose, calibration, credential 검사를 모두
  통과한 경우에만 인증된 온라인 Group Leader로 보냅니다. `sendTarget`에는 주소나
  credential이 아니라 Node ID만 표시합니다. Fusion Snapshot fan-out은 수신 기기별 500ms에
  최대 1개, component cycle당 대상 기기 1개로 별도 제한합니다.
- 대기 후보는 하나만 존재할 수 있습니다. 후보는 RAM에만 있고 닫기, 만료, 승인 성공 또는
  재부팅 시 제거됩니다.
- `stage`는 protocol version `1`만 받습니다. NAMO Node의 product는
  `namo-spatial-sensor`여야 하고 Satellite는 비어 있지 않은 product 식별자를 제공해야 합니다.
- caller는 trust를 선택할 수 없습니다. NAMO Node는 `community_unverified`, Satellite는
  `satellite`로 승인합니다. `official_verified`는 attestation 검증 전까지 사용할 수 없습니다.
- 승인과 활성화는 별도로 저장되는 변경입니다. 승인은 등록 창을 닫지만 참가자는
  `quarantined` 상태로 남습니다. `activate`만 `active` 상태와 `activeRemoteCount`를
  변경할 수 있으며, trust가 없거나 폐기된 참가자는 활성화할 수 없습니다.
- 레지스트리 변경은 transaction으로 저장합니다. 저장 실패 시 RAM snapshot을 바꾸지 않고
  HTTP `503`, code `persistence_failed`를 반환해야 합니다.
- identity 또는 레지스트리 저장소를 사용할 수 없으면 변경 요청은 HTTP `503`으로 닫힌 채
  실패합니다. 단독 재실 감지는 계속 동작해야 합니다.
- 응답에는 credential의 `provisioned`, `enabled`, `keyVersion` metadata만 포함합니다.
  private key, participant token, certificate, Site credential, preference blob, attestation material은 절대
  응답에 포함하지 않습니다.
- 저장된 peer credential은 participant 활성화 전에도 저빈도 control-plane heartbeat를
  인증합니다. 따라서 quarantined peer도 인증된 endpoint를 표시할 수 있지만, active이고
  readiness가 완료된 참여자만 Group coordination에 들어갑니다. revoked 또는 존재하지 않는
  참여자는 credential 정리가 남아 있어도 heartbeat를 보내거나 인증할 수 없습니다.
- 설정 원본은 active Site의 credential 배포 권한을 가집니다. Node는 기존 인증 UDP `49192`
  복제 transport에서 암호화된 `credential_bundle`을 직접 가져옵니다. bundle에는 설정 원본,
  같은 Fusion Group 구성원, 저빈도 Site control plane에 필요한 active Leader 가능 NAMO
  Node만 포함합니다. 자동 설정 원본 후임은 승계 계약에 필요한 모든 active 구성원
  credential을 보관할 수 있습니다. 이 동작은 로컬 1대와 원격 6대인 고빈도 fusion 입력 상한을
  늘리지 않습니다.
- 설정 원본이 아닌 Node는 원본과의 pair credential을 자신의 송신 신원으로 사용해 Site
  heartbeat, observation, Fusion Snapshot, Space summary와 replication을 서명합니다. 설정
  원본만 목적지별 직접 pair credential을 사용합니다. 수신 측은 인증 token을 payload
  송신자와 다시 대조하며 목적지 credential을 송신자 신원으로 취급하지 않습니다.
- Configuration Owner가 바뀐 직후 follower는 저장된 key에 붙어 있는 과거 participant
  이름을 신뢰하지 않습니다. credential 복제는 고정 상한의 로컬 registry를 순회하고 각
  요청을 `HMAC(key, localNodeId)`에 묶습니다. 새 Owner는 이 증명이 같은 active 요청자로
  해석될 때에만 bundle을 제공합니다. bundle 저장이 끝나기 전 follower의 heartbeat와
  observation 송신은 닫아 stale key가 peer endpoint 신원을 덮어쓰지 못하게 합니다.
  mDNS는 이 복구의 목적지 주소만 제공할 수 있으며 응답과 암호화된 bundle은 계속 pair
  credential로 인증합니다. 수신 bundle은 필요한 record를 모두 포함해야 하고 현재 Owner
  record가 요청에 실제 사용한 credential과 정확히 같아야 합니다. 따라서 과거 Owner의
  bundle은 새 Owner session을 완료할 수 없습니다.
- credential bundle은 복호화 전에 인증하고 기존 256-bit pair key로 암호화합니다. 확정 Site
  configuration generation과 일치할 때만 기존 고정 크기 registry에 transaction으로
  병합하며 HTTP에는 절대 노출하지 않습니다. 일부만 있거나 관계없는 record, 오래된 generation,
  저장 실패는 이전에 확정된 credential snapshot을 보존한 채 닫힌 상태로 실패합니다.
- roster revision만 바뀌면 인증 endpoint, Site·Group Leader와 summary cache를 지우지 않고
  정확한 credential bundle 갱신만 예약합니다. 설정 원본이나 authority epoch가 바뀌면 진행 중인
  credential bundle session만 취소하고, 이전에 확정된 credential snapshot을 보존한 채 canonical
  authority에서 제한된 갱신을 다시 시작합니다. 이 과정에서 Site·Group coordinator와 term,
  endpoint 인증, summary cache 및 production fusion을 초기화하면 안 됩니다. 새 canonical roster에
  완전한 기존 credential snapshot은 제한된 handover bootstrap으로 인증 heartbeat와 observation을
  계속 전송하며, 불완전한 snapshot은 fail-closed합니다. 갱신 중 고빈도 작업은 Group credential
  완전성 gate로 별도 차단합니다. 실제 Site identity 변경, 명시적 platform reset
  또는 호환되지 않는 영속 Site scope에서만 파괴적인 Site runtime 초기화를 사용합니다.
- heartbeat는 UDP `49191`, 2초 송신 간격, 6초 lease와 최대 320바이트의 고정 패킷을
  사용합니다. 펌웨어는 loop 한 번에 수신 패킷 최대 2개와 송신 대상 최대 2개만 처리합니다.
  소켓은 네트워크 연결 후에만 생성하며, 실패하면 5초 간격으로 재시도하고 기기 setup이나
  standalone 감지를 실패시키지 않아야 합니다.
- endpoint cache는 RAM 전용입니다. 등록된 peer의 주소와 온라인 상태는 인증된 heartbeat
  송신 주소를 기준으로 판단합니다. mDNS는 경로가 없거나 heartbeat가 만료된 endpoint를
  복구할 때만 사용하며, 등록된 peer가 모두 온라인인 동안에는 반복 검색하지 않습니다.
  pairwise HMAC, participant identity와 Site 일치, 더 새로운 boot/sequence 검증을 모두
  통과한 UDP 송신 주소만 인증합니다. DHCP로 IP가 바뀌어도 flash에 주소를 저장하지 않고
  다시 수렴합니다.
- `endpoint.online`은 마지막 인증 heartbeat 이후 8초가 지나면 false가 됩니다. 발견됐지만
  아직 인증되지 않은 경로는 주소가 있어도 `authenticated`와 `online`이 모두 false일 수 있습니다.

---

### 9.4 Shadow Group 조정

`GET /api/platform/groups`는 크기가 제한된 읽기 전용 Group coordinator
snapshot을 반환합니다.

`site` 객체와 모든 로컬 `groups` 항목은 한 번의 변경 불가능한 control loop publication에서
가져옵니다. HTTP 직렬화는 서로 다른 선출 cycle의 변경 중 coordinator를 섞어 읽지 않습니다.
최초 publication 전에는 HTTP `503`, code `platform_role_snapshot_unavailable`을 반환합니다.

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
    "leaseRemainingMs": 6000,
    "participantsOnline": 1,
    "participantsExpected": 2,
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

규칙:

- 최상위 `capacity`는 Site directory의 Group 최대 8개와 Group별 감지 participant 최대
  7개라는 서로 독립된 계층 한도를 뜻합니다. Site 전체 participant를 하나의 배열로 제한하는
  값이 아닙니다. participant 관리 endpoint는 별도의 로컬 관계 registry 용량을 보고합니다.
- coordinator 자체는 계속 읽기 전용입니다. 표준 NAMO 제품 빌드는 인증 production
  fusion을 포함하며 `shadowOnly: false`, `productionFusion.buildEnabled: true`를 보고합니다.
  로컬 Node가 선출된 Group Leader이고 모든 runtime gate를 통과하기 전까지
  `productionAuthority`는 `false`입니다. Transport smoke나 외부 component 빌드는 production
  fusion을 끌 수 있습니다. 빌드 capability는 저장된 Site, readiness 또는 migration 상태를
  바꾸지 않습니다.
- `shadowFusion`은 모든 빌드의 응답에 포함됩니다. `enabled: false`는 개발용
  관측 수신·런타임 경로가 빌드되지 않았거나 메모리를 할당하지 못했다는 뜻입니다.
  활성화된 런타임은 선출된 Group Leader에서만 동작하며 로컬 관측원 한 개와 active인
  인증 원격 point-track 관측원 최대 6개를 받고 fused track을 최대 6개로 제한합니다.
  `remoteParticipantId`는 하위 호환 진단용으로 첫 원격 participant를 유지하며 실제
  admission 입력 수는 `diagnostics.validInputs`에 표시합니다. `state`는 대기 또는 활성
  상태의 원인을 표시합니다. 직렬화된 `shadowFusion` 객체 자체는 읽기 전용이며 production
  권한을 부여하지 않습니다. 안정된 runtime track을 production 경로에 넣을지는 별도로
  gate된 `productionFusion` adapter가 결정합니다.
- `productionFusion`은 같은 인증 runtime 위에 놓인 고정 크기 adapter 입니다.
  `confirmed`와 `coasting` track만 최대 3개까지 내보내며, 기존 target,
  구역, state 경로에 전달하기 전에 world 좌표를 선출된 Group Leader의
  로컬 radar 좌표로 역변환합니다. runtime이 `active`이고 Leader의 승인된
  transform이 유효할 때만 `authoritative: true`가 될 수 있습니다.
- `productionFusion.state: degraded`는 비어 있는 공간을 뜻하지 않습니다.
  인증 transport, readiness, 관측원 수 또는 transform이 유효하지 않으면
  firmware는 즉시 기존 local tracker 경로를 유지합니다. production presence는
  local presence와 authoritative fused target의 OR이므로 transport 손실이
  직접 presence OFF를 만들 수 없습니다.
- 설정된 Group은 기존 authority lease가 끝난 뒤 온라인 Node 한 대가 Group Leader로
  선출되면 제한 운전을 계속할 수 있습니다. 그 기기의 canonical transform과 로컬 관측이
  유효하면 고정 크기 runtime은 로컬 source 하나를 받아 Group automation을 계속
  제공합니다. 영속 `siteId`, `spaceId`, `groupId`와 예상 구성원 수를 standalone으로
  바꾸지 않으며, 온라인 구성원이 설정 수보다 적은 동안 Site directory health는
  `degraded`입니다. 로컬 관측, transform 또는 모든 적격 Leader를 잃으면 계속
  fail-closed unavailable로 처리합니다.
- `local_transform_unavailable`은 선출된 Group Leader에 승인된 canonical Site spatial
  profile이 없는 상태입니다. 런타임은 보정되지 않은 로컬 배치와 보정된 원격 배치를 섞지 않고
  fail-closed로 대기합니다.
- 런타임 입력은 인증을 통과한 고정 크기 latest-wins 관측입니다. 만료된 관측은
  inbox에서 제거하며, 선택된 로컬·원격 sequence 집합을 그대로 다시 읽어 fusion
  confirmation을 진행하지 않습니다.
- 각 NAMO Node는 canonical Site spatial profile에 저장된 Group만 선택합니다. 같은
  Site에 있는 다른 Group은 해당 기기의 fusion 입력, observation 송신, Group 선출,
  Fusion Snapshot 배포에 들어오지 않습니다. canonical 배정이 없는 version 1·2 Site
  record는 Group이 정확히 하나일 때만 결정적인 호환 fallback을 사용하며, 둘 이상이면
  임의로 고르지 않고 fail-closed로 대기합니다.
- 인증 transport envelope의 message type `2`는 `Fusion Snapshot V1`입니다. 선출된
  Group Leader는 UDP `49190`에서 같은 Group의 active·ready 기기 각각에 고정 크기
  snapshot 하나를 직접 보냅니다. 기존 pairwise HMAC credential을 재사용하며 `siteId`,
  `leaderNodeId`, `groupId`, `bootId`, transport `sequence`, `groupTerm`, fusion
  `sourceSequence`, `ttlMs`, presence, motion과 최대 3개의 confirmed/coasting target
  world 좌표만 담습니다.
- 수신 기기는 인증 송신자가 현재 선출된 Group Leader이고 Site·Group ID와 Group term이
  일치하며 송신자가 active·spatial ready일 때만 snapshot을 받습니다. 캐시는 queue 없이
  최신값 하나만 보관하고 같은 boot의 중복·역순 sequence를 거부합니다. 최대 5초 안에
  만료하며 현재 production 송신 TTL은 2초입니다.
- 융합 계산을 맡지 않는 기기는 받은 world 좌표를 자신의 committed spatial transform으로
  로컬 좌표에 변환하고 자기 `GET /api/state`에서 제공합니다. 따라서 대시보드는 다른 기기의
  `/api/state`를 직접 요청하지 않습니다. snapshot이 만료되면 로컬 감지 표시로 돌아가며
  잘못된 미재실 권한 이벤트를 만들지 않습니다. 이 표시·state bridge는 아직 로컬 Home
  Assistant entity와 구역 자동화 stream을 대체하지 않습니다.
- `GET /api/state`의 `automation`은 완전한 Group 자동화 snapshot입니다. 정상 Group Leader의
  순방향 승계 중에만 마지막 완전한 값을 `health: recovering`, `available: true`,
  `continuityHeld: true`로 최대 30초 동안 유지할 수 있습니다. `holdAgeMs`, `holdRemainingMs`,
  `continuitySourceLeaderNodeId`, `continuitySourceGroupTerm`으로 고정된 보존 범위와 원본
  권위를 표시합니다. 반복 실패는 이 시간을 연장하면 안 됩니다. 보존 상태는 비권위·읽기 전용이며
  motion은 false이고 새 타깃, Zone 또는 공용 구역 근거를 만들지 않습니다. 소비자는 정상
  5초 source age에 보고된 hold age를 더한 범위까지만 허용합니다.
- 보존 시간이 끝나거나 권위가 역행하거나 설정, 평면도, pose, calibration, membership 또는
  transform이 맞지 않으면 `automation.available`은 false이고 재실, 움직임, 타깃 수와 모든
  Zone 값은 false나 0이 아니라 `null`입니다.
- `GET /api/platform/participants`의 `shadowTransport.fusionSnapshot`에는 제한된
  `received`, `admitted`, `sent`, `rejected`, `expired`, `source`,
  `lastAdmission` 진단값이 표시됩니다.
- `tools/shadow_fusion_compare.py`는 `/api/state`와 Shadow output을 PC에서 읽기 전용으로
  비교합니다. `confirmed`와 `coasting` Shadow track만 target으로 세고 Shadow가 `active`가
  아닌 sample은 불일치 판정에서 제외합니다. 선택적으로 모든 sample을 JSONL로 기록하며
  기기에 history 저장이나 비교 작업을 추가하지 않습니다. 캡처 스키마 버전 2는 활성
  production target 좌표, 크기가 제한된 Shadow track 목록, 주기별 association 진단,
  source 수와 track 간 거리를 보존합니다. 요약은 과다 검출과 과소 검출을 나누고 연속
  과다 검출을 episode로 묶으며, 원인을 candidate pair 없음, 모호한 pairing, 연결되지
  않은 candidate 또는 lifecycle 중첩으로 분류합니다. Production 좌표와 Shadow 좌표는
  별도로 기록하며 서로 같은 관측 좌표계라고 보장할 수 없으므로 localization 오차라고
  단정하지 않습니다.
- `site`는 독립적인 Site Leader snapshot입니다. active 참가자의 생존 상태만 사용하며
  타깃 observation을 받거나 전달하지 않습니다. 현재 Site term에서는 결정적으로 선택된
  Leader 자격 보유 NAMO Node 하나만 이깁니다. Satellite와 quarantined/revoked 참가자는
  Site Leader가 될 수 없습니다.
- `groupRoster`는 로컬 고빈도 Group의 영속 roster metadata만 제공합니다. A/B 저장소에는
  로컬 Node를 포함한 NAMO Node가 최대 7대 들어가며 runtime snapshot은 PSRAM에 둡니다.
  이 endpoint에는 구성원 ID, credential 또는 key를 노출하지 않습니다. Group에 배정되지
  않은 standalone Node에서 `ready: false`는 정상입니다. `storeResult`는 마지막 로컬 load,
  migration 또는 commit 결과를 나타냅니다.
- `siteGroups`의 `assignmentRevision`은 Site Leader가 소유하는 영속 Group 배정
  세대이고, `term`은 현재 Group Leader의 RAM 전용 런타임 lease 세대입니다. 두 값은 서로
  독립적이며 같다고 가정하거나 동등 비교해서는 안 됩니다. Group Leader가 재부팅되어도
  `assignmentRevision`은 바뀌지 않습니다. 살아 있는 peer는 관측한 `term`으로 runtime 선출을
  fence하고 모든 RAM 상태가 사라진 경우에는 새 term을 확정할 수 있습니다. Leader, Floorplan
  또는 예상 구성원 집합이 바뀔 때만
  `assignmentRevision`을 증가시키며 같은 배정의 갱신은 증가시키지 않습니다.
- 로컬 `groups` 항목은 `assignmentRevision`, `canonicalAssignment`,
  `credentialsComplete`를 제공합니다. 확정 Directory 배정이 있는데 Leader fence 또는 필요한
  credential이 완전하지 않으면 production observation과 fusion 권한은 닫힌 상태를 유지합니다.
- 선출된 Site Leader만 확정 Site Directory projection의 권한 원본입니다. 현재 Group
  Leader가 아니더라도 active이고 Leader 자격이 있는 모든 후임 후보는 이 고정 크기
  directory replica를 최신으로 유지합니다. follower는 선출된 Site Leader가 인증해 보낸
  directory에 한해서만 같은 세대 또는 더 오래된 파생 cache를 교체할 수 있습니다.
  canonical Site configuration에서 제거된 Group과 canonical manifest 또는 남은 Group에
  더 이상 등록되지 않은 Floorplan은 사용할 수 없는 route로 남기지 않고 directory에서도
  제거합니다.
- `configurationReady`가 `true`이면 예상 구성원과 Leader admission은 확정된 Site
  roster로 제한합니다. canonical roster는 Leader 자격의 유일한 원본이며, 기기별
  participant registry의 trust는 인증 및 생존 확인 경계로만 사용합니다.
- readiness의 `spaceId`와 `groupId`는 보정 중 제안된 배정입니다. 승인 후에는 Site
  configuration 저장 version 3이 각 NAMO Node의 canonical spatial profile에 두 값을
  영속 저장합니다. coordinator, observation transport, fusion 입력 선택과 Fusion
  Snapshot 배포는 이 canonical 배정을 사용합니다. version 1·2 record는 설정 원본이
  version 3을 게시할 때까지 readiness를 호환 fallback으로만 사용합니다.
- NAMO Node 한 대도 같은 term/lease 계약을 사용하며 `groupId: null`인 미설정
  `standalone` Group으로 표시합니다.
- active이면서 Leader 자격이 있는 NAMO Node만 결정적 Group 선출에서 이길 수
  있습니다. canonical configuration이 있으면 Site와 Group coordinator는 모든 Node에서
  동일한 자격 집합을 사용합니다. participant registry는 canonical configuration이
  생기기 전의 fallback으로만 사용합니다. Satellite는 health 계산에는 포함되지만
  Leader가 될 수 없습니다.
- 온라인인 적격 Node가 없으면 Leader를 비워 두고 health를 degraded로 표시합니다.
  로컬 Node라는 이유만으로 부적격 기기가 자신을 Leader로 승격해서는 안 됩니다.
- heartbeat는 고정 크기 RAM 상태만 사용하고 만료, 중복 및 역순 입력을 거부합니다.
  결정적으로 선택된 로컬 winner만 새 term을 발급합니다. follower는 winner의 인증된 term을
  다시 증가시키지 않고 채택합니다. 활성 Leader의 유효한 lease 중에는 그 Leader가 보낸 더
  낮은 term을 거부하며, 재시작한 후보가 로컬 초기값으로 살아 있는 cluster term을 덮을 수
  없습니다. 모든 Node가 RAM 상태를 잃은 경우에는 독립적인 영속 assignment revision과 term을
  비교하지 않고 제한된 선출로 새 term을 확정할 수 있습니다. 이 전이 자체는 production 감지를
  변경하지 않습니다.
- `health`는 `standalone`, `healthy`, `degraded` 중 하나입니다. 전송 끊김을 빈 방으로
  표현해서는 안 됩니다.
- 인증된 peer heartbeat 송수신기는 일반 펌웨어에서 동작합니다. active이고 Group readiness가
  완료된 참여자의 heartbeat는 이 shadow coordinator에 반영되지만 production fusion
  권한을 부여하지는 않습니다.
- heartbeat wire version 6은 Floorplan ID, 제한된 원본 광고와 독립 Floorplan payload
  revision을 제외한 Site topology revision/digest를 추가합니다. Version 7은 확정 Site
  Directory의 topology revision과 의미 payload digest를 추가합니다. 현재 Group Leader는
  인증 UDP `49191`에서 선출된 Site Leader에게만 summary를 보냅니다. Version 7은 version
  1부터 6까지 계속 읽고, 상대가 최신 fence 지원을 증명하기 전에는 v6, v5 또는 v4를
  주기적으로 전송합니다. summary lease는 RAM에서 만료되고 영속 저장하지 않으며, 로컬
  기기가 Site Leader가 아니게 되면 즉시 지웁니다.
- Site Leader 선출은 유효한 v7 Directory fence, 더 높은 Directory topology revision,
  기존 역할과 Node ID 순서로 후보를 비교합니다. 후보가 여러 개인 기기는 부팅 후 최대
  Site lease 한 번 동안 최신 권위를 확인하며, 모든 peer가 offline이면 그 뒤 제한된
  standalone leadership을 재개합니다. 같은 revision의 Directory digest가 다르면 결정적으로
  선출된 Site Leader의 인증 replica로 follower를 복구합니다.
- heartbeat wire의 `siteTerm`은 현재 Site coordinator term을 전달합니다. 원격 권한을
  채택할 때 term을 한 번 더 증가시키지 않습니다. 만료, 중복 및 역순 입력을 거부하고 활성
  Leader의 유효한 권한 lease 중에는 그 Leader가 보낸 더 낮은 term을 거부합니다. 선택된
  교체 후보가 살아 있는 cluster에서 관측한 term보다 새 term을 먼저 발급하며 follower는
  각자 후임 term을 만들지 않습니다. 공유 Site 설정 commit은 계속 5단계 책임입니다.
  `productionAuthority`는 표준 제품 빌드의 선출된 Group Leader가 active이고 admission을
  통과한 production output을 가질 때만 `true`가 됩니다.

### 9.4.1 Site Configuration 기반

`GET /api/platform/site-configuration`은 영속 저장된 고정 크기 Site 설정 헤더와
canonical roster를 반환합니다. roster는 로컬 NAMO Node를 포함해 최대 20개 participant를
담습니다. 이 값은 설정 상태이며 실시간 endpoint 목록이 아닙니다. 주소와 online 상태는 계속
heartbeat 기반 RAM 상태로 관리합니다.
Schema version 2는 `authorityEpoch`, 16자리 16진수 `configurationDigest`,
`configurationConsistent`와 `replication.autoPendingAuthorityEpoch`를 추가합니다.

현재 구현된 schema는 Floorplan 한 개와 Configuration Owner 한 대만 표현합니다. 목표
다중 Floorplan 구조는 Floorplan마다 독립적인 Configuration Owner를 두지만, 해당 directory는
아직 현재 API에 포함되지 않았습니다. 클라이언트에서 사용하기 전에 새로운 versioned 계약,
저장 migration 및 테스트가 필요합니다.
내부의 제한된 directory codec과 호스트에서 검증한 단일 Floorplan 변환 기반을 추가했습니다.
부팅 시 호환 directory도 예약된 ESP32 transaction flash slot에 저장하지만, 이
endpoint로 공개하지 않으며 현재 schema도 바꾸지 않습니다. 선출된 로컬 Site Leader는
확정된 manifest, 모든 active 구성원의 공간 배정, 제한된 Group 구성과 합의된 Group
Leader가 완전할 때만 이 mirror를 내부 단일 Floorplan projection으로 원자적으로
승격합니다. 명시적인 출처 표시를 사용하므로 이 전환 projection은 현재 설정을 따라
갱신할 수 있지만 이후 정식 다중 Floorplan directory를 덮어쓸 수 없습니다. canonical 활성화 전
페어링·보정 트랜잭션은 참가 Node가 canonical에서 아직 quarantined인 동안 정확한
Configuration Owner를 명시하여 직접 동기화할 수 있습니다. 직접 Owner 관계가 먼저 active가 된
제한 구간도 여기에 포함합니다. canonical 활성화 후에는
명시적·자동 동기화 모두 로컬 Group에 배정된 Floorplan 원본을 선택합니다. 이 배정과 다른
원본을 명시하거나 directory와 다른 Floorplan manifest가 반환되면 거부합니다. 이 내부
전환은 공개 필드나 endpoint를 추가하지 않습니다.

`POST /api/platform/site-configuration`은 다음 form encoded action을 지원합니다.

| Action | 인자 | 동작 |
|---|---|---|
| `claimOwnership` | 없음 | 저장된 owner가 미확정이고 roster도 미확정일 때만 로컬 NAMO Node를 설정 원본으로 명시적으로 지정하고, 로컬의 해제되지 않은 participant registry를 canonical roster로 원자적으로 확정합니다. |
| `commitRoster` | 없음 | configuration owner가 로컬 Node와 revoked되지 않은 로컬 participant를 정렬된 canonical roster로 확정합니다. |
| `commitFloorplan` | `floorplanId`, `geometryVersion`, `coordinateWidthMm`, `coordinateHeightMm` | 현재 저장된 평면도 CONFIG와 IMAGE의 hash를 authoritative manifest로 확정합니다. 좌표 크기는 전체 이미지 좌표계를 mm 단위로 나타내며 payload를 다시 업로드하지 않습니다. |
| `sync` | 선택 `ownerNodeId` | 인증된 pull을 시작합니다. 페어링·보정 중에는 참가 Node가 canonical에서 아직 quarantined인 동안 명시한 ID가 canonical configuration owner와 정확히 같으면 인증된 직접 관계를 통해 동기화할 수 있습니다. 직접 관계가 먼저 active가 된 제한 구간도 포함합니다. 그 밖에는 생략 시 정식 directory의 로컬 Group에 배정된 Floorplan 원본을 사용하고, directory가 없으면 저장된 기존 owner ID를 사용합니다. 명시한 owner는 서로 다른 canonical 배정을 덮어쓸 수 없습니다. |
| `cancelSync` | 없음 | 현재 활성 평면도를 바꾸지 않고 로컬 pull session을 취소합니다. |
| `resolveConflict` | `confirm=1`, `expectedDigest` | 복구 전용 action입니다. 사용자가 원본으로 선택한 로컬 소유 기기에서 roster, manifest와 spatial profile 전체를 유지한 채 authority epoch를 올립니다. 인증된 peer가 실제 설정 충돌을 보고하고 16자리 16진수 digest가 선택한 로컬 snapshot과 여전히 일치할 때만 허용합니다. |

규칙:

- 인증된 canonical NAMO 구성원은 quarantine 상태에서도 Site Configuration을 읽을 수
  있습니다. 이 payload가 active roster로 다시 수렴하는 데 필요하기 때문입니다. 초기 등록으로
  만들어진 로컬 Directory가 아직 compatibility mirror인 동안에는 직접 연결된
  Configuration Owner credential을 통해 compatibility mirror가 아닌 확정 Site Directory를
  읽기 전용으로 받습니다. active 구성원이 2대를 넘고 Owner가 Site Leader가 아니라면 live
  quorum과 선출된 Site Leader의 정확히 같은 revision/digest 광고가 있어야 제공할 수 있습니다.
  이 bootstrap은 이미 존재하는 nonzero canonical Directory를 덮어쓰지 않습니다. revoked
  구성원은 계속 거부하며, 두 예외 모두 평면도, credential, 선출 또는 쓰기 권한을 열지 않습니다.
- online authenticated peer가 같은 fenced revision에 서로 다른 내용을 보고할 때만 공유
  쓰기와 NAMO 활성화는 `site_configuration_conflict`를 반환합니다. peer가 이전·이후
  revision이거나 아직 fence를 보고하지 않은 상태는 제한된 수렴 상태로 처리하고
  `site_configuration_continuity_unavailable`을 반환합니다. commit 전에 peer 충돌이
  사라지거나 선택한 로컬 digest가 바뀌면 `resolveConflict`는
  `site_configuration_conflict_changed`를 반환합니다.
- quorum과 canonical 설정이 일치하지만 대체 자격을 허용한 비-Owner 구성원이 하나도
  없다면 비상 역할 집중 상태에서도 공유 쓰기를 허용합니다. 허용된 후계자가 존재하지만
  offline, stale, credential 불완전 또는 평면도 replica 미완성 상태이면 기존 continuity
  fence를 유지합니다.
- `commitFloorplan`으로 좌표계가 바뀌면 새 좌표계 안에 남는 기존 레이더 transform은
  보존합니다. 좌표계 밖의 transform은 중앙으로 옮기고 calibration revision을 올립니다.
  대시보드는 저장이 끝난 뒤 영향을 받은 모든 기기를 목록으로 표시하고 각 기기의 레이더
  위치 설정 화면으로 이동하는 버튼을 제공합니다.

- 단독 Node는 HACS나 다른 기기 없이 자신이 소유한 1대의 committed roster로 시작합니다.
- 이전 연결에서 혼자 남아 있고 revoked되지 않은 다른 연결이 없는 Node는 확인된 pairing
  과정에서 초대한 Site 정보를 받아 이동할 수 있습니다. 오래된 revoked 기록만으로는
  `site_conflict`를 반환하지 않습니다. 아직 다른 기기를 관리 중인 Node는 기존 연결을
  고립시키지 않도록 이동을 차단합니다.
- pairing은 공유 설정보다 pairwise trust를 먼저 저장합니다. Leader는 자신을 유일한
  configuration owner로 먼저 확정해야 합니다. Joiner는 로컬 Site record를 owner 미확정,
  roster 미확정 상태로 바꾸고 해당 Leader에서 직접 가져온 인증된 canonical
  configuration으로만 owner를 채웁니다. Joiner가 보이는 peer를 owner로 추측해 기록하지 않습니다.
- 같은 두 기기의 역할을 뒤집는 작업은 사용자가 확인한 pairing commit에서만, 양쪽 모두
  다른 revoked되지 않은 연결이 없을 때 허용합니다. 이때 새 Leader는 정확히 그 peer에게
  남아 있던 권한만 회수하고 로컬 평면도 manifest를 유지합니다. 다른 owner 또는 세 번째
  participant가 있으면 `site_conflict`로 중단합니다.
- 마지막 revoked되지 않은 연결을 제거하면 RAM pairing session을 초기화하고 기기를
  로컬 NAMO Node 한 대만 있는 standalone 설정으로 수렴시킵니다. 복구는 기존 `siteId`를
  유지하고 로컬 NAMO Node를 configuration owner로 지정하며, 오래된 roster 구성원을
  제거하고 로컬 membership mode를 `standalone`으로 바꿉니다. 네트워크에서 보이는 기기를
  원본으로 추측하지 않습니다. 로컬 평면도 payload와 manifest, 레이더 위치, zone,
  tuning과 calibration은 유지합니다.
- 이 record가 없던 펌웨어에서 올라온 enrolled Node는 owner가 미확정이고 roster도
  uncommitted인 상태로 시작합니다. 현재 보이는 peer를 owner로 추측하지 않습니다. 사용자는
  `claimOwnership`으로 이 상태를 명시적으로 해결할 수 있으며, 로컬 Node가 Leader 역할로
  새 pairing을 확정할 때도 동일한 제한 조건의 claim을 수행합니다. owner 또는 확정된 roster가
  이미 존재하면 claim은 실패하며 이를 교체하지 않습니다.
- 저장은 data slot 두 개와 commit marker 하나를 사용합니다. 갱신이 실패하면 재부팅 후에도
  직전 committed roster를 읽을 수 있습니다.
- Site configuration 저장 version 2는 roster에 있는 NAMO Node마다 선택적인 canonical
  `spatialProfile`을 추가합니다. Version 3은 이 profile에 승인된 `spaceId`와 `groupId`를
  추가하고 codec workspace를 flash sector 하나인 4 KiB로 제한합니다. 최대 길이 배정
  20개도 이 경계 안에 들어갑니다. Version 1·2 record도 계속 읽을 수 있습니다. 설정 원본이
  다음 reconciliation을 수행할 때 후보 Group이 하나뿐이면 canonical profile로
  승격합니다. 업그레이드된 record의 배정 ID가 비어 있으면 설정 원본은 부팅 중에도 같은
  reconciliation을 수행합니다. pose와 calibration revision이 일치할 때는 `spaceId`와
  `groupId`만 채우며 승인된 transform과 revision은 바꾸지 않습니다. 로컬 Group 후보가
  여러 개이면 추측하지 않습니다. profile이 없거나 배정이 모호하면 spatial admission은
  fail-closed로 멈춥니다.
- 저장 version 4는 `authorityEpoch`를 추가합니다. Version 1부터 3까지는 epoch 1로 계속
  읽습니다. 권한 revision은 `(authorityEpoch, generation)` 순서로 비교하므로 이전 epoch의
  원본이 더 큰 generation을 내더라도 권한을 다시 가져갈 수 없습니다.
- 설정 원본 기기는 승인된 보정의 양쪽 transform을 canonical Site record에 함께 반영합니다.
  roster 갱신과 자동 설정 원본 승계는 이 profile을 그대로 보존합니다. 더 새롭게 승인된
  calibration은 profile을 교체할 수 있지만, 같은 version의 소유권 변경이 새 원본 기기의
  보정 전 로컬 배치로 조용히 되돌리면 안 됩니다. 자동 승계가 성공하면 새 설정 원본은
  부팅 때와 같은 배정 전용 reconciliation을 즉시 실행하므로, 기존 profile의 배정 ID가
  빈 상태로 남지 않습니다.
- Site configuration과 평면도 payload는 UDP 49192의 HMAC 인증 패킷으로 전송합니다.
  payload body는 최대 384 byte입니다. follower는 CONFIG와 IMAGE를 비활성 flash slot에 바로
  기록하고 두 hash를 검증한 뒤 한 번의 header commit으로 두 active slot을 함께 바꿉니다.
  전체 평면도 이미지를 RAM이나 HTTP task stack에 올리지 않습니다.
- Configuration Owner는 인증된 평면도 follower lease를 한 번에 하나만 제공합니다. 다른
  follower는 RAM 대기열에 저장되지 않고 busy 응답을 받은 뒤 기존의 결정적인 slot과
  제한된 전체 session backoff로 다시 시도합니다. `replication.source`는 현재 요청자,
  2.5초 lease 잔여 시간과 accepted·busy·completed·expired 누적값을 제공합니다. wire version
  1 호환성을 위해 busy는 기존 `storage_failed` status와 인증된 예약 payload hash
  `0x42555359` 조합으로 표현합니다. 작은 Site directory 요청은 이 평면도 lease를 사용하지 않습니다.
- storage header version 1은 legacy 이미지를 겹치지 않는 두 번째 image slot으로 먼저
  옮긴 뒤 version 2를 확정합니다. migration이 중단되면 version 1 header가 계속 원본입니다.
- 로컬 participant membership 변경이 성공하면 configuration owner는 canonical roster도
  자동 갱신합니다. activation 뒤 roster commit이 실패하면 participant를 다시 quarantine하고
  복구에 필요한 인증 제어 경로 credential은 유지합니다.
- 인증된 canonical replica는 readiness 또는 calibration을 교체하는 동안 수신 NAMO Node를
  quarantined 상태로 유지할 수 있습니다. canonical roster에 남아 있고 revoked되지 않은 로컬
  Node는 이 control-plane 갱신을 받을 수 있지만, 최종 active roster를 복제하기 전까지
  heartbeat Leader 선출, observation admission 및 fusion에서는 제외됩니다. roster에서
  누락되었거나 revoked된 Node는 복제를 거부합니다.
- 각 Node는 configuration owner에서 직접 가져옵니다. Site Leader가 configuration owner와
  같은 기기인 경우가 아니면 이미지 chunk를 중계하지 않습니다.
- 같은 UDP 49192 transport는 제한된 Site Directory를 별도 `site_directory` 작업으로
  전송합니다. 평상시에는 선출된 Site Leader만 확정 canonical Directory를 제공하며 모든
  active NAMO Node가 로컬 사본을 유지합니다. 초기 등록에서는 활성화 전에 이미 인증된
  Configuration Owner를 읽기 전용 원본으로 사용합니다. active 구성원이 2대를 넘으면 그
  Owner가 선출된 Site Leader이거나, live quorum 아래의 선출된 Site Leader가 정확히 같은
  Directory를 광고해야 합니다. active 구성원 1~2대의 초기화 복구 fallback은 Owner 또는
  유일한 canonical peer로 제한합니다.
- follower는 30초의 저빈도 간격으로 선출된 Site Leader를 확인합니다. bootstrap과 일반
  요청은 결정적인 2.5초 roster slot과 제한된 backoff를 사용합니다. bootstrap은 로컬
  compatibility mirror에 확정된 non-compatibility Directory만 적용하며, 기존 nonzero
  same-revision 충돌은 계속 fail-closed 처리합니다. mirror의 topology revision은 Site
  Configuration roster에서 파생된 값이므로 canonical Directory authority revision과
  비교하지 않습니다. 이 수렴에는 별도 POST action이나 설정 화면이 없습니다.
- 자동 설정 원본 후임은 임의로 고르지 않습니다. 모든 Node가 현재 원본을 제외한 active,
  Leader 가능 NAMO Node 중 Node ID가 가장 작은 기기를 동일하게 선택합니다. 후임은 Site
  Leader이고, 완전한 로컬 평면도 replica와 모든 active 구성원의 pairwise credential을
  보유한 상태를 30초 동안 유지해야 원본을 승계합니다.
- 여러 Node가 있는 Site의 설정 원본은 결정적 후임이 online이고 현재 generation, 평면도
  replica와 필요한 active 구성원 credential을 모두 보유했을 때만 공유 쓰기를 허용합니다.
  감지와 읽기는 계속되며, 손상된 관계의 quarantine, revoke, forget은 복구 경로로 유지합니다.
- 공간 보정 중 canonical configure 또는 activation revision이 바뀌면 대시보드는 새로
  연결하는 기기의 replica뿐 아니라 기존 online active peer 전체가 새 revision을 확인해
  설정 원본의 `sharedWritesAllowed`가 다시 열릴 때까지 기다린 뒤 다음 mutation을 시작합니다.
  이 수렴 대기는 제한 시간 안에서만 수행하며 실제 digest 충돌을 자동 덮어쓰지 않습니다.
- 자동 승계가 성공하면 한 번의 transaction에서 `authorityEpoch`를 올리고 설정 원본과
  roster revision만 바꿉니다. canonical roster와 평면도 manifest는 그대로 유지합니다.
- 후보 생성 또는 영속 저장 실패는 같은 의미상 authority fingerprint 아래에서 제한된 jitter
  retry를 예약합니다. 재시도 간격에는 상한이 있고 기한 전 같은 평가는 flash를 다시 쓰지 않으며,
  eligibility나 fingerprint가 바뀌면 안정화 fence를 다시 시작합니다. client가 무관한 mutation을
  다시 실행해 복구를 흉내 내면 안 됩니다.
- owner에서 공유 이미지, 방 geometry, scale, wall, 이름, occlusion 또는 가구 저장이
  성공하면 대시보드는 현재 CONFIG/IMAGE hash를 authoritative manifest로 자동 확정합니다.
  레이더 pose, zone, LD2410C tuning과 calibration은 로컬에 남으며 Site generation을
  발행하지 않습니다. 동일한 manifest 재확정은 generation을 올리지 않습니다.
- 더 높은 owner generation을 발견한 follower는 canonical roster 순서에 따라 2.5초
  간격 slot에서 자동 pull을 시작합니다. follower 19대의 최초 요청은 최대 47.5초에 걸쳐
  분산되며 canonical roster에 없는 Node는 자동 동기화하지 않습니다.
- 전체 전송 실패는 2.5, 5, 10, 20, 30초 뒤 재시도한 다음 해당 generation의 자동
  재시도를 중단합니다. 더 새 generation 또는 명시적인 `sync`는 새 시도를 엽니다. 개별
  packet 재시도 한도는 이와 별도로 유지합니다.
- `autoPendingGeneration`, `autoStartInMs`, `autoRetryCount`는 저장되지 않는 선택적 runtime
  진단값입니다.
- heartbeat wire version 5는 authority epoch, generation과 결정적인 64-bit 설정 digest를
  전송합니다. online active peer가 이 세 값을 정확히 보고해야 Site·Group 선출, 공유 쓰기,
  directory reconciliation과 fusion에 참여할 수 있습니다. revision 지연은 bounded
  convergence로 처리하고, 같은 fenced revision의 digest 불일치만
  `configuration_conflict`로 분류합니다. 로컬 감지는 계속하지만 제어 경로는 fail-closed로
  멈춥니다.
- 순차 펌웨어 업데이트 중에는 상대가 v5 지원을 증명할 때까지 최신 기기가 기존 v4
  heartbeat도 주기적으로 보냅니다. 따라서 구형 기기가 최신 기기를 offline으로 오판하지
  않습니다. 이후 도착한 v4 호환 패킷은 생존과 replica 정보만 갱신하며, 이미 확인한 v5
  authority fence를 지우지 못합니다.
- heartbeat wire version 6은 v5 설정 fence를 유지하면서 Site topology fence와 제한된
  Floorplan routing metadata를 추가합니다. Version 7은 확정 Site Directory topology
  revision과 의미 digest를 추가합니다. Site Leader 선출은 이 fence를 사용해 재부팅한
  오래된 Directory 보유자가 최신 online 후보보다 먼저 권위를 얻지 못하게 합니다. v7
  Node는 상대가 v7 지원을 증명할 때까지 v6도 주기적으로 전송합니다.
- heartbeat wire version 8은 v7 Directory fence를 유지하고 로컬 canonical credential
  준비 판정을 추가합니다. 현재 설정 원본은 결정적 후임이 필요한 credential을 모두
  보유했다는 v8 heartbeat를 인증하기 전까지 공유 쓰기를 닫아 둡니다. v8 Node는 상대가
  v8 지원을 증명할 때까지 v7 heartbeat도 주기적으로 전송합니다.
- heartbeat wire version 9는 v8을 유지하고 제한된 Site 및 Group election-view digest를
  추가합니다. 이 버전의 기존 Group digest에는 각 구성원의 credential 준비 bit가 포함됩니다.
- heartbeat wire version 10은 credential 준비 상태를 Group Leader 후보 자격과 권위 admission의
  필수 조건으로 유지하지만, 순간적으로 변하는 이 bit는 Group election-view identity에서
  제외합니다. v10 Group digest에는 현재 살아 있는 canonical 구성원 집합과 Directory 배정
  qualifier가 계속 포함되므로 비대칭 네트워크 분할이나 오래된 배정은 권위를 얻을 수 없습니다.
  순차 업데이트 중 v10 Node는 상대가 v10 지원을 증명할 때까지 v9 호환 digest를 전송합니다.
  짧은 view 불일치 구간에는 외부 Group 권위를 비우며, 재부팅하지 않은 정확히 같은 Leader가
  돌아오면 보류한 term을 복원하고 실제 Leader가 교체되면 term을 증가시킵니다.
- heartbeat wire version 11은 Site와 Group election-view digest를 결정적으로 선택된 winner에
  결합합니다. 선택된 로컬 winner만 새 role term을 발급할 수 있고 follower는 cluster term을
  전달할 수는 있지만 선택된 winner가 보낸 term만 채택합니다. 보류했던 같은 Leader를 다시
  선출하면 term을 유지하고 실제 교체일 때만 정확히 한 번 증가합니다. 따라서 모든 follower가
  term을 각각 증가시키거나 같은 구성원 view에서 서로 다른 Leader를 선택한 채 상호 승인하는
  상황을 막습니다. 순차 업데이트 중 v11 Node는 상대가 v11 지원을 증명할 때까지 v10 호환
  digest를 전송합니다.
- heartbeat wire version 12는 winner가 결합된 Site와 Group election identity를 설정된 전체
  canonical roster와 roster revision으로 안정화합니다. heartbeat 도착 순서, 일시적인 생존 상태,
  로컬 Directory 복제 및 credential 준비 상태는 더 이상 이 identity를 바꾸지 않으며 각각
  독립된 authority admission fence로 남습니다. v12 Node는 상대가 v12 지원을 증명할 때까지
  정확한 v11 winner 결합 호환 digest를 전송하므로 1~7대 순차 업데이트에서 서로 다른 두
  계약을 같은 wire version으로 오해하지 않습니다.
- 현재 wire version의 active Node가 같은 authority epoch 안에서 generation과 roster revision이
  정확히 한 단계만 차이 나는 전환 상태라면, credential이 완전하고 Floorplan geometry·hash와
  radar pose가 canonical spatial profile에 정확히 맞을 때에만 heartbeat와 기존 fusion을 계속
  사용할 수 있습니다. 이때 `configurationConsistent`와 공유 쓰기는 정확한 revision 수렴 전까지
  계속 닫혀 있고 원본 광고와 Group 요약도 받지 않습니다. epoch 변경, 두 단계 이상 차이, 구형
  wire version 또는 공간 계약 불일치는 계속 `configuration_conflict`로 처리합니다.
- revision은 authority epoch를 generation보다 먼저 비교합니다. 같은 fenced revision의
  내용이 다르면 `revision_conflict`, 더 오래된 fenced revision은 거부합니다. online active
  peer가 다른 fence를 보고하거나 아직 v5 fence를 보고하지 않으면
  `configurationConsistent`는 `false`입니다.
- `resolveConflict`는 대시보드에 노출하지 않습니다.
  `tools/site_configuration_conflict_repair.py`가 양쪽을 먼저 백업하고 명시적으로 선택한
  원본에서만 epoch를 갱신한 뒤 owner, revision, digest와 Floorplan의 정확한 수렴을 확인합니다.
- 레이더 배치는 Node별 sidecar로 유지합니다. 공유 geometry를 복제해 읽을 때 owner의 pose를
  복사하지 않고 해당 Node의 로컬 레이더 pose를 덮어 적용합니다.
- NAMO Node는 인증된 online heartbeat가 canonical generation, geometry version, CONFIG
  hash와 IMAGE hash의 정확한 일치를 보고해야 active로 전환할 수 있습니다. presence-only
  Satellite는 기존 readiness 계약을 유지합니다.
- 이 API 하나만으로 production gate를 우회할 수 없습니다. 공간 권한에는 canonical Floorplan
  replica, 최신 radar pose와 calibration, readiness, active roster membership, credential 및
  현재 Group authority가 모두 필요합니다.

### 9.5 안전한 Community 기기 연결

성숙한 Site에 새 기기를 등록할 때만 `authenticated_enrollment` 복제 모드를
사용합니다. 미확정 단일 구성원 Joiner에 저장된 활성 pairwise credential의 상대가
원격 canonical owner이고, 원격 roster가 로컬 Node를 유지할 때에만 높은 authority
epoch를 한 번 받아들입니다. 수렴 후에는 이 예외가 닫히며 일반 복제와 자동 Owner
승계는 기존의 엄격한 epoch fence를 그대로 사용합니다.

`GET /api/platform/site-configuration`의 `replication`에는 `mode`와
`validationResult`가 포함됩니다. 검증 결과는 전송 시작 시 `not_evaluated`로
초기화되고 payload 해석 후와 transaction commit 직전에 다시 갱신됩니다.

확정된 Site에 기기를 추가할 때에는 현재 Configuration Owner의 저장된 평면도를
항상 원본으로 유지합니다. 상대 평면도 선택은 단일 구성원 Site의 첫 연결에서만
허용합니다.

`commitResult=site_configuration_owner_required`는 현재 기기가 설정 원본이
아님을 뜻합니다. 대시보드는 잘못된 기기에서 연결을 계속하지 않고 온라인 설정
원본 기기로 이동시킵니다. 상대가 commit했지만 HTTP 응답만 유실된 경우에는 상대
상태를 확인한 뒤 계속하고, 확인할 수 없으면 로컬 quarantine 정보를 지우지 않은
채 재개 가능한 상태로 남깁니다. 이미 수렴한 Joiner의 연결 재시도는 canonical Site
설정을 다시 미확정 상태로 초기화하지 않습니다.

`GET /api/platform/pairing`은 RAM에만 존재하는 고정 크기 연결 session 하나를
반환합니다. 응답에는 공개 offer와 proof만 포함합니다. ECDH private key, 파생 master
key, transport key와 participant token은 절대 반환하지 않습니다.

연결 요청이 있으면 응답의 `invitation`에는 `state`, 요청 기기 주소와 남은 시간이
포함됩니다. 요청이 없으면 `invitation`은 `null`입니다.

`POST /api/platform/pairing`은 form encoded action을 받습니다.

| Action | 필수 인자 | 동작 |
|---|---|---|
| `invite` | `requesterAddress`, 선택 `durationSeconds=30..180` | 등록 창이나 암호화를 시작하지 않고 고정 크기 RAM 연결 알림 하나만 저장합니다. dashboard는 180초를 사용합니다. |
| `rejectInvite` | 없음 | 요청한 설정 마법사가 대기를 끝낼 수 있도록 현재 알림을 짧은 시간 동안 거절 상태로 둡니다. |
| `dismissInvite` | 없음 | 연결 및 등록 상태를 바꾸지 않고 RAM 알림을 지웁니다. |
| `prepare` | `role=leader|joiner`, 선택 `durationSeconds=30..300` | 로컬 등록 창이 열린 동안 새 P-256 offer를 만듭니다. |
| `exchange` | 상대 기기의 `offer` 전체 | P-256 ECDH와 HKDF-SHA256으로 transcript에 결합된 pair key를 만듭니다. |
| `verify` | 상대 기기의 `proof` | transcript key를 실제로 가지고 있는지 HMAC으로 확인합니다. |
| `confirm` | 6자리 `confirmationCode` | 상대 기기를 `community_unverified`와 quarantined 상태로 저장하고, 인증된 제어 경로에 사용할 pairwise credential을 활성화합니다. |
| `cancel` | 없음 | RAM session과 secret을 지웁니다. |

암호화 pairing 작업을 받으면 HTTP `200`, `accepted: true`, `operationId`를 반환합니다. 현재
ESPHome IDF web server wrapper가 지원하지 않는 응답 코드를 `500`으로 바꾸기 때문에
의도적으로 `200`을 사용합니다. 호출자는 GET 응답에서 `completedOperationId`가 같아지고
`busy=false`가 될 때까지 확인합니다. ECDH와 저장
작업은 ESP-IDF `httpd` task가 아니라 component loop에서 실행합니다.

mutation이 접수된 뒤 일시적인 GET 실패가 발생해도 작업 실패가 확정된 것은 아닙니다. 호출자는
제한된 backoff로 상태 조회를 다시 시도한 뒤 최종 commit 상태를 확인해야 합니다. 상태 조회 한 번이
실패했다는 이유만으로 mutation을 다시 실행하면 안 됩니다. 이 복원력 규칙은 pairing뿐 아니라
operation ID를 제공하는 다른 접수형 API에도 적용합니다.

`commitResult`는 상태 전이 정책과 저장 실패를 구분합니다. `site_conflict`는 다른 활성
연결 또는 관련 없는 configuration owner 때문에 요청한 이동을 진행할 수 없다는
뜻입니다. `site_state_invalid`는 저장된 Site 상태로 유효한 최종 roster를 만들 수 없다는
뜻입니다. `persistence_failed`는 실제 transaction 저장 쓰기가 실패한 경우에만 사용합니다.

독립 Site 복구가 진행 중이거나 실패한 상태에서 연결을 시작하거나 진행하는
action을 요청하면 HTTP `503`과 `site_reconciliation_pending` 또는
`site_reconciliation_failed`를 반환합니다. RAM 상태를 언제든 정리할 수 있도록
`cancel`, `rejectInvite`, `dismissInvite`는 계속 허용합니다. 복구는 component
loop에서 한 번만 실행하며 실패 후 flash 쓰기를 자동으로 반복하지 않습니다.

연결 알림 action은 작은 RAM 상태만 즉시 바꾸므로 `operationId`를 반환하지 않습니다.
알림은 최대 하나만 유지하고 180초 안에 만료되며 flash에 저장하지 않습니다. 알림 수락은
기존 등록 및 보안 연결 절차를 시작할 뿐이고, 거절하거나 만료되면 어떠한 신뢰 권한도
생기지 않습니다.

두 기기 모두 등록 창을 열고 서로 반대 역할을 선택해야 합니다. Proof 교환 후 양쪽
대시보드에 같은 6자리 코드가 보여야 하며 각 기기에서 사용자가 따로 확인해야 합니다.
양쪽 확인이 성공하면 동일한 pairwise token/key와 상대 Node ID를 저장합니다. Joiner는
registry와 credential 저장이 성공한 뒤에만 Leader의 Site ID를 적용합니다. 작업이
중단되거나 일부 저장이 실패하면 active 권한 없이 quarantined 상태로 남습니다.

양쪽 pairing 확인 후 대시보드는 현재 Site revision의 복제를 먼저 요청하고
Configuration Owner의 `sharedWritesAllowed`가 열릴 때까지 기다린 뒤 선택한 평면도
manifest를 확정합니다. 그 commit이 다시 한 번 continuity 전환과 겹친 경우에만
대기 후 한 번 재시도하며, 다른 오류는 자동으로 재시도하지 않습니다. 새로 확정된
revision은 기존의 제한 시간 수렴 검사를 사용해 Joiner에 동기화합니다.

이 흐름의 trust는 `community_unverified`입니다. 제조 출처를 증명하지 않으며
`official_verified`를 만들 수 없습니다. 6자리 코드는 일반적인 key exchange 불일치를
찾지만 대시보드는 여전히 로컬 HTTP로 제공됩니다. Attestation과 channel 보호가
추가되기 전에는 신뢰할 수 있는 LAN에서 두 기기 페이지를 독립적으로 열고 코드를
비교해야 합니다.

기기 연결과 활성화는 별도 단계입니다. Spatial Node는 capability, assignment, pose,
calibration readiness를 먼저 갖춰야 합니다. Pairing credential의 유효성은 인증 상태이고,
quarantined membership은 실제 동작 권한을 차단하는 gate입니다. `activate`는 readiness와
replica 검사를 통과한 뒤 registry membership만 active로 바꿉니다. 재설정과 quarantine은
제한된 제어 경로 동기화를 위해 credential을 유지하며, revoke는 비활성화하고 forget은 삭제합니다. Standalone 감지와 Home Assistant는
이 API에 의존하지 않습니다. 연결만으로 production fusion 권한을 부여하지 않으며
`fusionEnabled`는 제품 빌드에 포함된 capability만 나타냅니다.

대시보드의 연동 해제는 `revoke`와 `forget`이 로컬 레지스트리 동작이라는 경계를
유지하면서 양쪽을 순서대로 정리합니다. 자동 발견 또는 수동 입력으로 상대 주소를
얻고 `/api/platform/descriptor`의 participant ID가 선택한 기기와 일치하는지 확인한
뒤, 상대 기기에서 이 로컬 Node를 먼저 삭제하고 삭제 결과를 확인합니다. 그 다음에만
요청을 시작한 기기에서 상대 기록을 삭제합니다. 상대쪽 확인 전에 실패하면 요청을
시작한 기기의 기록을 재시도 기준으로 남깁니다. 양쪽 레지스트리에서 관계가 모두
사라지기 전에는 성공으로 표시하지 않으므로, 연동 해제를 끝내려면 상대 기기가
온라인이어야 합니다. 마지막 관계를 제거하면 RAM에만 있는 pairing session도 `idle`로
되돌리고 위의 단독 Site 정규화 규칙을 적용합니다.

### 9.6 요청형 로컬 기기 발견

`GET /api/platform/discovery`는 가장 최근에 완료된 제한 크기의 `_namo._tcp` query
결과를 반환합니다. `POST /api/platform/discovery`는 form encoded `action=scan`을 받아
최대 3초 안에 끝나는 비동기 PTR query 하나를 시작합니다. 요청을 받으면 HTTP `200`과
`accepted: true`를 반환하며, 호출자는 GET 응답의 `scanning`이 `false`가 될 때까지
확인합니다. Query가 실행 중일 때 같은 요청이 다시 들어와도 성공으로 처리하며 discovery가
영구적으로 busy 상태에 남지 않습니다.

규칙:

- 펌웨어는 안정적인 identity가 준비되면 `_namo._tcp`를 광고하지만, 로컬 query 시작은
  광고 등록 성공 여부에 의존하지 않습니다.
- HTTP task가 아니라 component loop가 비동기 mDNS query를 시작하고 확인합니다. 시작이
  실패하면 제한된 시간 동안만 재시도하며 scan의 busy 상태는 반드시 해제합니다.
- 사용자 검색이 이미 실행 중인 endpoint 복구 query와 겹치면 새 query를 만들지 않고 기존
  query를 화면에 보이는 `scanning` 상태로 승격합니다. 내부 `idle` 결과를 오류로 노출하지 않습니다.
- 완료된 결과는 mDNS query와 결과 목록을 해제하기 전에 고정 크기 runtime 저장소로
  복사합니다. 원격 후보는 최대 19개까지만 남깁니다.
- 안정적인 `participantId`로 중복을 제거합니다. 자기 자신, 잘못된 TXT record, 호환되지
  않는 protocol 범위, NAMO Node가 아닌 participant type, 다른 product, IPv4 주소를
  확인할 수 없는 후보, 준비되지 않은 identity는 제외합니다.
- 발견 결과는 주소만 제공합니다. Pairing window를 열거나, trust를 승인하거나, secret을
  교환하거나, participant를 등록하거나, fusion을 활성화하지 않습니다.
- 대시보드는 선택한 후보 주소를 기존 보안 기기 연결 흐름에 전달할 수 있습니다. Multicast
  발견을 사용할 수 없는 환경을 위해 수동 주소 입력은 계속 제공합니다.
- Query가 완료될 때마다 이전 고정 크기 RAM snapshot을 교체합니다. Flash에는 저장하지
  않으며 retry 빈도도 제한합니다.

## 10. 호환성과 기존 응답 전환

기존 응답 필드는 펌웨어와 대시보드가 구조화된 대체 필드를 모두 지원할 때까지 유지합니다.

기존 endpoint를 구조화된 응답으로 전환할 때는 다음 순서를 따릅니다.

1. 기존 응답 옆에 `code`, `severity`, `detail`을 추가합니다.
2. mock/demo API도 같은 필드를 반환하게 합니다.
3. 프론트엔드는 code 기반 문구를 우선 사용하게 바꿉니다.
4. 새 경로가 안정화될 때까지 legacy 필드를 유지합니다.
5. 대시보드와 펌웨어 검증 후 legacy 의존을 제거합니다.

---

## 11. 기존 Endpoint 전환 순서

문자열 기반 필드에 의존하는 기존 endpoint는 다음 순서로 전환합니다.

1. 진단 및 디버그 API
2. 보정 상태
3. 평면도 업로드 상태 및 오류
4. setup 및 Wi-Fi handoff 상태
5. OTA 업로드 및 재응답 확인 상태
6. `/api/system/status`

다음 순서는 호환성 위험을 줄이기 위한 것입니다.

- 디버그와 보정 동작은 다른 기능과 비교적 독립적입니다.
- 업로드, setup, OTA 흐름은 상태 전환이 길기 때문에 구조화된 응답 패턴을 먼저 검증해야 합니다.
- `/api/system/status`는 가장 많은 클라이언트가 사용하므로 마지막에 전환합니다.

---

## 12. 설정값 참조

Software zone type:

| 값 | 의미 |
| --- | --- |
| `detection` | 구역 안의 target을 zone presence로 계산합니다. |
| `filter` | 구역 안의 target을 오탐 후보로 보고 차단합니다. |
| `reduced` | 새 후보는 PIR 근거가 생기거나 같은 트랙이 구역을 벗어날 때까지 잠정 상태로 둡니다. 이미 확정된 트랙은 계속 유효합니다. |
| `disabled` | 구역을 설정에는 보존하지만 감지에는 사용하지 않습니다. |
| `exit` | 사람이 감지 영역을 나가는 경로 후보를 표시합니다. 이것은 exit evidence이며 filter가 아닙니다. |

Calibration zone type:

| 값 | 의미 |
| --- | --- |
| `filter` | 반복되는 오탐 후보를 차단합니다. |
| `reduced` | 새 후보는 PIR 근거가 생기거나 같은 트랙이 구역을 벗어날 때까지 잠정 상태로 둡니다. 이미 확정된 트랙은 계속 유효합니다. |
| `disabled` | 보정 구역을 설정에는 보존하지만 사용하지 않습니다. |

`exit`은 software zone type 전용입니다. calibration zone type으로 취급하면 안 되며
그 자체로 target을 제거하면 안 됩니다.

구역이 겹치면 차단 판정이 항상 우선합니다. 그다음에는 사용자 설정 출처를 진단 정보에
유지하기 위해 software `reduced`가 calibration `reduced`보다 우선하며, 두 유형은
동일한 잠정 admission 동작을 사용합니다.

---

## 13. 호환되지 않는 구현

다음 변경은 이 계약을 위반합니다.

- 버전이 명시된 전환 절차 없이 기존 응답 필드를 제거하거나 호환성을 깨는 변경
- 표시 문자열에 의존하는 클라이언트 로직
- API 응답에 포함된 번역된 펌웨어 사용자 문구
- 문장 형태의 status code
- 일반 status 응답에 포함된 큰 로그
- 다국어 작업에 함께 포함된 endpoint 의미 변경
- peer 기기 사이에서 교환되는 사용자 표시 문장
- 명시적인 geometry 설정이 없는 다중 센서 좌표 융합
- filter zone으로 해석되는 exit zone

---

## 14. 기본 상태 응답

Endpoint별 절에서 다른 schema를 정의하지 않았다면 성공한 상태 응답은 다음 구조를 사용합니다.

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

기존 문자열 필드는 호환을 위해 유지할 수 있습니다. 새 클라이언트는 `code`를 제어 값으로 사용합니다.

---

## 15. LD2410C 재실 보조 규약

`GET /api/state`의 `debug.staticRadar`는 LD2410C 입력과 재실 보조 상태를 제공합니다.
`detectionDistanceMm`은 LD2410C가 보고한 단일 최종 감지 거리에 최근
3개 중앙값 필터를 적용한 값입니다. 1초에 최대 한 개의 거리만 중앙값 이력에
넣으며, 짧은 유효하지 않은 거리값은 마지막 안정값을 덮어쓰지 않습니다.
보조 레이더 재실이 해제되거나 센서가 없어지면 필터 상태를 초기화합니다.
`movingDistanceMm`과 `stillDistanceMm`은 진단용 원시값이며 클라이언트가
두 값을 조합해 타깃 위치를 추론하면 안 됩니다.
`assist` 객체는 `armed`, `active`, `armPending`, `armElapsedMs`, `exitVeto`와 선택적인
`heldTarget` 스냅샷을 포함합니다.
LD2410C 타깃 위치를 표시하는 클라이언트는 `presence`와 단일
`detectionDistanceMm`만 사용해야 합니다.
`debug.presenceEvidence`는 PIR, LD2450 트래커, LD2410C 보조 증거를 각각 표시합니다.

LD2410C는 PIR 또는 확정된 LD2450 트랙으로 이미 시작된 재실 세션만 유지할 수 있습니다.
LD2410C 단독으로 재실을 새로 켜면 안 되며 움직임, 활성 타깃 좌표와 개수, 방 상태,
구역 재실 또는 히트맵 샘플을 만들면 안 됩니다.
현재 drop 이벤트가 `lost_after_exit` 또는 `lost_after_room_exit`이면 해당 소실에 대해 새로운 정적 레이더 hold를 시작하지 않습니다.
LD2410C가 없거나 통신할 수 없으면 `available: false`로 처리하고 기존 PIR 및 LD2450 동작을 그대로 유지합니다.

보조 유지가 활성화된 동안 `assist.heldTarget`은 정확히 하나였던 마지막 확정 또는 코스팅
LD2450 트랙의 좌표를 표시 목적으로만 보존할 수 있습니다. 명확한 이전 공간 타깃이 없으면
`null`입니다. 이 스냅샷은 활성 타깃이 아니며 타깃 수, 구역, 방, 보정 또는 히트맵 계산에
포함하면 안 됩니다.

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
`staticDistanceMm`과 `distanceDeltaMm`은 `null`이고 `distanceMatched`는 `false`입니다. 대시보드는 거리 일치
스냅샷을 기존 타깃 모습으로, 불일치 스냅샷을 위치 불확실 상태로 표시할 수 있습니다.

리플레이의 `tg` 튜플은 다음 순서로 트래커 입력을 기록합니다.

```text
[valid, xMm, yMm, speedCmS, distanceMm, reducedZone, filterBlocked]
```

`filterBlocked=1`인 관측값은 기존 트랙과 연결하기 위한 후보로만 남으며,
트랙을 생성하거나 갱신하거나 사용자에게 노출할 수 없습니다.
리더는 기존 5개 및 6개짜리 튜플도 받아야 하며 빠진 플래그는 `false`로 처리합니다.

리플레이의 선택적 `sr` 튜플은 다음 순서로 정적 레이더 근거를 기록합니다.

```text
[available, presence, moving, still, movingDistanceMm, stillDistanceMm, movingEnergy, stillEnergy, detectionDistanceMm]
```

리더는 기존 8개짜리 튜플도 받아야 하며 빠진 `detectionDistanceMm`은 `0`으로
처리합니다.

리플레이의 선택적 `sf` 튜플은 다음 순서로 융합 상태를 기록합니다.

```text
[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending, exitVeto, armElapsedMs]
```
