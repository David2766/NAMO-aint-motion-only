# NAMO Satellite 규약

상태: **점진적으로 구현 중인 논리 계약 초안. 호스트용 V1 fixture, validation, bounded simulator,
MessagePack 선택, 인증된 768바이트 observation envelope와 golden vector, 안정적인 identity,
크기가 제한된 registry 영속 저장, 읽기 전용 descriptor 발견 및 제한된 로컬 registry 관리
API와 인증된 Shadow transport까지 구현했습니다. Community NAMO Node 사이의 안전한 pairing과
pairwise credential 발급뿐 아니라 명시적으로 활성화하는 NAMO 간 production fusion 경로도
구현했습니다. 외부 Satellite 가입, production membership key rotation 및 공식 기기 attestation
검증은 아직 구현하지 않았습니다.**

영문 문서: [satellite-protocol.md](satellite-protocol.md)

이 문서는 승인된 외부 Satellite와 NAMO Node가 NAMO 공간 플랫폼 Site 안에서 observation을
교환하기 위한 논리 계약을 정의합니다. 필드 이름과 예시는 설계 후보이며 각 절에서 구현된 읽기 전용
표면이라고 명시한 경우를 제외하면 현재 사용할 수 있는 HTTP endpoint나 펌웨어 동작을 설명하지 않습니다.

## 1. 프로토콜 원칙

- Satellite는 최종 사용자 상태가 아니라 observation을 제공합니다.
- Satellite는 NAMO Node가 아니며 Leader나 설정 replica가 될 수 없습니다.
- 제품 이름과 attestation 상태로 observation capability를 추론하지 않습니다.
- Site가 소속과 신뢰를 부여하며 참여자가 스스로 권한을 줄 수 없습니다.
- 공간 observation은 자신의 좌표계와 보정 버전을 명시합니다.
- 모든 stream은 빈도, 항목 수, TTL 및 payload 크기를 제한합니다.
- 오래되거나 중복되거나 인증되지 않았거나 버전이 맞지 않는 observation은 fusion 전에 거절합니다.
- 같은 계약으로 standalone NAMO Node 한 대와 Group Leader 한 대 및 다른 NAMO Node 또는
  Satellite 최대 6대로 제한된 Group을 지원합니다.

## 2. 참여자 및 메시지 식별 정보

| 필드 | 목적 |
| --- | --- |
| `protocolVersion` | wire contract의 major version. |
| `siteId` | pairing 과정에서 Site가 부여합니다. |
| `participantId` | NAMO Node 또는 Satellite의 안정적인 식별자. IP 주소를 사용하지 않습니다. |
| `participantType` | `namo_node` 또는 `satellite`. 등록 후 바꿀 수 없습니다. |
| `bootId` | process 또는 기기 부팅마다 변경됩니다. |
| `sequence` | 하나의 `bootId`와 stream 안에서 단조 증가합니다. |
| `groupId` | observation을 받는 Fusion Group. |
| `observedAtMs` | 송신자 기준 monotonic observation time. |
| `ttlMs` | observation을 사용할 수 있는 최대 수명. |

NAMO Node의 `participantId`는 해당 `nodeId`입니다. 외부 Satellite는 등록 중 `satelliteId`를
받고 공통 envelope에서는 그 값을 `participantId`로 사용합니다. 역할별 이름은 의미를
분리하고, 공통 식별자는 하나의 제한된 participant registry를 사용할 수 있게 합니다.

IP 주소, hostname, 표시 이름, 현재 Leader 식별자, certificate serial 또는 제품 이름을
영구적인 Site, Space 또는 자동화 entity 식별자로 사용하면 안 됩니다.

### 2.1 로컬 Identity 저장

활성 V2 저장소는 변경되지 않는 로컬 NAMO `nodeId`와 변경 가능한 Site membership을 분리합니다.
Membership record에는 `siteId`, standalone/enrolled mode와 변경 세대인 generation이 들어갑니다.
두 record 모두 식별자 길이, storage version, 인코딩된 길이 및 손상 탐지 checksum을 명시합니다.
IP 주소, hostname, `bootId`, private key 또는 certificate는 저장하지 않습니다. `bootId`는 임시
값이며 부팅할 때마다 바뀝니다.

