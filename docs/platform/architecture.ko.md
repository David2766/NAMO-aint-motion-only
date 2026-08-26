# NAMO 공간 플랫폼 아키텍처

상태: **점진적으로 검증 중인 초안입니다.**

- **구현됨:** 안정적인 identity, 크기가 제한된 registry, 안전한 Community NAMO Node 연결,
  인증 heartbeat와 observation transport, 공유 평면도 복제, 보정, 결정적인 역할 승계,
  고정 메모리 7개 입력 Group fusion 및 Home Assistant custom integration.
- **실기기 검증됨:** 단일 Group의 5대 Site에서 신규 기기 연결, 평면도 복제, 보정, 활성화,
  단일 역할 기기 장애, 두 역할 기기 동시 장애 및 정상 복귀 흐름을 완료했습니다. 생존한
  strict majority에서는 자동화를 계속 사용할 수 있었습니다. 또한 7대까지 순차 확장하여
  페어링, 복제, 보정, 활성화, 최종 7/7 canonical 수렴, 융합·자동화 및 자원 상태를 확인했습니다.
- **남은 릴리스 게이트:** 다른 Group 규모의 장애·복귀 matrix, 다중 Group 및 순차 업데이트.
- **미완료:** 제조사 attestation, 외부 Satellite SDK 및 SmartThings integration. 같은 revision
  복구 격리, 긴급 삼중 역할 fallback과 해제, pairing poll 복구, 하나로 통일된 pairing 수명 및
  bounded handover retry와 기존 Leader를 유지하는 5→6→7대 Group roster 확장은 host test로
  검증했습니다.

영문 문서: [architecture.md](architecture.md)

이 문서는 NAMO 기기가 설정을 공유하고, runtime 역할을 선출하고, observation을 교환하며,
장애 중에도 자동화 상태를 유지하는 방식을 이해해야 하는 개발자를 위한 문서입니다. 다중 센서
기능의 아키텍처 경계를 정의하며, 정확한 production HTTP 동작은 현재 펌웨어와
[API 계약](../api-contract.ko.md)이 정의합니다.

## 읽는 순서

- 타사 센서 또는 bridge를 연동하려면 3장, 5장, 6장 및 12장을 먼저 읽은 뒤
  [Satellite 규약](satellite-protocol.ko.md)을 참고하세요.
- pairing, 복제, 역할 선출 또는 장애 복구를 다루려면 4장, 6장, 7장 및 11장을 읽으세요.
- 좌표 변환, 보정 또는 타깃 융합을 다루려면 8장, 9장 및 10장을 읽으세요.
- endpoint 필드, 상태 코드 및 오류는 [API 계약](../api-contract.ko.md)을 기준으로 확인하세요.

## 1. 목표

- 센서가 한 대뿐인 환경에서 현재 standalone 동작을 유지합니다.
- 각 Site를 최대 8개 Fusion Group의 저빈도 directory로 구성합니다.
- 각 Fusion Group은 Group Leader 한 대와 다른 센싱 participant 최대 6대, 총 7개 입력으로 제한합니다.
- 비슷한 방향 또는 서로 다른 방향을 보는 센서 관측을 융합합니다.
- 필수 클라우드나 항상 켜져 있는 외부 coordinator 없이 완전 로컬로 동작합니다.
- 타사 및 커뮤니티 센서가 문서화된 capability 기반 Satellite 규약으로 참여할 수 있게 합니다.
- 제어 권한은 NAMO Node에 두되 공식 인증 기기와 명시적으로 승인된 커뮤니티 NAMO 빌드를 모두 허용합니다.
- Home Assistant와 SmartThings 자동화를 물리 센서나 현재 Group Leader가 아니라
  안정적인 논리 공간에 연결합니다.
- ESP32급 기기에서 CPU, RAM, 저장 공간, 패킷 빈도 및 queue 크기를 제한합니다.

## 2. 첫 릴리즈의 비목표