플랫폼 기능 이전 펌웨어에 record가 없는 상태는 초기화되지 않은 상태이며, 기존 문자열을
legacy identity로 재해석하지 않습니다. 손상되거나 지원하지 않는 record를 새로 생성한 ID로
조용히 교체하면 Site membership과 안정적인 자동화 identity가 깨지므로 그렇게 하면 안 됩니다.
복구와 교체는 명시적인 control-plane 작업으로 처리합니다. Checksum은 저장 손상만 탐지하며,
인증 수단이 아니고 아래의 pairing 또는 attestation 규칙을 대신하지 않습니다.

펌웨어 업데이트 시 기존 결합형 V1 identity를 두 ID 변경 없이 이전합니다. V1 record는 최초 V2
commit이 중단됐을 때를 위한 복구 journal로 남지만 정상 부팅은 분리된 V2 record를 읽습니다. Site
membership은 두 slot을 사용하며 inactive slot이 영속 저장된 뒤 checksum이 있는 commit pointer만
전환합니다. Commit이 없을 때 유효한 slot이 정확히 하나인 경우만 복구하고, 모호하거나 손상된 상태는
안전하게 실패합니다. 의도적인 전체 NVS erase 또는 hardware factory reset은 이 record들을 지우므로
새 identity lifecycle을 시작합니다.

이 record는 펌웨어 내부 저장 형식이며 Satellite wire format이 아닙니다. 공용 wire 표현은 이
문서에 정의된 versioned protocol을 계속 사용합니다.

## 3. 발견과 Pairing

발견은 후보의 위치만 찾으며 신뢰 권한을 부여하지 않습니다.

1. 후보가 임시 식별자, participant type, 제품, protocol 범위 및 pairing 상태를 로컬 discovery service로 알립니다.
2. 사용자가 기존 Site에서 제한된 pairing window를 열거나 standalone NAMO Node에서 Site를 만듭니다.
3. Site가 후보 descriptor와 capability를 읽습니다.
4. 공식 기기라면 Site가 매번 새로운 attestation challenge를 보내고 응답 signature와 certificate chain을 검증합니다.
5. 사용자가 후보를 승인하고 Floorplan, Space 및 Group을 배정합니다.
6. Site가 안정적인 participant identity, membership data와 인증 자료를 발급합니다.
7. 필요한 위치와 보정이 유효해질 때까지 참여자는 `quarantined` 상태를 유지합니다.
8. 활성화를 버전이 있는 Site 설정 변경으로 기록합니다.

제조사 attestation은 검증을 간소화할 수 있지만 명시적인 로컬 pairing window를 우회하거나
스스로 Group 소속을 부여할 수 없습니다. Multicast discovery를 사용할 수 없는 환경에는 주소 수동
입력을 fallback으로 제공합니다.

현재 펌웨어는 HTTP 80번 port의 `_namo._tcp`에 protocol 범위, 안정적인 Node participant ID,
participant type, product, descriptor 경로, `pairing=manual` 및 identity 준비 상태를 광고합니다.
같은 descriptor는 `/api/platform/descriptor`에서 직접 읽을 수 있습니다. 발견은 신뢰를 부여하지
않습니다. 로컬 `/api/platform/participants` endpoint는 제한된 window와 transaction registry
lifecycle을 검증할 수 있지만 peer를 발견하거나 Site credential을 교환하고 attestation을 검증하거나
observation을 전송하지는 않습니다.

현재 endpoint는 선언한 capability, Floorplan/Space/Group 배정, pose version,
calibration version을 포함하는 제한된 readiness profile도 별도 transaction으로
저장합니다. 승인은 quarantine으로 끝나며 profile이 완성되기 전에는 activation을
거부합니다. 호스트에서 검증한 shadow admission core는 active membership, Site/Group,
capability, version, TTL, boot identity, sequence를 검사합니다. 펌웨어에는 인증된 UDP
observation을 이 admission core와 제한된 shadow 진단에 연결하는 개발 전용 수신기가
추가됐습니다. 표준 NAMO 제품 build는 active NAMO Node에 한해 같은 제한된 인증 transport를
사용하며, 선출된 Group Leader에만 production 권한을 부여합니다. 외부 Satellite 가입은
계속 비활성화 상태입니다.

## 4. 참여자 Descriptor

외부 Satellite 예시:

```json
{
  "protocolVersion": 1,
  "participantId": "satellite-bedroom-east",
  "participantType": "satellite",
  "product": "community-mmwave-bridge",
  "softwareVersion": "0.1.0",
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": true,
    "range": false,
    "presence": true,
    "motion": true,
    "zonePresence": false,
    "maxTargets": 3,
    "bridge": true
  },
  "protocolRange": {
    "minimum": 1,
    "maximum": 1
  }
}
```

Verified NAMO Device 예시:

```json
{
  "protocolVersion": 1,
  "participantId": "namo-bedroom-west",
  "participantType": "namo_node",
  "product": "namo-spatial-sensor",
  "softwareVersion": "1.0.0",
  "attestation": {
    "scheme": "namo-device-v1",
    "certificateChain": ["base64-device-cert", "base64-intermediate-cert"]
  },
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": true,
    "presence": true,
    "motion": true,
    "maxTargets": 3
  },
  "protocolRange": {
    "minimum": 1,
    "maximum": 1
  }
}
```

Satellite descriptor에는 `fusionLeaderEligible` 또는 `configReplicaEligible`를 넣지 않습니다.
이 둘은 NAMO Node에만 Site가 부여할 수 있는 권한입니다. Capability는 명시적으로 선언합니다.
수신자는 해석할 수 없는 required capability를 거절하고 모르는 optional field는 무시합니다.

## 5. Observation Envelope

JSON debug 표현 초안:

```json
{
  "protocolVersion": 1,
  "siteId": "site-home",
  "participantId": "satellite-bedroom-east",
  "participantType": "satellite",
  "bootId": "7d3e0d7c",
  "groupId": "group-bedroom",
  "sequence": 1842,
  "observedAtMs": 12345678,
  "ttlMs": 1500,
  "frame": {
    "id": "sensor-local",
    "poseVersion": 4,
    "calibrationVersion": 2
  },
  "tracks": [
    {
      "localTrackId": 7,
      "xMm": 840,
      "yMm": 2110,
      "vxMmS": -40,
      "vyMmS": 15,
      "qualityScore": 82,
      "sampleAgeMs": 28,
      "positionVarianceMm2": [14400, 0, 0, 32400]
    }
  ],
  "evidence": {
    "presence": true,
    "motion": false,
    "rangeMm": null,
    "zonePresence": []
  },
  "health": {
    "state": "healthy",
    "sampleAgeMs": 28
  }
}
```

압축된 data-plane 표현은 짧은 numeric key나 고정 layout을 사용할 수 있지만 이 논리 의미를
그대로 보존해야 합니다.

## 6. Observation 종류와 권한

### 6.1 Point Track

`pointTracks` 참여자는 크기가 제한된 local track을 제공할 수 있습니다. 좌표는 선언된 sensor
frame 기준입니다. Track에는 위치, local track 식별자, observation time 및 quality score가 필요합니다.
속도와 uncertainty는 descriptor에서 사용할 수 없다고 선언한 경우에만 생략할 수 있습니다.

Local track 식별자는 한 participant boot 안에서만 유효합니다. 사람 ID가 아니며 그렇게 노출하면
안 됩니다. 허용된 확정 track은 재실을 시작하거나 유지하고 spatial fusion candidate를 만들 수 있습니다.

`qualityScore`는 0~100 범위의 구현 휴리스틱이며 보정된 확률이 아닙니다. 소비자는 이를 확률로
표시하면 안 됩니다. V1 불확실도는 제곱 mm 단위의 row-major covariance matrix
`[xx, xy, yx, yy]`를 사용합니다. 현재 NAMO tracker는 대각 variance만 제공하므로 wire mapping은
내부 C++ struct를 복사하지 않고 두 cross term을 0으로 넣습니다.

### 6.2 Range

Range observation은 알려진 sensor pose에서의 거리를 제한합니다. 단독으로 2차원 타깃을 결정하거나
재실을 시작할 수 없습니다. Pose와 calibration이 현재 버전일 때만 거리 근거를 fusion에 사용합니다.