- Thread, Zigbee 또는 범용 메시 패킷 전달.
- 사람 신원 식별 또는 층간 개인 추적.
- 기기 간 raw radar frame 공유.
- 큰 설치 오차나 평면도 오차의 자동 보정.
- Satellite 또는 bridge의 Group Leader 및 Site Leader 참여.
- 커뮤니티 빌드의 로컬 동작에 제조사 attestation을 필수로 요구하는 것.
- Group fusion과 Site coordination 부하를 실측하고 검증하기 전의 계층형 용량 지원 선언.

## 3. 도메인 모델

| 개념 | 의미 | 안정적인 식별자 |
| --- | --- | --- |
| Site | 하나의 관리 단위인 설치 환경. | `siteId` |
| Floorplan | 하나의 지도와 물리 좌표계. | `floorplanId` |
| Space | 사용자와 자동화가 사용하는 논리 방 또는 공간. | `spaceId` |
| Fusion Group | 관측을 서로 융합할 수 있는 참여자 묶음. | `groupId` |
| NAMO Node | NAMO Core Runtime과 Node 계약을 실행하는 기기. | `nodeId` |
| Verified NAMO Device | 유효한 제조사 attestation을 가진 공식 NAMO Node. | `nodeId + deviceCertificate` |
| Community NAMO Node | 제조사 attestation 없이 사용자가 만든 NAMO Node. | `nodeId` |
| Satellite | 관측만 제공하는 승인된 외부 센서 또는 bridge. | `satelliteId` |
| Participant | Site와 Group 제한에 포함되는 NAMO Node 또는 Satellite. | `participantId` |
| Configuration Owner | 정확히 하나의 Floorplan에서 권위 있는 geometry와 payload를 소유하는 NAMO Node. | `floorplanId + generation` |
| Group Leader | 한 Group의 고빈도 observation을 받아 융합하도록 일시적으로 승인된 NAMO Node. | `groupId + term` |
| Site Leader | 저빈도 Group directory와 Site health를 조정하도록 일시적으로 승인된 NAMO Node. | `siteId + term` |

Satellite는 NAMO Node가 아닙니다. 제품 출처가 observation capability를 결정하지는 않지만,
권위 있는 Floorplan 설정 소유, Leader 선출, 신뢰 승인 및 최종 자동화 상태 발행은
NAMO Node만 수행할 수 있습니다. 모든 Fusion Group에는 최소 한 대의 NAMO Node가 있어야 합니다.

제조사 attestation과 Node capability는 서로 분리합니다. Verified NAMO Device는 명시적인
로컬 pairing 과정에서 자동으로 정품 여부를 검증할 수 있습니다. Community NAMO Node도 관리자가
직접 승인하고 Leader 자격을 명시적으로 허용하면 같은 로컬 Node 역할을 받을 수 있습니다.
Attestation은 공식 제조 및 펌웨어 출처를 증명하지만, AGPL에 맞게 만든 로컬 Node를 영구적으로
막는 자격증으로 사용하지 않습니다.

## 4. 토폴로지

```text
Site
|-- Floorplan A
|   |-- Space: 거실
|   |   `-- Fusion Group: living-room
|   |       |-- Verified NAMO Device A (Leader 기본 자격)
|   |       |-- Community NAMO Node B (수동 신뢰, 선택적 Leader 자격)
|   |       `-- 타사 Satellite C (관측 전용)
|   `-- Space: 침실
|       `-- Fusion Group: bedroom
`-- Floorplan B
    `-- Space: 위층 복도
        `-- Fusion Group: upstairs-hall