### 6.3 Presence와 Motion

PIR과 같은 신뢰된 motion 근거는 명시적인 Site 정책 아래 비공간 재실을 시작하거나 유지할 수
있지만 좌표를 만들거나 타깃 수를 늘릴 수 없습니다.

LD2410C와 같은 presence-only 근거는 허용된 point track 또는 motion 근거로 이미 준비된 재실
세션을 확인하거나 유지하는 데만 사용할 수 있습니다. 세션을 단독으로 시작하거나 다시 준비하거나,
좌표를 만들거나, 타깃 수를 늘리거나, 공간 Zone을 재실로 바꿀 수 없습니다. 만료된 근거와 오래된
target drop reason은 준비 조건으로 인정하지 않습니다.

### 6.4 Zone Presence

Zone 근거는 Site 설정에 이미 존재하는 안정적인 `zoneId`를 사용합니다. Satellite가 observation
메시지로 임의의 Zone을 만들 수 없습니다. Zone 근거 권한은 명시적으로 허용해야 하며 공간 타깃
좌표를 의미하지 않습니다.

## 7. 좌표 규칙

- 거리는 mm를 사용합니다.
- 속도는 mm/s를 사용합니다.
- 시간 간격은 ms를 사용합니다.
- `sensor-local` 원점은 radar 위치이며 +X는 radar 기준 오른쪽, +Y는 radar 정면 방향입니다.
- Floorplan pixel 원점은 이미지 좌상단이며 +X는 오른쪽, +Y는 아래쪽입니다.
- `rotationDeg`는 Floorplan 위쪽에서 sensor-local +Y까지 시계 방향으로 측정하며 단위는 degree입니다.
- Site가 소유한 Floorplan transform은 별도의 `mmPerPxX`, `mmPerPxY`와 radar `scaleFactor`를
  사용합니다. 이를 평균낸 단일 scale로 바꾸면 안 됩니다.
- Pose와 calibration 변경 시 각각의 version을 증가시킵니다.

sensor-local point `(radarX, radarY)`에 기존 dashboard가 적용하는 계약은 다음과 같습니다.

```text
angle = rotationDeg * pi / 180
dx = radarX * scaleFactor / mmPerPxX
dy = -radarY * scaleFactor / mmPerPxY
floorplanX = originX + dx*cos(angle) - dy*sin(angle)
floorplanY = originY + dx*sin(angle) + dy*cos(angle)
```

역변환은 `tools/platform-sim/fixtures-v1.json`의 공용 V1 좌표 fixture로 정의합니다. 향후 world
coordinate 메시지는 명시적인 frame ID와 transform version을 가져야 하며, 수신자가 participant
제품 이름으로 axis를 추론하면 안 됩니다.

## 8. 시간, 순서 및 만료

- `sequence`는 하나의 `bootId` 안에서 단조 증가합니다.
- `bootId`가 바뀌면 이전 상태를 재사용하지 않고 sequence ordering을 초기화합니다.
- 수신자는 중복 또는 더 낮은 sequence를 거절합니다.
- `ttlMs`는 observation이 fusion에 영향을 줄 수 있는 시간을 제한합니다.
- 실시간 observation을 잃어도 이미 오래된 데이터는 재전송하지 않습니다.
- Clock offset을 숨기면 안 됩니다. 참여자가 time quality를 제공하거나 수신자가 arrival 및
  heartbeat timing으로 age를 계산합니다.
- V1의 `observedAtMs`는 sender의 unsigned 32-bit monotonic 값입니다. 수신자는 짧은 elapsed
  interval을 modulo-2^32 subtraction으로 계산합니다. `bootId`가 바뀌면 이전 timing과 ordering
  상태를 모두 버리며 서로 다른 boot의 timestamp를 비교하지 않습니다.

Leader 상태에는 `term`도 포함합니다. 더 낮은 term의 상태는 sequence가 더 커도 거절합니다.

## 9. Group 상태 계약

대시보드와 플랫폼 integration이 소비할 논리 상태 초안:

```json
{
  "protocolVersion": 1,
  "siteId": "site-home",
  "floorplanId": "floorplan-main",
  "spaceId": "space-bedroom",
  "groupId": "group-bedroom",
  "configVersion": 12,
  "term": 7,
  "sequence": 991,
  "leaderNodeId": "namo-bedroom-west",
  "observedAtMs": 12345999,
  "ttlMs": 3000,
  "occupancy": true,
  "motion": false,
  "targetCount": 1,
  "zones": [
    {
      "zoneId": "zone-bed",
      "occupancy": true,
      "targetCount": 1
    }
  ],
  "health": "healthy",
  "participantsOnline": 2,
  "participantsExpected": 2
}
```

플랫폼 adapter는 이 상태를 소비하며 개별 observation으로 fusion을 다시 구현하지 않습니다.

## 10. 권한 및 Source 변경

현재 Leader는 NAMO Node transport endpoint이며 안정적인 자동화 식별자가 아닙니다. 소비자는 더
새로운 유효 term을 확인하거나 이전 lease가 만료된 경우에만 source를 바꿉니다. 다른 endpoint가
더 빨리 응답한다는 이유만으로 반복 전환하면 안 됩니다.

Leader가 아닌 NAMO Node는 알고 있는 Leader 식별자와 term을 포함한 구조화된
`group_not_leader` 응답을 반환할 수 있습니다. Satellite는 Group 상태를 제공할 수 없습니다. 새 Leader는
증분 변경 전에 완전한 Group snapshot을 전송합니다.

## 11. Transport 후보

외부 구현이 ESPHome 세부사항에 묶이지 않도록 protocol 의미와 transport를 분리합니다.

| 통신 | 초기 후보 | 요구사항 |
| --- | --- | --- |
| 발견 | mDNS 및 수동 fallback | 저빈도, trust를 부여하지 않음 |
| Pairing/설정 | HTTP/JSON | 신뢰성 및 인증 필요 |
| Heartbeat/권한 | 크기가 제한된 unicast 메시지 | 저빈도 |
| Observation | UDP unicast 위 인증된 MessagePack | 표준 payload, 제한된 datagram, shadow부터 적용 |
| 디버그 | JSON snapshot | 사람이 읽을 수 있고 선택적 |
| HA/ST 상태 | HTTP snapshot, 이후 push stream | 안정적인 Group 상태만 제공 |

### 11.1 Observation Datagram V1

ADR-0002에서 3B단계 호스트 contract를 확정했습니다. Datagram 하나는 최대 768바이트입니다.

| 구성 | 크기 | 용도 |
| --- | ---: | --- |
| Prefix | 24바이트 | Magic, envelope version, kind, flags, prefix 크기, key version, participant token, payload 길이, reserved field |
| MessagePack | 1..712바이트 | 논리 Observation V1 payload |
| 인증 tag | 32바이트 | Prefix와 payload 전체에 대한 HMAC-SHA256 |

Prefix의 모든 정수는 network byte order를 사용합니다. Pairing된 연결은 불투명한 64비트
participant token을 크기가 제한된 credential 후보를 찾는 용도로만 사용하며, 수신자는 인증된
credential의 participant ID와 payload identity가 같은지 확인합니다. 공개 smoke fixture만
결정적인 테스트 token을 유지합니다. 수신자는 packet 경계와 prefix field를 확인하고,
활성화된 32바이트 participant key를 찾고, HMAC과 MessagePack을 검증한 다음 identity,
readiness, TTL, sequence 및 admission 규칙을 적용합니다. 인증 실패 packet은 논리 admission에
도달하지 않습니다.

Observation은 ACK와 재전송 없는 UDP unicast로 전달합니다. Participant 하나는 초당 datagram을
최대 2개 보내며 수신자는 participant마다 최신 observation 하나만 대기시킵니다. 인증을 통과한
수신 packet에는 scheduler와 네트워크 지터를 흡수하기 위해 고정 1초 window마다 1개의 추가
여유를 둡니다. 이 여유는 송신자의 초당 2개 계약을 늘리지 않습니다. Participant
credential transaction 저장과 안전한 pairing은 구현됐습니다. 현재 `--transport-smoke`
build는 UDP `49190`에서 개발 전용 인증
송수신기와 RAM 전용 공개 smoke identity/key 하나를 켭니다. Follower는 active membership,
readiness, local pose, calibration, credential 검사를 통과하고 상대가 인증된 온라인 Group
Leader일 때만 전송합니다. 송신기는 수신 소켓을 재사용하고 500ms마다 최대 한 번 보내며 ACK나
재전송을 하지 않습니다. 실제 기기 자원, admission 및 Shadow Fusion 검증 전용이며 production
state를 바꿀 권한이 없습니다. 표준 NAMO 제품 build는 공개 smoke credential을 제거하고 저장된
active NAMO Node credential만 허용하며, 선출된 Group Leader가 제한된
융합 결과를 게시할 수 있게 합니다. 일반 build와 외부 Satellite에는 production observation
권한을 부여하지 않습니다.