```

Site 전체 통신은 발견, heartbeat, 토폴로지 및 설정 버전으로 제한합니다. 고빈도 관측은
해당 Fusion Group 안에서만 전송합니다. Site Leader는 모든 observation이 아니라 크기가 제한된
Group 요약만 받습니다. Group별 현재 Group Leader 경로, 권한 term, health, 구성원 수 및
설정 revision을 관리하며 Group observation stream이나 융합 target snapshot을 중계하지 않습니다.

활성 Floorplan마다 영속 Configuration Owner를 정확히 한 대 둡니다. 소유권과 generation은
Floorplan별로 독립적으로 증가합니다. Site Leader는 coordination에 필요한 제한된
`floorplanId`, 원본 경로, generation/digest, Group 배정 및 health 메타데이터만 보관합니다.
Site Leader라는 이유만으로 Floorplan 이미지나 geometry를 저장하지 않습니다. 물리 NAMO Node
한 대는 활성 Floorplan 하나의 Configuration Owner만 맡습니다.

현재 heartbeat 경로는 안정적인 `participantId`와 pairwise credential을 사용합니다. mDNS가
경로 후보를 제공하고, 인증된 UDP 송신 주소가 peer의 현재 IP를 RAM에서 갱신합니다. 네트워크
주소는 영속 Site identity가 아니며 DHCP 갱신이나 재부팅 후 바뀔 수 있습니다.

## 5. 역할, Capability 및 근거 권한

프로토콜은 제품 이름이 아니라 observation capability를 인식합니다.

| Capability | 책임 |
| --- | --- |
| `point_tracks` | 로컬 타깃 위치와 속도를 제공합니다. |
| `range` | 방사형 거리 제약을 제공합니다. |
| `presence` | 비공간 재실 근거를 제공합니다. |
| `motion` | 비공간 움직임 근거를 제공합니다. |
| `zone_presence` | 설정된 Zone 범위의 재실 근거를 제공합니다. |
| `bridge` | 다른 프로토콜을 Satellite observation으로 변환합니다. |

Leadership와 설정 복제는 Satellite capability가 아니라 NAMO Node 권한입니다. Satellite
descriptor는 Leader 자격을 요청하거나 광고할 수 없습니다.

각 근거의 권한은 다음처럼 명시합니다.

| 근거 | 재실 시작 또는 유지 | 공간 타깃 생성 |
| --- | --- | --- |
| 확정된 point track | 가능 | 가능 |
| PIR과 같은 신뢰된 motion 근거 | 정책에 따라 가능 | 불가 |
| LD2410C와 같은 presence-only 근거 | 이미 준비된 세션 유지에만 사용 | 불가 |
| range-only 근거 | 보조 제약으로만 사용 | 불가 |

`presence` 근거만 제공하는 Satellite는 재실 세션을 단독으로 시작하거나, 공간 타깃을 만들거나,
타깃 수를 늘리거나, 공간 Zone을 재실로 바꿀 수 없습니다. 먼저 허용된 point track 또는 motion
근거로 준비된 세션을 확인하거나 유지하는 데만 참여할 수 있습니다. 이는 현재 NAMO의 LD2410C
설계 원칙을 유지하면서 비공간 센서가 유효한 세션을 보강하게 합니다. 오래된 drop reason이나
만료된 근거로 세션을 다시 준비하면 안 됩니다.

## 6. Control Plane과 Data Plane

Control plane은 저빈도이며 신뢰성 있는 작업을 담당합니다.

- 발견 및 pairing;
- Site, Floorplan, Space, Group, NAMO Node 및 Satellite 설정;
- capability 협상과 trust 분류;
- 버전 및 hash 비교;
- 보정 명령과 snapshot;
- health 및 Leader 발견.

공유 Floorplan payload는 크기가 제한된 별도 control-plane 전송으로 취급합니다. NAMO Node는
자신에게 배정된 Floorplan만 해당 Floorplan의 영속 Configuration Owner에서 인증된 chunk로
직접 받아 로컬 비활성 flash slot에 commit합니다. 각 Configuration Owner는 활성 복제 session을
한 번에 하나만 처리합니다. 나머지 대상 Node는 결정적인 제한 slot에서 기다리고 제한된
backoff로 다시 요청합니다. Site Leader는 해당 byte를 대기시키거나 저장하거나 중계하지 않습니다.
Site Leader에는 작은 원본, generation, hash, 배정 및 health 메타데이터만 전달합니다.

canonical Site directory는 같은 인증·고정 크기 조각 전송의 별도 저빈도 경로를 따릅니다.
Site Leader가 아닌 모든 active NAMO Node는 평상시에 현재 Site Leader에서 직접 받아 기존
로컬 A/B directory 저장소에 commit합니다. 초기 등록에는 별도 bootstrap 단계가 있습니다.
quarantined joiner는 보통 역할 권위를 얻기 전에 이미 인증된 Configuration Owner 관계를 통해
확정 canonical 사본을 읽습니다. 같은 Site의 오래된 canonical 사본은 활성화를 막지 않으며,
선출 및 복제 fence가 수렴 전까지 해당 Node를 권위 없는 상태로 유지합니다. active 구성원이
2대를 넘는 Site에서 Owner가 Site Leader가
아니라면, live quorum이 있고 선출된 Site Leader가 정확히 같은 Directory revision과
digest를 광고할 때만 이 읽기 전용 사본을 제공할 수 있습니다. joiner는 이 단계에서 자신의
compatibility mirror를 canonical authority로 승격하지 않습니다. heartbeat v7은 확정 Directory revision과 의미 digest를 광고하므로 선출은
역할과 Node ID를 비교하기 전에 더 최신인 영속 사본을 우선합니다. 각 기기의 flash 저장
generation은 로컬 bookkeeping으로 남깁니다. canonical에서 제거된 Group과 등록이 사라진
Floorplan은 정리합니다. 이 수렴은 자동으로 진행되며 사용자가 topology를 설정하는 별도
workflow를 요구하지 않습니다.

Data plane은 수명이 짧은 observation을 전달합니다.

- raw radar frame이 아닌 크기가 제한된 local track;
- 단조 증가 sequence와 boot identifier;
- timestamp, TTL, 좌표계 버전 및 보정 버전;
- source confidence와 제공 가능한 uncertainty 정보.

논리 데이터 계약은 전송 방식과 분리합니다. Control plane은 HTTP/JSON을 우선합니다. V1 Observation은
표준 MessagePack을 사용합니다. 펌웨어에 호환되는 ArduinoJson 구현이 이미 있고 제한된 호스트
benchmark에서 JSON보다 작고 빨랐으며 사설 binary codec을 유지할 필요가 없기 때문입니다. Observation
transport를 활성화하기 전에 대상 하드웨어 부하 검증을 반드시 통과해야 합니다. 타사 개발을 위한 JSON
debug 표현은 계속 제공합니다.

## 7. Leader와 장애 처리

각 Fusion Group은 자격이 있는 NAMO Node 중 하나를 Group Leader로 선출합니다. Site는 이와
독립적으로 자격이 있는 NAMO Node 중 저빈도 Site Leader 하나를 선출합니다. Satellite는
Leader나 설정 replica가 될 수 없습니다. Site Leader가 모든 타깃 관측을 처리하면 안 됩니다.

정상 상태에서는 Site 제어와 고빈도 Group 융합이 같은 물리 기기에 몰리지 않도록 결정적인
역할 충돌 회피 순서를 사용합니다. Site Leader 후보는 Configuration Owner도 현재 canonical
Group Leader도 아닌 기기, Configuration Owner만 맡은 기기, 현재 canonical Group Leader만
맡은 기기, 두 역할을 모두 맡은 기기 순으로 고릅니다. 따라서 무관한 Site Leader 교체가 살아
있는 Group Leader를 가져가 Group term까지 바꾸는 역할 연쇄를 피합니다. Group Leader 후보는
해당 Group의 active·ready NAMO Node로 제한하며, Site Leader와
그 Group의 Floorplan Configuration Owner가 아닌 후보, 해당 Configuration Owner, 마지막
장애 fallback인 Site Leader 순으로 선택합니다. 같은 우선순위에서는 Node ID로 결정합니다.
하나의 ESP32-S3는 Fusion Group 하나의 Group Leader만 맡을 수 있습니다. Configuration Owner와
Site Leader, Configuration Owner와 Group Leader의 겸임은 허용합니다. Site Leader와 Group
Leader의 겸임은 피하고 일반 후보가 없을 때만 허용합니다. 다중 Node Site에서는
해당 Group에 다른 적격 후보가 전혀 없을 때에만 Configuration Owner, Site Leader 및
Group Leader 세 역할을 한 기기에 긴급 배정합니다. 이 삼중 역할 incumbent는 보호하지 않으며
적격 후보가 생기면 역할을 넘깁니다. standalone 호환 동작은
로컬에서 이 책임을 접을 수 있지만 다중 Node 역할 배정의 선례로 사용하지 않습니다. 여러
Group이 보이는 상황에서 로컬 Group 배정이 canonical 데이터로 확인되지 않으면 로컬 기기는
임의의 Group을 맡지 않고 fail-closed로 남습니다.

외부에 공개하는 역할 진단은 변경 불가능한 generation snapshot입니다. HTTP reader는 Site
coordinator와 모든 로컬 Group coordinator를 한 번 완료된 control loop publication에서
읽어야 하며 서로 다른 선출 cycle의 변경 중 상태를 섞어 보면 안 됩니다.

정상 Group Leader가 교체되는 동안 마지막 완전한 자동화 snapshot은 최대 30초 동안
비권위·읽기 전용 연속성 상태로 표시할 수 있습니다. 이 고정 시간은 실패가 반복되어도 늘어나지
않습니다. 움직임은 false로 고정하고 새 타깃이나 Zone 근거를 만들지 않습니다. 설정, 평면도,
pose, calibration, membership 불일치 또는 권위 term 역행이 있으면 즉시 연속성을 폐기합니다.

Verified NAMO Device는 Site 승인 후 기본적으로 Leader 자격을 가집니다. Community NAMO Node는
관리자가 명시적으로 Leader 자격을 허용해야 합니다. 자격 있는 NAMO Node가 한 대도 없는 Group은
활성화할 수 없습니다.

권한은 `term`과 제한된 `lease`로 표현합니다. 소비자는 권한, 설정 버전 및 sequence가
현재 값일 때만 상태를 받아들입니다. 이전 Leader가 늦게 보낸 패킷은 무시합니다.

Leader 전환 중에는 다음 규칙을 지킵니다.

- 마지막 유효 재실 상태를 정의된 TTL 동안 유지합니다.
- 권한이 불확실한 동안 health를 `degraded`로 바꿉니다.
- 통신 실패를 퇴실 이벤트로 변환하지 않습니다.
- 상태 만료는 `unoccupied`가 아니라 `unavailable`로 처리합니다.
- 새 Leader는 증분 이벤트보다 먼저 전체 snapshot을 제공합니다.

첫 구현은 범용 합의 시스템 대신 결정적인 lease 기반 선출을 사용할 수 있습니다. 다만 설정
충돌 규칙은 명확하고 테스트 가능해야 합니다.

제한된 제어 평면 계약은 다음과 같습니다.

- 역할 선출은 각 기기가 우연히 online으로 보는 집합이 아니라 canonical 설정 구성원의
  strict majority를 사용합니다.
- 적격 투표자는 Site 또는 Group Leader가 권위를 갖기 전에 같은 제한된 election-view
  digest를 승인해야 합니다.
- 현재 Site와 Group election-view identity에는 설정된 전체 canonical roster, roster revision
  및 선택된 winner를 포함합니다. 생존 상태, Directory 배정 및 순간적인 credential 준비 상태는
  이 안정적인 identity에서 제외하지만 각각 quorum, 의미 일관성, 후보 자격 및 권위 admission의
  독립된 필수 조건으로 사용합니다.
- term, lease, canonical roster·directory revision 및 semantic digest로 stale 또는 서로
  다른 후보를 차단합니다.
- 같은 authority epoch에서 현재 wire protocol을 사용하는 active Node가 generation과 roster
  revision에서 정확히 한 단계만 차이 나고 credential, Floorplan 및 radar pose 계약이 그대로라면
  이미 동작 중인 heartbeat와 fusion 데이터 평면은 유지할 수 있습니다. 공유 쓰기, 원본 광고 및
  Group 요약은 정확한 canonical revision이 수렴할 때까지 차단합니다. 이 제한된 전환 규칙은
  epoch 변경, 두 단계 이상 차이, 구형 wire version 또는 공간 계약 불일치를 허용하지 않습니다.
- minority는 더 작은 canonical membership을 임의로 만들지 않고 분산 역할 권위를 비우며
  로컬 감지만 유지합니다.
- 안정적인 적격 majority는 결국 수렴해야 하며, 후보 장애나 일시적인 transport·저장 실패는
  권위를 영구 고착시키지 않고 bounded retry로 다시 평가해야 합니다.
- 충돌 차단은 숫자 revision 전체가 아니라 거부한 semantic 후보를 식별해야 합니다. 같은
  revision의 다른 후보에 더 강한 quorum 근거가 생기면 replay 진동을 허용하지 않는 범위에서
  다시 평가할 수 있어야 합니다.

Configuration Owner 승계는 Site 및 Group runtime leadership과 독립적입니다. 기존 Group
Leader와 canonical 배정이 Owner 장애, 승계 또는 과거 Owner 재합류 중에도 유지되면,
credential bundle이 일시적으로 수렴하는 과정은 Group Leader나 term을 바꾸면 안 됩니다.
짧은 election-fence 공백에서는 외부 권위를 비우고 fail-closed하지만, 재부팅하지 않은 정확히
같은 Leader가 일치하는 view를 회복하면 보류한 term을 그대로 재개합니다. 다른 Leader가
선출되면 반드시 더 새로운 term을 사용합니다.

현재 설정 구성원 기준 계약에서는 1대 Site는 standalone으로 동작하고, 2대 Site에서 한 대를
잃으면 분산 권한을 승계할 수 없습니다. 더 큰 Site는 `floor(N/2)+1`개의 일치하는 active 구성원이
필요합니다. 향후 생존 기기 재구성 정책은 별도의 quorum 계약 변경이며 암묵적인 복구 편법으로
추가하면 안 됩니다.

## 8. 공유 좌표계와 보정

각 Floorplan의 Configuration Owner가 권위 있는 geometry를 소유합니다. NAMO Node는 자신에게
배정된 Floorplan과 Space만 저장합니다. 각 spatial participant는 버전이 있는 다음 변환을 가집니다.

```text
센서 로컬 좌표
-> 회전
-> 이동
-> 제한된 보정값
-> Floorplan world 좌표
```

초기 배치는 사용자가 제어합니다. 보정 snapshot은 겹치는 영역에 있는 한 사람을 여러 센서가
동시에 본 관측을 비교할 수 있습니다. 소프트웨어는 작은 이동이나 회전 보정을 제안할 수 있지만,
물리 설치 오류를 숨기는 scale, shear 또는 큰 보정을 조용히 적용하면 안 됩니다.

보정되지 않았거나 버전이 맞지 않는 observation은 진단용으로 표시할 수 있지만 공간 융합에
참여시키면 안 됩니다.

보정이 승인되면 Site configuration은 NAMO Node마다 하나의 canonical spatial profile을
저장합니다. 이 profile에는 승인된 pose revision, calibration revision 및 유효 transform이
들어갑니다. 이 record는 인증된 Site 복제 경로를 따르므로 Group Leader 또는 설정 원본이
바뀌어도 새 기기가 로컬 배치에서 좌표계를 다시 만들지 않고 같은 좌표계를 이어받습니다.
profile이 없거나 version이 오래되면 fail-closed로 처리합니다.

## 9. Fusion 경계

현재 로컬 tracker는 하나의 LD2450 stream을 안정적인 local track으로 만드는 책임을 유지합니다.
Fusion Engine은 adapter를 통해 이 track을 받고 raw sensor frame을 해석하지 않습니다.

Fusion은 다음을 고려합니다.

- world 좌표 거리와 uncertainty;
- 속도 및 이동 방향 호환성;
- observation age와 TTL;
- 기존 fused track과의 연속성;
- source health와 confidence;
- 비슷한 각도로 설치된 센서의 상관 오차.

다른 각도의 observation은 geometry를 개선할 수 있습니다. 비슷한 각도의 observation은 confidence를
단순히 두 배로 만들면 안 됩니다. 애매한 observation은 억지로 합치지 않고 분리된 상태로 둡니다.

PIR 또는 LD2410C와 같은 비공간 근거는 위 근거 권한표를 따릅니다. 이미 준비된 재실 세션을
보강할 수 있지만 타깃 좌표를 만들거나 타깃 수를 늘릴 수 없습니다.

## 10. 플랫폼 출력

Group Leader는 크기가 제한된 논리 Space 상태를 제공합니다.

```text
occupancy
motion
fused target count
zone occupancy와 count
입실 및 퇴실 transition
fusion health
기여 참여자 수와 예상 참여자 수
권한 term, sequence 및 observation time
```

내장 대시보드는 로컬에 cache된 상태를 사용합니다. Home Assistant와 SmartThings는 Site Leader를
저빈도 Group directory로 사용한 뒤 각 논리 Space 상태를 현재 Group Leader에서 직접 읽습니다.
Site Leader는 고빈도 target 상태를 모으거나 중계하지 않습니다. Platform adapter는 confidence
병합, target association, TTL 판단 또는 충돌 해결을 수행하지 않습니다.

Home Assistant 식별자는 `siteId + spaceId + entity type`을 사용합니다. SmartThings 논리 child는
`spaceId`와 `spaceId + zoneId`를 사용합니다. 따라서 Leader와 IP 변경이 자동화 대상을 다시 만들지 않습니다.

## 11. 계층형 용량 목표

Site control plane과 Group data plane의 용량을 서로 독립적으로 제한합니다.

- Site 하나에는 활성 Fusion Group을 최대 8개 둡니다.
- 모든 활성 Fusion Group은 정확히 하나의 Floorplan에 속합니다. 따라서 Group이 하나씩 있는
  Floorplan 8개라면 Site의 활성 Floorplan도 8개입니다.
- 모든 활성 Floorplan에는 독립적인 Configuration Owner 한 대가 있으며, Site Leader는
  크기가 제한된 directory record만 저장합니다. 물리 NAMO Node 한 대는 활성 Floorplan
  하나만 소유합니다.
- Fusion Group 하나에는 Group Leader 한 대와 다른 NAMO Node 또는 Satellite 최대 6대,
  총 7개의 센싱 participant를 둡니다.
- Satellite는 Group Leader가 될 수 없으므로 모든 Fusion Group에는 NAMO Node가 최소 한 대 있어야 합니다.
- 하나의 ESP32-S3는 Fusion Group 하나의 Group Leader만 맡습니다.
- Site Leader는 크기가 제한된 Group directory 및 health record 8개만 저장합니다.
  각 Group의 observation을 저장하거나 Group fusion을 수행하지 않습니다.
- 참여자당 local track 최대 수를 선언하고 제한합니다.
- fused track 최대 수는 구현 계약에서 별도로 제한합니다.
- 모든 queue와 registry는 크기가 제한된 저장소를 사용합니다.
- 오래된 observation은 재전송하지 않고 만료합니다.
- 비싼 matching 전에 공간 gate로 association 후보를 줄입니다.
- Leader는 radar frame이나 대시보드 데이터가 아닌 local track summary를 받습니다.

현재 펌웨어는 로컬 관계 관리를 위해 평평한 20개 participant registry를 유지합니다. Group
membership과 fusion은 센싱 participant 7대로 별도 제한하며, Site directory는 Group 8개로
제한합니다. 따라서 관계 registry의 크기는 Group 용량 계약이 아닙니다. 검증에서는 7개 입력
Group, 8개 Group Site directory 및 한 기기가 일시적으로 Site와 Group Leader를 겸하는 장애
상태를 각각 확인해야 합니다. 패킷 빈도, 누락된 observation, peak heap, largest free block,
fusion cycle당 CPU 시간, Leader 전환 시간 및 잘못된 상태 transition을 측정합니다.

## 12. 보안, 신뢰 및 기기 Attestation

Trust 등급은 observation capability와 분리합니다.

| Trust 등급 | 등록 | Leader 자격 | 사용자 표시 |
| --- | --- | --- | --- |
| Verified NAMO Device | 로컬 pairing 중 제조사 attestation 검증 | Site 승인 후 기본 허용 | `official_verified` |
| Community NAMO Node | 관리자 명시적 승인 | 명시적 opt-in만 허용 | `community_unverified` |
| Satellite | 명시적 승인과 capability 검증 | 불가 | `satellite` |
| 알 수 없는 기기 | 격리 | 불가 | `unknown` |

Production NAMO 하드웨어에는 제조 단계에서 기기별 고유 private key와 device certificate를 넣습니다.
Certificate는 NAMO 제조사 root로 서명합니다. Pairing 시 매번 새 challenge를 보내고 signature를
검증하여 key 보유 여부를 확인합니다. Private key는 절대로 기기 밖으로 내보내지 않습니다. Site는
private key가 아니라 검증한 certificate identity와 trust 결과만 저장합니다.

Attestation 설계에는 안전한 key 저장, certificate chain과 version, root rotation, 기기 revoke,
교체와 복구 및 진단 redaction을 포함해야 합니다. Certificate는 공식 기기 출처를 증명하지만 그
자체로 Site 가입, Group 배치, 보정 또는 observation capability를 부여하지 않습니다. 명시적인
로컬 pairing은 계속 필요합니다.

Community NAMO Node는 AGPL 소스 모델 안에서 계속 허용합니다. 공식 인증을 주장할 수 없고 자동으로
신뢰하지 않으며, 로컬 관리자가 권한을 직접 허용한 경우에만 Leader 후보가 됩니다. 이 구조는 공식
하드웨어를 구분하면서도 제조사 attestation을 폐쇄형 protocol gate로 만들지 않습니다.

모든 참여자에는 다음 규칙을 적용합니다.

- 명시적으로 제한된 pairing window에서만 가입을 허용합니다.
- Site가 식별자, Group 소속 및 trust 상태를 부여합니다.
- 메시지에 `siteId`, 참여자 식별자, `bootId`, `sequence` 및 TTL을 넣습니다.
- Observation이 자동화 상태에 영향을 주기 전에 인증과 replay 방지를 적용합니다.
- 새로 가입했거나 보정되지 않은 참여자는 `quarantined`로 시작합니다.
- 활성화 전에 capability와 protocol version을 검증합니다.
- 모르는 optional field는 무시하고, 지원하지 않는 required feature는 pairing 또는 observation을 거절합니다.

## 13. 코드 책임

| 현재 경로 | 책임 |
| --- | --- |
| `components/radar_api_server/presence_tracker.*` | 로컬 LD2450 tracking. |
| `components/radar_api_server` 아래의 static-presence 모듈 | PIR 및 LD2410C 근거. |
| `components/radar_api_server` 아래의 `site_*`, `group_*`, `participant_*`, `observation_*`, `spatial_*` 모듈 | Registry, trust, 인증 transport, coordination, fusion, calibration 및 복제된 공간 상태. |
| `components/radar_api_server` 아래의 handler와 dashboard 모듈 | 공개 HTTP API 및 내장 대시보드 transport. |
| `custom_components/namo` | Home Assistant platform adapter. |
| `smartthings-edge/radar-zone` | SmartThings platform adapter. |

공간 플랫폼은 `components/radar_api_server` 아래의 역할별 C++ 모듈로 분리되어 있으며,
`presence_tracker.cpp`에 분기를 계속 추가하는 방식으로 구현하지 않습니다. 향후 디렉터리를
분리하더라도 이 책임 경계는 유지해야 합니다. 타사 개발자는 Satellite 계약만 구현하며 NAMO
로컬 tracker 내부에 의존하지 않습니다.

## 14. 호환성

- Fusion을 끄면 현재 로컬 tracking과 재실 동작을 유지합니다.
- NAMO Node 한 대인 Site도 같은 Site, Space, Group 및 권한 schema를 사용합니다.
- 다른 참여자를 추가해도 논리 Space 식별자는 바뀌지 않습니다.
- 기존 ESPHome 물리 엔티티를 계속 제공합니다.
- 기존 자동화 의미를 조용히 바꾸지 않고 새 HA 및 SmartThings 논리 엔티티를 추가합니다.
- 계획된 cluster endpoint는 구현 및 호환성 테스트 전까지 현재 API 계약에 포함하지 않습니다.

## 15. 관련 문서

- [Satellite 규약](satellite-protocol.ko.md)
- [구현 로드맵](implementation-roadmap.ko.md)
- [ADR-0001: 분산 공간 플랫폼](../adr/0001-distributed-spatial-platform.ko.md)
- [ADR-0002: 인증된 Observation Transport](../adr/0002-authenticated-observation-transport.ko.md)
- [ADR-0003: 참가자 자격 증명 저장 경계](../adr/0003-participant-credential-storage.ko.md)
- [ADR-0004: 안전한 Community 기기 연결](../adr/0004-secure-community-pairing.ko.md)
- [현재 API 계약](../api-contract.ko.md)
- [대체된 다중 센서 메모](../multi-sensor-contract.ko.md)