해당 개발 build를 업로드한 뒤에만 다음 실기기 smoke를 실행합니다.

```powershell
python tools/participant_readiness_smoke.py --device http://192.168.29.104 --transport
```

수신기는 component loop마다 datagram을 최대 2개만 읽고, 초과 크기 검출을 포함한 고정
769바이트 수신 buffer와 remote participant별 만료 가능한 shadow 항목 하나만 사용합니다.
카운터는 `GET /api/platform/participants`로 제공합니다. 별도 task를 만들지 않으며 observation
traffic을 NVS나 radar data partition에 기록하지 않습니다. 송신기는 고정 768바이트 buffer
하나에서 MessagePack과 HMAC을 만듭니다. 분산값이 있는 타깃 3개를 보낼 때는 중복된 선택 필드인
타깃별 `sampleAgeMs`만 생략하고 observation 전체 age를 기준으로 사용합니다.

Credential 저장소는 원격 participant 19개로 제한되며 두 slot과 checksum commit sector 하나를
사용합니다. 새 sector 3개는 기존 registry/readiness sector 6개의 주소를 바꾸지 않고 그 앞에
추가됩니다. Runtime transport는 모든 key를 복사하지 않고 이 owner를 참조합니다. 공개 participant
API는 provisioned/enabled/key-version metadata만 반환하고 participant token과 key byte는
반환하지 않습니다. 개발용 smoke key는 RAM에만 존재합니다.

V1은 MessagePack을 사용합니다. 호스트 benchmark는 다음 명령으로 반복할 수 있습니다.

```text
python -m pip install -r tools/platform-sim/requirements.txt
python tools/platform-sim/benchmark.py --iterations 2000
```

2026-07-21 Windows/Python 3.11에서 5,000회 측정한 중앙값은 다음과 같습니다. 이 결과는 wire 크기와
호스트 구현의 상대 비교일 뿐 ESP32의 heap 또는 parsing 시간을 대신하지 않습니다.

| Observation | JSON | MessagePack |
| --- | ---: | ---: |
| presence-only | 416 bytes / encode 20.8 us / decode 19.0 us | 330 / 16.5 / 13.1 |
| point track 1개 | 553 / 34.3 / 31.1 | 427 / 24.7 / 24.1 |
| point track 3개 | 831 / 55.0 / 49.5 | 627 / 41.7 / 39.8 |

현재 ESPHome build에는 MessagePack 지원과 PSRAM 기반 document allocator를 제공하는
ArduinoJson 7.4.2가 이미 포함되어 있습니다. 이전 fixed-binary spike는 1-track을 130 bytes까지
줄였지만 C++ codec과 중복 validation code 655줄이 필요했습니다. 이를 사설 wire format으로 유지하지
않고 제거했습니다. CBOR도 이미 포함된 MessagePack보다 저장소 차원의 이점 없이 구현을 하나 더
늘리므로 제외했습니다.

현재 500ms observation 주기에서 원격 participant 최대 6대는 Group Leader에 초당 12개
observation packet을 보냅니다. 이 용량을 지원한다고 선언하기 전에 실제 기기에서 peak internal
heap, PSRAM 사용량, parse 시간 및 drop packet을 측정해야 합니다. MessagePack이 이 gate를
통과하지 못하면 먼저 숫자 key로 줄이는 방식을 검토하고, 사설 fixed codec은 마지막
fallback으로만 사용합니다.

Protocol, codec 및 경계 테스트는 다음과 같이 실행합니다.

```text
cd dashboard
npm run test:platform
```


## 12. 보안과 기기 Attestation

### 12.1 공통 Trust 규칙

- Pairing은 명시적인 로컬 사용자 동작과 제한된 시간 window가 필요합니다.
- Membership credential은 하나의 Site와 participant 범위로 제한합니다.
- Observation 인증은 식별자, boot ID, sequence, payload 및 expiry metadata를 포함합니다.
- Replay 방지는 packet reorder를 허용하되 재사용한 sequence는 거절합니다.
- 폐기된 participant는 LAN에서 계속 응답해도 무시합니다.
- 진단에서 secret, 불필요한 certificate 및 private address를 가립니다.
- Bridge는 자신이 선언한 source와 capability보다 더 신뢰하지 않습니다.

### 12.2 공식 기기 Attestation

제조되는 NAMO 기기마다 고유 private key와 device certificate를 넣습니다. Certificate는 NAMO
제조사 root로 이어지는 chain을 가집니다. Key는 통제된 제조 공정에서 생성하거나 주입하며 일반
펌웨어 API로 내보낼 수 없는 방식으로 저장해야 합니다.

Pairing 과정은 다음과 같습니다.

1. Site가 매번 새로운 random nonce와 필요한 transcript context를 보냅니다.
2. 기기가 device private key로 challenge에 서명합니다.
3. Site가 signature, certificate chain, 제품 claim, 유효 기간 및 revoke 상태를 검증합니다.
4. Site가 `official_verified` 상태와 certificate identity를 기록합니다.
5. Site 전용 membership credential은 별도로 발급합니다.

Attestation signature에는 replay와 protocol 간 재사용을 막기 위해 nonce, protocol version,
임시 식별자 및 transcript context를 함께 묶습니다. Private key는 기기 밖으로 내보내지 않습니다.
Manufacturer root는 version을 부여해 NAMO Core Runtime과 함께 배포하며 production 발급 전에
rotation 및 revoke 동작을 정의해야 합니다.

Attestation은 capability, Floorplan, Group 또는 calibration을 부여하지 않으며 로컬 Site 승인을
대체하지 않습니다.

### 12.3 Community NAMO Node

사용자가 빌드한 NAMO Core Runtime은 로컬 관리자의 명시적인 승인 후
`community_unverified`로 등록할 수 있습니다. 일반 NAMO Node 기능을 제공할 수 있지만 관리자가
별도로 opt-in하기 전에는 Leader 자격이 꺼져 있습니다. 공식 Verified NAMO Device라고 표시하거나
광고할 수 없습니다.

이 구분은 AGPL과 호환되는 개방형 구현 경로를 유지하면서 공식 하드웨어 출처도 의미 있게 만듭니다.

### 12.4 외부 Satellite

Satellite는 명시적인 승인 후 Site가 발급한 membership 인증을 사용합니다. Vendor certificate나
protocol conformance가 있어도 관측 전용 역할을 유지합니다. Protocol conformance는 호환성을
증명할 뿐 NAMO 제조, 공식 인증 또는 Leader 권한을 증명하지 않습니다.

Production에서 protocol을 활성화하기 전에 key 발급, 안전한 저장, root rotation, participant
revoke, 교체, backup 경계 및 복구를 확정해야 합니다.

## 13. 버전 관리

- `protocolVersion`은 호환되지 않는 wire 의미 변경에만 증가합니다.
- Schema 추가는 required로 명시하지 않는 한 optional입니다.
- Capability version은 feature 단위 호환성을 설명합니다.
- Site 설정은 별도의 단조 증가 `configVersion`을 가집니다.
- Pose와 calibration은 각각 독립적인 version을 가집니다.
- Attestation scheme과 trust policy version은 observation protocol version과 분리합니다.
- 혼합 버전 동작을 conformance fixture로 검증합니다.

## 14. 자원 제한

구현 계약에는 다음을 정의해야 합니다.

- Site당 활성 Fusion Group 최대 8개;
- 활성 Floorplan마다 정확히 한 대의 Configuration Owner;
- 물리 NAMO Node 한 대당 활성 Floorplan 소유권 최대 1개;
- Configuration Owner마다 활성 Floorplan 복제 session 최대 1개와 크기가 제한된 결정적
  대기 및 재시도;
- Fusion Group당 Group Leader 한 대와 다른 활성 NAMO Node 또는 Satellite 최대 6대;
- 물리 ESP32-S3당 Group Leader 역할 최대 1개;
- 활성 Fusion Group당 NAMO Node 최소 한 대;
- participant당 초당 observation 최대 수;
- observation당 track 및 evidence 최대 수;
- payload 최대 크기;
- 받아들일 수 있는 observation 최대 age;
- 고정 queue 깊이와 overflow 정책;
- fused-track 제한과 association budget.

현재 로컬 연결 registry는 로컬 Node를 포함해 participant 최대 20개인 별도의 평평한
경계를 유지합니다. 이는 pairing 및 저장 경계이며 제품 Group 용량이 아닙니다. Production Group
계약은 센싱
participant 최대 7개, observation당 point track 최대 3개, Zone evidence 최대 6개,
TTL 100~10000ms, participant당 초당 observation 2개, 인증된 datagram 최대 768바이트와
원격 participant별 최신 대기 observation 하나를 강제합니다. Raw JSON/debug codec의 decode 전
4,096바이트 제한은 별도 test 경계이며 production data plane이 아닙니다. Fused-track과
association budget은 제한된 임베디드 경계이자 release benchmark gate입니다.

수신 inbox는 sender의 `observedAtMs`를 receiver 시계와 직접 비교하지 않습니다. Sender monotonic
clock의 offset은 알 수 없으므로 `health.sampleAgeMs`, transport가 제공한 전달 지연 및 receiver의
도착 후 경과 시간으로 TTL을 적용합니다. Participant당 최근 sequence window는 8개, replay 차단용
최근 boot ID는 4개로 제한하며, 역순 packet은 현재 상태를 되돌리지 않습니다.

V1 JSON fixture는 `NaN`과 무한대 숫자를 거절합니다. Track velocity와 position uncertainty는
descriptor capability와 일치해야 합니다. Error `details`는 크기가 제한된 scalar value를 최대 8개만
가질 수 있으며 중첩된 diagnostic payload는 protocol error로 허용하지 않습니다.

제한을 넘으면 크기가 제한된 warning을 남기거나 observation을 거절합니다. 메모리를 무제한으로
늘리거나 로컬 재실 추적을 막으면 안 됩니다.

Floorplan 이미지와 geometry는 Site Leader payload가 아닙니다. 배정된 NAMO Node는 해당
Floorplan의 Configuration Owner에서 직접 받아 로컬 비활성 flash slot에 commit합니다.
Site Leader는 크기가 제한된 원본, generation/digest, Group 배정, 경로 및 health
메타데이터만 받습니다. 서로 다른 Floorplan은 독립적인 원본과 승계 상태를 가집니다.

## 15. 외부 SDK와 Conformance

첫 공개 SDK는 다음을 수행할 수 있는 작은 Python reference Satellite가 적절합니다.

- advertise 및 pairing;
- point, presence, motion 및 range fixture 제공;
- delay, loss, restart 및 stale packet 시뮬레이션;
- capability와 version negotiation 검증;
- NAMO 하드웨어 없이 conformance test server와 실행.

이후 C++ 및 ESPHome 예제가 같은 fixture를 재사용할 수 있습니다. Protocol conformance 통과가 공간
정확도, 공식 NAMO 제조 또는 Leader 자격을 보증하지는 않으며 이들은 별도로 유지합니다.

현재 `tools/platform-sim`은 deterministic sender, packet loss/reorder/restart, 기존의
20-participant 저장 한도, latest-wins inbox, 인증된 MessagePack datagram, byte 단위 golden
vector와 codec benchmark를 제공합니다. Discovery, pairing, socket, 실제 기기 데이터 발행 또는
Home Assistant custom integration 역할은 하지 않습니다.

## 16. 관련 문서

- [공간 플랫폼 아키텍처](architecture.ko.md)
- [구현 로드맵](implementation-roadmap.ko.md)
- [현재 API 계약](../api-contract.ko.md)
- [ADR-0002: 인증된 Observation Transport](../adr/0002-authenticated-observation-transport.ko.md)
