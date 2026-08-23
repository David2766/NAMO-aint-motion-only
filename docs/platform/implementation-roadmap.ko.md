# NAMO 공간 플랫폼 구현 로드맵

상태: **점진 검증 중인 초안. Production pairing, 공용 Floorplan 복제, 보정, 제한된
leadership, observation transport, 최대 7개 입력 fusion 및 Home Assistant Integration을
구현했습니다. 단일 Group의 실제 5대 Site에서 신규 기기 연결부터 평면도 복제, 보정, 활성화까지
관통하는 workflow와 역할 한 대 및 역할 기기 두 대의 전원 차단·복귀, 생존 과반수의 자동화
연속성을 검증했습니다. 4·6·7대, 다중 Group 및 순차 업데이트 릴리즈 matrix는 남아 있습니다. 공식
attestation, Satellite SDK 및 SmartThings는 미완성입니다.**

영문 문서: [implementation-roadmap.md](implementation-roadmap.md)

마지막 갱신: 2026-08-21

이 로드맵은 아키텍처를 독립적으로 검증 가능한 작업으로 나눕니다. 파일이나 interface가 생겼다는
이유만으로 단계를 완료 처리하지 않습니다. 현재 standalone 동작을 바꾸지 않고 해당 단계의 완료
조건과 테스트를 통과해야 완료입니다.

## 1. 상태 값

| 상태 | 의미 |
| --- | --- |
| `planned` | 범위는 문서화했지만 구현을 시작하지 않음. |
| `in_progress` | 작업 또는 검토 중이며 릴리즈 준비가 되지 않음. |
| `implemented` | 코드가 있고 집중 테스트를 통과함. |
| `verified` | 하드웨어, replay 및 호환성 gate를 통과함. |
| `blocked` | 문서화된 외부 또는 아키텍처 차단 요인 때문에 진행할 수 없음. |

## 2. 불변 규칙

모든 단계에서 다음 규칙을 지킵니다.

- Fusion을 끄면 현재 standalone 출력을 유지합니다.
- `presence_tracker.cpp`는 cluster 저장소가 아니라 로컬 tracker로 유지합니다.
- standalone NAMO Node 한 대와 계층형 다중 Group Site는 같은 안정적인 Site, Space,
  Group 및 participant 식별자를 사용합니다.
- Site 전체 통신은 저빈도로 유지하고 고빈도 observation과 fusion은 제한된 Group 안에 둡니다.
- 크기 제한이 없는 container, queue, log, retry loop 또는 peer list를 허용하지 않습니다.
- Transport 실패를 잘못된 unoccupied transition으로 바꾸지 않습니다.
- 물리 participant, Leader, IP 및 표시 이름이 바뀌어도 논리 Space 자동화 식별자는 바뀌지 않습니다.
- 외부 Satellite는 pairing, capability 검증, trust 및 필요한 보정 전에는 production 상태에
  영향을 줄 수 없으며 Leader나 설정 replica가 될 수 없습니다.
- 공식 attestation은 기기 출처를 증명하지만 로컬 Site 승인이나 observation capability 검증을 대체하지 않습니다.
- Community NAMO Node는 명시적인 로컬 승인으로 지원하되 Leader 자격에는 별도의 opt-in 승인이 필요합니다.
- 미래 설계 문서가 현재 API endpoint 의미를 바꾸지 않습니다.

## 3. 단계 요약

| 단계 | 상태 | 범위 |
| ---: | --- | --- |
| 0 | `implemented` | 아키텍처, protocol, ADR 및 roadmap의 핵심 경계를 고정했습니다. 이후 단계는 소유권과 좌표 규칙을 다시 열지 않고 구현을 확장합니다. |
| 1 | `implemented` | Local Observation Adapter 구현, 실제 기기 smoke 검증 대기. |
| 2 | `implemented` | V1 fixture, validation, bounded simulator 및 호스트 benchmark 구현. V1 인코딩으로 MessagePack 선택. |
| 3 | `in_progress` | 3A, 3B, 3C 검증 완료. Community 자동 발견, 보안 연결, 연결 관리와 양방향·오프라인 정리 UI를 실기기에서 확인했습니다. 공식 attestation과 credential lifecycle은 3D로 분리합니다. |
| 4 | `in_progress` | 고정 크기 Site/Group coordinator, 엄격한 canonical-roster quorum, 독립 term/lease 선출, 인증 heartbeat, RAM 전용 endpoint 추적, 역할 연쇄 방지와 원자적으로 공개되는 역할 snapshot을 구현했습니다. 같은 revision 복구 격리, 적격 후보가 없는 Group의 제한적 삼중 역할 fallback과 후보 등장 시 해제, 제한된 승계 재시도, pairing poll 복구, 통일된 3분 invitation 수명, 인접 roster 전환 중 공간 데이터 평면 연속성 및 독립 Site/Group 승계 안정성은 host 회귀 테스트를 통과했습니다. 단일 Group의 실제 5대 역할 장애·복귀 matrix도 통과했으며 다른 규모와 다중 Group 검증은 남아 있습니다. |
| 5 | `in_progress` | 5A 공유 geometry와 복제, 5B 연결 workflow 및 5C 보정·활성화 경로는 검증 완료했고 기존 4대 Site에 다섯 번째 기기를 추가하는 실기기 workflow도 통과했습니다. 5D 자동 승계는 구현했고 두 Node와 5대 단일 Group 승계·재합류 smoke를 통과했습니다. 5E는 제한된 directory codec, transaction 저장소, 결정적인 단일 Floorplan runtime projection 및 대기열 없는 원본 직렬 처리를 구현했으며 다중 Floorplan 자동 routing이 남아 있습니다. |
| 6 | `implemented` | 고정 메모리 기반 7개 관측원 Shadow Fusion 코어, 개발용 인증 transport·런타임, 승계 후에도 유지되는 canonical spatial profile, 제한된 진단 및 production 비교 수집을 구현했습니다. 반복한 단일 인물 수집에서 지속적인 중복 track episode는 사라졌으며 실제 2인 교차 수집은 production 릴리즈 gate로 남깁니다. |
| 7 | `implemented` | Opt-in production fusion이 기존 target, presence, motion, Zone 및 ESPHome 경로를 하나의 고정 크기 자동화 상태로 연결합니다. 완전한 sequence snapshot, fail-closed 무효화, 안정적인 논리 ID, 로컬 진단과 연장 불가능한 30초 읽기 전용 승계 연속성을 구현했습니다. 5대 승계 연속성 smoke는 통과했으며 혼합 업데이트와 다른 규모 실기기 smoke는 릴리즈 검증으로 남깁니다. |
| 8 | `in_progress` | standalone과 두 Node production build의 내부 RAM·HTTP 자원 gate를 통과했습니다. 각 Node는 선출, transport와 fusion이 함께 사용하는 최대 7대 Group roster 하나를 영속 저장하고, Site Leader는 인증된 저빈도 Group 요약을 최대 8개만 보관합니다. 7대와 8×7 host matrix는 통과했습니다. 실제 4·6·7대 및 다중 Group Site 자원·장애 검증은 남아 있습니다. |
| 9 | `in_progress` | 보안 연결, 연결 관리, 보정, 업로드 및 다각형 직접 작성 평면도, 다중 레이더 배치, 물리 벽과 문 편집, 공용 차폐 렌더링, fusion health 및 융합 타깃 표시를 구현했습니다. 연결된 기기 작업은 반응형 설정 창 하나로 정리했고 pairing trace 도구가 전체 브라우저·기기 workflow를 기록합니다. 전체 local-versus-fused 진단, Satellite workflow 및 더 넓은 브라우저 회귀 검증이 남아 있습니다. |
| 10 | `planned` | SmartThings Site/Space integration. |
| 11 | `implemented` | Home Assistant NAMO Integration. Site별 한 개 등록, Group별 기기, 평면도 없는 단독 기기 처리, 제한된 경로와 권한 검증, Group 진단, 지속 문제 Repairs 알림, 안정적인 공용 자동화 구역, 동적 Space 추가 및 기존 엔티티 정리를 구현하고 집중 호스트 테스트를 통과했습니다. `verified` 전에는 Home Assistant 컨테이너, DHCP, 역할 변경, HACS 및 업데이트 검증이 남아 있습니다. |
| 12 | `planned` | 외부 Satellite SDK 및 conformance suite. |
| 13 | `in_progress` | 직접 업데이트 평면도 축척 복구, 재부팅 회귀 검증, 서명 OTA 및 RAM에만 저장하는 10분·1회 Developer 알파 설치 허용을 구현했습니다. 직접/순차 업데이트 혼합 실기기 smoke, 공개 beta migration 정책, 키를 넣은 실기기 보안 smoke, 문서 및 soak test가 남아 있습니다. |
| 14 | `planned` | 기존 ESPHome X/Y 엔티티를 이용한 단독 NAMO 센서의 선택적 Radar Map Manager 호환. 융합 Group과 NAMO 고급 기능은 이 호환 경로에 포함하지 않습니다. |
| 15 | `planned` | 선택적인 bounded consensus formal model 및 model checking. 실행 가능한 역할·권위 계약이 안정된 뒤, v1.0 또는 quorum·가용성 정책 변경 전에 다시 검토합니다. |

### 구현 완료한 분산 제어 수정

다음 항목은 새 아키텍처 단계가 아니라 현재 제한된 제어 평면의 릴리즈 수정 사항입니다.
구현과 집중 host 회귀 검증은 완료했으며, 4단계를 `verified`로 올리기 전 완료한 5대 단일
Group matrix 밖의 실기기 검증은 여전히 필요합니다.

- **P0 - 같은 revision 복구 격리:** 권위 tracker는 숫자 revision 전체가 아니라 거부된
  의미상 후보 fingerprint만 막습니다. 같은 revision이어도 quorum-backed source, digest, mode
  또는 route가 다르면 정상 수렴 후보로 평가합니다.
- **P1 - 강제 역할 배치 불변식:** 다중 Node Site에서는 Configuration Owner, Site Leader,
  Group Leader를 한 물리 Node에 모두 배정할 수 없습니다. canonical assignment와 live election이
  같은 정책을 사용하며 한 Node standalone은 계속 허용합니다.
- **P1 - 승인된 작업의 견고한 polling:** 승인된 pairing mutation은 한 번만 실행합니다. 일시적인
  status 조회 실패에는 제한된 backoff와 최종 상태 재확인을 사용하고 mutation을 재실행하거나
  UI를 즉시 실패로 바꾸지 않습니다.
- **P1 - 사용자에게 보이는 연결 수명 일원화:** RAM invitation, 등록 session 및 dashboard
  countdown이 동일한 180초 수명을 사용합니다.
- **P1 - 제한된 승계 재시도:** 후보 생성 또는 영속 저장 실패는 같은 의미상 authority
  fingerprint 아래에서 상한과 jitter가 있는 재시도를 예약합니다. eligibility나 fingerprint가
  바뀌면 안정화 fence를 다시 시작합니다.
- **P1 - Owner와 독립된 Group 권위:** Group election-view identity에서 순간적인 credential
  준비 상태를 제외합니다. credential 완전성은 후보의 필수 조건으로 유지하며, 살아 있는 Group
  Leader는 Configuration Owner 승계와 과거 Owner 재합류 중에도 term을 유지합니다. heartbeat
  v10은 새 digest를 제공하고 순차 업데이트 중에는 v9 호환 view를 함께 전송합니다.
- **P1 - role term 단일 발급자:** heartbeat v11은 election 승인을 선택된 winner와 결합합니다.
  그 winner만 새 Site 또는 Group term을 발급하며 follower는 term을 별도로 증가시키지 않고
  채택합니다. 살아 있거나 보류된 같은 Leader는 term을 유지하고 실제 교체일 때만 한 번
  증가합니다. 순차 업데이트 중 current, v10 및 v9 호환 view를 함께 검증합니다.
- **P1 - 안정적인 제한형 election identity:** heartbeat v12는 설정된 전체 canonical roster,
  roster revision 및 선택된 winner를 결합하고, 생존 상태, Directory 상태 및 credential 준비
  상태는 독립된 admission fence로 유지합니다. 순차 업데이트 중 정확한 v11 호환 계약을
  전송하며, 3~7대 혼합 v11/v12 Site 및 Group 계약 수렴을 호스트 테스트로 검증했습니다.
- **P1 - 비파괴 Owner credential 갱신:** canonical Owner 또는 authority epoch 변경은 진행 중인
  credential bundle만 취소하고 새 authority에서 다시 갱신합니다. 인증 endpoint, Site·Group
  coordinator와 term, summary 및 production fusion은 보존합니다. 완전한 기존 credential
  snapshot은 제한된 handover bootstrap으로 heartbeat와 observation 전송을 유지하며, 불완전한
  snapshot은 fail-closed합니다. 파괴적인 runtime 초기화는 실제 Site scope 변경이나 명시적인
  platform reset에만 사용합니다.
- **P1 - 수직 workflow 증명:** production dashboard pairing client를 stateful firmware-contract
  harness와 연결해 invitation, commit, Floorplan 복제, 보정, 양방향 활성화, peer 장애 및 정상
  재합류까지 실행합니다. native role test는 C++ 권위와 영속 경로를 별도로 검증합니다.
- **P1 - 독립 Leader 승계:** 역할 없는 후보나 Owner만 맡은 후보가 있으면 Site Leader 교체가
  현재 canonical Group Leader를 가져가지 않습니다. 무관한 Site 승계 중 기존 Group Leader와
  term을 그대로 유지합니다.
- **P1 - 일관된 공개 역할 상태:** `/api/platform/groups`는 HTTP 요청 도중 변경되는 선출 상태를
  직접 섞어 읽지 않고 한 번 게시한 Site/Group coordinator generation을 직렬화합니다.
- **P1 - 제한된 자동화 연속성:** 순방향 Group Leader 승계는 마지막 완전한 자동화 상태를 최대
  30초 동안 비권위·읽기 전용으로 보존할 수 있습니다. 펌웨어나 Home Assistant의 반복 전송 실패가
  마감 시간을 다시 시작할 수 없으며 위험한 설정·공간 불일치는 계속 즉시 fail-closed합니다.
- **P1 - 인접 roster 전환 연속성:** 같은 authority epoch에서 generation과 roster revision이
  정확히 한 단계 차이 나는 현재 프로토콜 Node는 credential과 Floorplan·레이더 pose가 모두
  일치할 때만 기존 heartbeat와 fusion을 유지합니다. 공유 쓰기, 원본 광고 및 Group 요약은 정확한
  canonical revision이 수렴할 때까지 계속 차단합니다. epoch 변경, 두 단계 이상 차이, 구형 wire
  version 또는 공간 계약 불일치는 이 예외를 사용할 수 없습니다.
- **P1 - 비파괴 Leader 자격 변경:** 자동 대체 자격을 바꾸면 canonical roster만 갱신하며 현재
  Site 또는 Group term을 초기화하지 않습니다. 인접한 이전·신규 view는 공간 계약이 일치하는
  traffic만 유지하고 공유 쓰기는 계속 차단하며, roster가 수렴하면 일반적인 정확한 revision
  계약으로 돌아갑니다.

각 수정은 집중 browser·native 회귀 테스트와 seed 기반 1~7대 역할 case로 검증했고 전체
host suite와 프로세스 기반 2~7대 cluster matrix도 통과했습니다. 5대 단일 Group 역할 장애·복귀
실기기 smoke도 통과했으며 다른 규모와 다중 Group 검증은 남아 있습니다. 5대 설정에서 생존 기기 2대만으로
계속 운용하는 survivor-membership 정책은 별도 보류 항목이며, 이번 수정은 엄격한 과반수
계약을 조용히 바꾸지 않습니다.

## 4. 단계별 내용

### 0단계: 설계 고정

상태: 구현 완료. 소유권 경계, 좌표 규칙, trust 분류, standalone 호환 규칙, 독립적인
Site/Group Leader 역할 및 계층형 용량 목표를 영문·국문 아키텍처, protocol, ADR 및
roadmap에 고정했습니다. 이후 단계는 버전이 있는 계약을 확장할 수 있지만 이 경계를 조용히
대체해서는 안 됩니다.

산출물:

- 도메인 명칭과 책임 경계 승인;
- 미결정된 좌표 axis 및 angle 단위 결정;
- 첫 릴리즈 범위와 명시적인 비목표 고정;
- 고정 Primary 기반 다중 센서 메모를 대체됨으로 표시;
- 계획 endpoint를 현재 API 계약에서 제외.

완료 조건:

- 영문 및 국문 아키텍처, protocol, roadmap, ADR 내용이 일치합니다.
- 어떤 문서도 계획 상태를 현재 제공 기능처럼 설명하지 않습니다.
- 8개 Group Site와 7개 입력 Group 한도를 서로 독립적인 설계 및 테스트 목표로 유지합니다.

### 1단계: Local Observation Adapter

상태: 구현 완료, 실제 기기 smoke 검증 대기. Adapter는 내부 shadow 상태로만 존재하며 network endpoint를
추가하거나 Home Assistant custom integration을 요구하지 않습니다. 단독 센서의
공개 상태와 local presence 판정도 변경하지 않습니다.

산출물:

- 확정된 local track과 제한된 비공간 근거를 내부 Observation 모델로 변환;
- local acquisition, filtering 및 presence 판정 동작 유지;
- 기존 tracker에서 향후 spatial code 분리.

테스트 및 완료 조건:

- 현재 native tracker 및 replay 테스트 통과;
- Fusion OFF에서 안정적인 공개 상태 fixture가 byte 단위로 동일;
- 선택적인 sensor hardware가 없어도 조용히 무시.

### 2단계: Protocol Prototype과 Simulator

상태: 호스트 prototype 범위에서 구현 완료. 공용 좌표 규칙, V1 논리 fixture, 호스트용 validator,
bounded Satellite sender, latest-wins 수신 inbox 및 JSON/MessagePack benchmark를 구현했습니다.
펌웨어에 호환 구현이 이미 있고 사설 binary codec을 유지하지 않아도 되므로 V1 인코딩은
MessagePack으로 정했습니다. 이 단계 자체는 Production transport나 Home Assistant integration을
활성화하지 않으며 community pairing은 별도 3C 단계에서 구현합니다. Production transport 전에 실제 기기의 heap/시간/부하 검증을 반드시
통과해야 합니다.

산출물:

- 버전이 있는 descriptor, Observation, Group state 및 error fixture;
- JSON debug와 펌웨어에 이미 포함된 MessagePack data-plane 후보 benchmark;
- embedded peer 통신 전에 Python Satellite simulator 구현;
- payload, track, rate, queue depth 및 expiry 고정 제한 정의.

테스트 및 완료 조건:

- malformed, oversized, stale, duplicate, reordered 및 restarted stream;
- 서로 다른 optional capability 조합;
- simulated Group participant 1개, 2개, 6개 및 7개;
- wire 크기만이 아니라 실측값과 저장소 dependency 비용을 함께 보고 MessagePack 선택.

### 3단계: Site 가입, Trust 및 Attestation

상태: 구현 중입니다. 검증된 저장·admission 기반과 아직 구현하지 않은 network trust를 혼동하지
않도록 다음 세 경계로 나눕니다.

- **3A Identity, Registry 및 Readiness (`verified`):** 안정적인 identity, transaction 기반
  membership/readiness 저장, 제한된 로컬 관리, fail-closed shadow admission을 구현하고
  실기기에서 검증했습니다.
- **3B 인증된 Shadow Transport (`verified`):** 크기가 제한된 인증 호스트 wire contract,
  golden vector와 개발 전용 firmware 수신 경로를 구현했습니다. 원격 observation은 진단에만
  수신하며 production presence나 fusion에는 영향을 주지 않습니다. 전송·정리·재시작 실기기
  smoke까지 stack overflow나 rollback 없이 통과했습니다.
- **3C Community 가입 및 연결 관리 (`verified`):** transaction 기반 credential 저장,
  안전한 community key 교환, 요청형 자동 발견, 독립 기기 연결 탭과 재시도 가능한 양방향
  연동 해제를 구현했습니다. 수동 주소 연결과 자동 발견은 두 실제 기기 smoke를 완료했으며,
  새 관리, 양방향 해제 및 오프라인 정리 UI까지 실기기에서 확인했습니다.
- **3D 공식 Attestation 및 Credential Lifecycle (`planned`):** 제조 기기 출처 증명,
  certificate/root rotation, revoke, 교체와 복구는 community 가입과 분리해 진행합니다.

현재 구현된 heap-free Participant Registry는 원격 participant를 최대 19개 저장하며 로컬 NAMO
Node를 합쳐 평평한 20개 저장 경계를 지킵니다. 이는 기존 Site 저장 경계이며 목표 계층형 용량
모델이 아닙니다. Quarantine, 명시적 승인, 폐기, trust/type 호환성과 Leader 자격
규칙도 구현합니다. Runtime은 전체 hardware MAC으로 안정적인 `nodeId`를 만들고 standalone용 random
`siteId`를 발급합니다. 활성 V2 저장소는 변경되지 않는 Node identity와 변경 가능한 Site membership을
분리하며, 기존 결합형 V1 record를 ID 변경 없이 이전합니다. Membership과 registry 변경은 두 slot과
checksum이 있는 commit pointer를 사용하므로 write 중단 시에도 완성된 이전 또는 새 generation만
남습니다. Registry가 손상되면 standalone 감지는 유지하고 원격 참여만 비활성화합니다. Host test로
migration, commit 복구, 손상, write 실패, Node identity 불변성, registry lifecycle 복원과 로컬
1대+원격 19대 한도를 검증했습니다. 실기기 검증에서는 setup task의 과도한 registry scratch와
`httpd` task의 중첩된 HTTP 응답 생성이라는 서로 다른 stack 위험이 드러났습니다. Store가 여러 blob과
snapshot을 task stack에 놓는 대신 크기가 제한된 재사용 workspace 하나를 소유하도록 수정했습니다.
HTTP routing은 server가 소유하는 URL buffer 하나를 재사용하고, `/api/system/status`는 응답을
제한된 크기의 여러 구간으로 나누어 쓰며, platform status 생성에서는 큰 로컬 serialization 배열을
제거했습니다. 생성된 ELF에서 committed-load stack frame은 4,080바이트에서 80바이트로, registry 초기화
frame은 1,376바이트에서 32바이트로, 최상위 request 처리는 48바이트로, platform status 생성은
80바이트로 줄었습니다. 펌웨어 `0.5.5-m`은 실기기에서 OTA 부팅, setup, 소프트 재시작,
registry/readiness 복원과 lifecycle smoke를 완료했으며 `httpd` stack overflow나 rollback이 다시
발생하지 않았습니다. 안정적인 identity는 `/api/system/status`와 `/api/platform/descriptor`를 통해 읽기 전용으로
제공합니다. `_namo._tcp` 광고는 사용자가 요청할 때만 3초 동안 최대 원격 후보 19개를
찾는 자동 발견에 사용하고 주소 수동 입력 fallback도 유지합니다. 발견 자체는 trust를
부여하거나 등록 창을 열지 않습니다. Pairing
window를 최대 5분으로 제한하고 승인되지 않은 후보를 RAM에 하나만 유지하는 heap-free enrollment
core도 구현했습니다. 승인 성공 시 window를 닫고 trust를 저장하지만 참가자는 quarantine 상태로
유지하며, 활성화는 별도로 저장되는 변경입니다. Leader opt-in, quarantine, revoke 및 forget도 같은
transaction registry 경로를 사용합니다. 저장 실패는 live registry를 바꾸지 않으며, attestation
verifier가 생기기 전에는 `official_verified`를 거절합니다. 제한된 window와 registry lifecycle을
credential 노출 없이 제공하는 로컬 GET/POST participants API를 구현했으며, `pairingEnabled`는 이
로컬 window가 열려 있을 때만 `true`입니다. 실기기 lifecycle smoke에서 stage, 승인, readiness 설정,
활성화, quarantine, forget, 정리와 재부팅 복원을 확인했습니다. 빠르게 바뀌는 registry generation 숫자는
완료 근거로 사용하지 않습니다. 수동 주소 기반 사용자 연결과 자동 발견은 두 실제 기기에서
완료했습니다. 독립 연결 관리 탭, 양방향 연동 해제와 오프라인 정리도 실기기에서 확인했습니다. Production
observation 권한은 표준 NAMO 제품 빌드의 gate된 7단계 adapter에서만 사용할 수 있으며,
공식 기기 attestation 검증은 아직 구현하지 않았습니다.

산출물:

- 안정적인 Site, NAMO Node, Satellite 및 공통 participant 식별자;
- discovery와 주소 수동 입력 fallback;
- 명시적인 pairing window, 승인, 폐기 및 quarantine;
- `official_verified`, `community_unverified`, `satellite`, `unknown` trust 등급;
- 제조 NAMO 기기용 기기별 private key와 certificate 발급 계약;
- manufacturer root, challenge-response, rotation, revoke, 교체 및 복구 규칙;
- 8단계 계층 전환 전까지 유지하는 원격 participant 19개와 로컬 Node 1개의 현재 평평한 registry;
- Site config version 및 배정된 Floorplan, Space, Group.

테스트 및 완료 조건:

- DHCP 주소 변경이 새 participant를 만들지 않습니다.
- 알 수 없거나 폐기된 participant observation을 거절합니다.
- 재시작 시 안정적인 participant identity는 유지하고 `bootId`만 바뀝니다.
- replay, 잘못된 제품, 만료, 신뢰하지 않는 root 및 revoke certificate attestation fixture를 거절합니다.
- public API나 진단으로 private key를 반환하지 않습니다.
- attestation만으로 membership, capability 또는 Group 배치를 부여할 수 없습니다.
- Community NAMO Node는 명시적 승인과 별도 Leader opt-in이 필요합니다.
- 21번째 활성 participant는 크기가 제한된 오류로 거절합니다.
- pairing window 만료, 후보 충돌, 저장 rollback 및 재부팅 복원이 마지막 commit generation을 보존합니다.

추가로 fusion을 켜지 않은 admission 기반을 구현했습니다. 기존 participant registry 저장
형식은 바꾸지 않고, 같은 19개 원격 slot에 capability, Floorplan/Space/Group 배정,
pose version, calibration version을 저장하는 별도 transaction readiness registry를
추가했습니다. 승인은 계속 quarantine에서 끝납니다. Presence-only 참여자는 완전한 비공간
profile이, spatial 참여자는 0이 아닌 pose/calibration version까지 준비되어야
`activate`할 수 있습니다. active profile을 바꾸면 먼저 quarantine을 저장합니다.

Heap-free shadow admission gate는 unknown/inactive 참여자, 잘못된 Site/Group,
capability 위반, pose/calibration version 불일치, 만료 sample, 중복/역순 sequence를
거부합니다. 개발 전용 build에서는 인증된 network observation을 제한된 shadow 진단으로
받지만 production presence를 바꾸지는 않습니다. 일반, beta, release build에서는 수신기를
생성하지 않습니다.
호스트 테스트는 저장 rollback, 재부팅 복원, activation gate, 재설정 quarantine,
정리, TTL, version, sequence 거부를 검증합니다. 실제 기기의 전체 lifecycle은
`tools/participant_readiness_smoke.py` 한 번으로 묶어 확인할 수 있습니다. 기본 실행은
기기를 재부팅하지 않습니다. 이 도구는 `satellite-smoke-*`로 예약된 record만 제거하고,
정리 전에 pairing을 닫으며, 불필요한 마지막 요청을 추가하지 않고 정리 작업 안에서 삭제를
검증합니다. ESPHome이 정상 부팅을 확정한 뒤 재부팅 저장 복원을 수동으로 확인하는 과정을
별도 실기기 승인 단계로 유지합니다. 이 3단계 transport smoke 경로에서는
`fusionEnabled=false`를 유지하며, 표준 제품 빌드의 gate된 7단계 adapter만 융합 상태를
게시할 수 있습니다.

Participant registry와 readiness registry의 반복 transaction 저장은 더 이상
20 KiB ESPHome NVS 파티션을 사용하지 않습니다. 기존 radar data 파티션 끝의 고정
4 KiB sector 6개에 각각 두 slot과 commit record를 저장합니다. 업데이트 후 첫 부팅에서는
유효한 legacy NVS transaction을 새 영역에 먼저 복사한 뒤 legacy key를 제거합니다.
마이그레이션에 실패하면 NVS 원본은 그대로 보존하고 원격 참여만 비활성화하며,
standalone 재실 감지는 계속 동작합니다. Host test는 원격 participant 19개가 가득 찬
상태의 마이그레이션, 중단된 마이그레이션, 반복적인 최대 용량 업데이트를 검증합니다.

`--transport-smoke` 실기기 검증에서는 정상 HMAC/MessagePack observation 수락, 잘못된 인증,
초당 rate 초과와 중복 sequence 거부를 확인합니다. 이 과정에서 로컬 presence, 구역,
Home Assistant 상태 및 standalone 재시작이 바뀌지 않아야 합니다.

3단계의 남은 구현에 적용할 임베디드 자원 제한:

- 큰 request, response, registry 또는 readiness 배열을 ESPHome task stack에 두지 않습니다.
- 큰 JSON 응답은 제한된 크기의 여러 구간으로 나누어 전송합니다.
- 저장 변경은 transaction으로 유지하되 불필요한 연속 write는 만들지 않습니다.
- 새 firmware transport 묶음을 활성화하기 전에 생성된 ELF stack frame을 확인합니다.
- firmware 경계를 `verified`로 표시하기 전에 실기기에서 standalone 부팅, 소프트 재시작,
  로컬 감지 및 정리 smoke를 확인합니다.

2026-08-19까지 기록한 현재 실기기 smoke 기준선:

| 검증 항목 | 현재 결과 | 완료 상태를 유지하기 위한 근거 |
| --- | --- | --- |
| Standalone 부팅, 로컬 감지 및 소프트 재시작 | 통과 | rollback 또는 stack overflow가 없는 serial 부팅 로그 |
| Participant lifecycle, 정리 및 재시작 후 lifecycle | 통과 | 재시작 전후 `participant_readiness_smoke.py` |
| 두 Node의 발견, 보안 연결, heartbeat 단절 및 DHCP 주소 복구 | 통과 | 양쪽 기기 연결 화면에서 현재 상대 주소가 다시 표시됨 |
| Owner 평면도와 로컬 레이더 위치의 반복 저장 | 통과 | 새로고침 후 저장값 유지 및 `httpd` abort 없음 |
| 인증된 owner-to-follower 평면도 복제와 대시보드 활성화 | 통과 | replica는 유지되고 Node별 레이더 위치와 구역은 로컬에 남음 |
| 내부 heap 단편화, HTTP stack, firmware 및 저장소 margin | 통과 | 실기기에서 `device_resource_smoke.py` 통과. release 검증 시 JSON report 보관 |
| 두 Node snapshot 보정, 승인 및 양방향 활성화 | 통과 | 양쪽이 같은 geometry revision과 digest로 활성화되고 승인 transform은 로컬 레이더 위치를 덮어쓰지 않음 |
| 기존 4대 Site에 다섯 번째 기기 연결·보정·활성화 | 통과 | 연결 추적에서 invitation, pairing commit, 평면도 복제, configure, 양방향 activate 및 최종 5/5 canonical 수렴 완료 |
| canonical Leader 합의, 수동 takeover 및 정상 재합류 | 통과 | `site_leader_handover_smoke.py --handover`가 takeover와 전원 복구 후 rejoin을 확인 |
| 5대 단일 Group의 Owner·Site·Group 역할 장애 및 재합류 | 통과 | 역할 한 대 장애와 세 가지 역할 기기 두 대 부재 조합에서 생존 3/5의 자동화를 유지했고 Site Leader, Group Leader 및 설정 writer 중복이 없었음. 2026-08-19 재검증에서는 Owner와 Site Leader 부재·복귀 중에도 fusion authority가 유지되고 최종 5/5로 수렴함 |
| 5대 자동 대체 자격 변경 후 전원 차단·복귀 | 통과 | 자격 변경 중 Site와 Group term을 유지하고 여러 기기가 오프라인으로 잘못 표시되지 않았으며, 이어진 대상 기기 전원 차단·복귀 후 정상 5/5 roster로 수렴함 |
| 손실·과부하 상태의 7개 입력 Group과 8개 Group Site | 미검증 | 8단계 실기기·simulator 부하 행렬 |

기본 자원 gate는 내부 heap 최소 32 KiB, 내부 최대 연속 block 16 KiB,
HTTP task stack 최소 여유 2 KiB, firmware slot 여유 512 KiB, 저장소 여유
128 KiB이며 제한된 읽기 전용 부하 중 내부 여유 감소는 최대 32 KiB입니다.
이는 초기 safety gate이며 계층형 용량 지원을 검증했다는 의미가 아닙니다.

장시간 실제 사용 관찰과 제한된 인공 부하는 분리합니다. `window` 범위는 실행 전에 이미
발생했고 실행 중 더 낮아지지 않은 누적 최저치를 경고로만 남기며, 실행 중 새로운 최저치가
생기면 실패합니다. 펌웨어는 내부 RAM의 새로운 최저치를 확인한 최근 작업 경계를 PSRAM
platform workspace에 고정 크기로 기록합니다.

실기기 실행 결과는 다음 두 가지로 보관합니다.

```text
python tools/device_resource_smoke.py --device http://DEVICE_IP --mode observe --scope window --duration-seconds 3600 --interval 5 --output device-resource-observe.json
python tools/device_resource_smoke.py --device http://DEVICE_IP --mode stress --scope lifecycle --rounds 30 --concurrency 4 --output device-resource-stress.json
```

3B는 검증 완료된 ADR, 768바이트 UDP unicast 인증 envelope, MessagePack/HMAC golden vector,
호스트 conformance·오용 테스트와 명시적으로 켜는 firmware 수신 전용 shadow 경로까지
구현했습니다. 이 경로는 component loop당 최대 2패킷과 participant별 대기 observation 하나로
제한되며 production state 권한이 없습니다. Membership credential은 Home Assistant API key 및
제조사 attestation key와 분리합니다. 실기기 transport·정리·soft restart·재시작 후 lifecycle
smoke가 stack overflow나 rollback 없이 통과했으므로 3B는 verified 상태입니다.

3C는 같은 원격 참가자 19개 한도 안에서 동작하는 credential registry를 추가했습니다.
Participant ID, 불투명 token, key version, enabled 상태와 32-byte transport key를 task
stack에 큰 snapshot을 만들지 않고 transaction으로 저장합니다. Transport는 이 owner를
참조하며 rate counter만 RAM에 별도로 둡니다. 공개 API는 metadata만 반환하고 token/key는
반환하지 않습니다. Revoke는 credential을 비활성화하고 forget은 삭제하며 registry membership이
최종 fail-closed 권한을 가집니다. 개발용 smoke key는 RAM에만 존재합니다.

Community 연결은 고정 크기 RAM session 하나와 command slot 하나를 사용합니다. 임시
P-256 ECDH, SHA-256 transcript 결합, HKDF-SHA256, 역할별 HMAC proof와 양쪽
대시보드에서 확인하는 6자리 코드를 적용했습니다. HTTP는 작업을 queue에 넣기만 하고
암호 연산과 transaction 저장은 component loop에서 수행하므로 `httpd` stack에
타원곡선 및 저장 frame을 얹지 않습니다. 확인이 끝나면 상대 Node는
`community_unverified`, quarantined 상태와 인증된 제어 경로에 사용하는 활성 pair credential로 저장됩니다.
Joiner는 registry와 credential 저장 뒤에만 Site membership을 바꾸고 configuration
owner는 미확정으로 유지하며, 인증된 canonical pull로만 owner를 받아들입니다. Leader는
복제 전에 단 하나의 owner를 확정합니다. 같은 두 기기의 역할 교체는 사용자가 확인한 pairing
commit에서, 세 번째 관계를 고립시키지 않을 때만 허용합니다. 마지막 관계를 제거하면 RAM
pairing 상태를 초기화하고 같은 Site를 로컬 기기 한 대의 standalone 설정으로 확정합니다.
원격 owner를 추측하거나 새 Site identity를 만들지는 않습니다. Standalone 감지와 Home
Assistant 동작은 그대로입니다. 표준 NAMO 제품 빌드는 fusion capability를 포함하지만 7단계
production adapter는 Site 및 Group 권한 gate를 통과한 뒤에만 authoritative 상태가 됩니다. Host test는 transcript
일치, proof/code 거부, 만료, one-slot backpressure, 지연 실행과 commit gate를
검증합니다. 대시보드는 별도 2단 기기 연결 탭에서 진입 시 제한된 검색 한 번을 시작하고,
사용자가 같은 검색을 명시적으로 다시 실행할 수 있습니다. 이미 등록된 Node는 새 연결 목록에서
제외하며 발견된 주소는 현재 대시보드 세션에서만 유지합니다. 수동 주소 입력도 fallback으로
유지합니다. 연동 해제는 상대 identity를 확인하고 상대쪽 관계를 먼저 삭제·검증합니다. 상대가
응답하지 않으면 로컬 credential을 revoke해 연결 목록에서 `연동 해제 대기`로 분리하고,
상대가 다시 발견됐을 때 양방향 정리를 재시도할 수 있습니다. 사용자가 로컬 기록만 완전히
삭제하는 경우에는 상대 기록이 남을 수 있음을 명시합니다. Host test는 자기 자신 제외, 호환성
필터, 중복 제거, 원격 후보 19개 제한, 등록 후보
제외, 양방향 처리 순서, identity 불일치와 상대 정리 실패를 검증합니다. 새 관리, 양방향 연동 해제와
오프라인 정리 UI의 실기기 smoke도 완료했습니다. 제조사 attestation, key rotation과 교체 복구는 3D 범위입니다.

3C 연결 복구 경계는 성숙한 Site에 대한 1회성
`authenticated_enrollment`으로 정리했습니다. 확인된 상대의 활성 credential과
상호 quarantine만 재부팅 후 복구 근거로 사용하며, 정확한 canonical Owner와
로컬 Node가 포함된 roster만 높은 epoch를 받아들입니다. 수렴 후 예외는 닫힙니다.
설정 원본이 아닌 기기의 연결 시작은 명시적으로 거절하고 온라인 설정 원본으로
이동시키며, 확정된 Site의 평면도는 새 기기 평면도로 교체하지 않습니다. 응답 유실,
잘못된 Owner, 다른 연결 잔재, roster 누락, 재시도 멱등성 회귀 테스트를 포함합니다.

### 4단계: Group과 Leader

현재 구현 상태:

- heap을 쓰지 않는 coordinator가 active readiness record에서 Group membership을
  파생합니다.
- Node 한 대인 Site도 같은 term/lease 계약을 따릅니다.
- 결정적 선출, Leader 자격, heartbeat 만료, 재시작 sequence, 이전 term 거부와
  현재 평평한 registry 메모리 한도에 호스트 테스트가 있습니다.
- `/api/platform/groups`는 production 재실이나 fusion에 영향을 주지 않는 읽기 전용
  shadow health를 제공합니다.
- 기기 연결 화면은 인증된 등록과 양방향 연동 해제를 완료하지만, 서로 독립적으로 저장된 로컬
  평면도에서 readiness나 활성화를 파생하지 않습니다. 연결된 peer는 5단계에서 도입할 Site 설정
  owner가 공유 geometry와 Node별 배치를 commit할 때까지 quarantine 상태를 유지합니다.
- 일반 펌웨어는 UDP 49191에서 pairwise HMAC 인증 heartbeat를 교환하며, active이고
  readiness가 완료된 peer를 고정 크기 Shadow Group 상태에 반영합니다.
- ESP-IDF build는 기존 HTTP/API/mDNS와 heartbeat/replication UDP 전송이 함께 동작하도록
  lwIP socket 한도를 20개로 명시합니다. 일반 대시보드 요청에서 HTTP accept 경로가 더는
  고갈되지 않는지는 실기기 회귀 검증이 남아 있습니다.
- peer IP는 안정적인 participant ID로 식별하고 UDP 송신 주소로 인증하며, DHCP 변경 시
  RAM에서만 갱신하고 flash에는 저장하지 않습니다.
- 별도의 heap-free Site coordinator가 타깃 observation을 받거나 전달하지 않으면서 active이고
  Leader 자격이 있는 NAMO Node 하나를 결정적으로 선출합니다. term과 lease는 인증 heartbeat의
  기존 `siteTerm`을 사용합니다.
- canonical roster가 확정되면 모든 Node의 Site 및 Group Leader 자격은 이 roster 하나를
  원본으로 사용합니다. 기기별 participant record는 인증 및 생존 확인 경계로만 남습니다.
- 다중 Node 권한에는 설정된 canonical membership의 엄격한 과반수와 일치하는
  election-view digest가 필요합니다. 현재 보이는 Node만으로 과반수를 다시 계산하지 않으며,
  생존 구성원 변경은 별도의 명시적인 설정 정책입니다.
- 원격 Leader를 채택할 때는 인증된 term을 그대로 유지하고 한 번 더 증가시키지 않습니다.
  선택된 로컬 winner만 살아 있는 cluster에서 관측한 term보다 새 교체 term을 발급하며,
  재시작한 후보가 로컬 초기값으로 그 term을 덮을 수 없습니다.
- 온라인인 적격 Node가 없을 때 부적격 로컬 Node가 자신을 Leader로 선출할 수 없으며,
  active인 비Leader 참가자는 Site health 계산에는 포함됩니다.
- `/api/platform/groups`는 Group snapshot과 함께 독립 Site Leader snapshot을 제공합니다.
  7단계 production adapter는 선출된 Group Leader에서 모든 runtime gate를 통과한 뒤에만
  active 권한을 보고할 수 있습니다.
- `tools/site_leader_handover_smoke.py`는 양방향 active/readiness/credential 상태,
  Site 및 Group Leader 합의와 canonical 후임 자격을 먼저 확인한 뒤 수동 lease 전환과
  정상 재합류를 검증합니다. API 재부팅이나 Leader 자격 변경은 수행하지 않습니다.
- 기기 연결 화면에서 설정 원본 기기는 active Community NAMO Node의 자동 대체
  조정 권한을 명시적으로 허용하거나 해제할 수 있습니다. 현재 온라인인 Leader 자격
  보유 Node가 한 대뿐이면 낮은 강도의 경고를 표시합니다. canonical
  `leaderEligible`을 그대로 사용하므로 이후 verified 기기 attestation은 UI나
  schema를 교체하지 않고 같은 권한을 자동 부여할 수 있습니다.
- canonical roster 자격을 적용한 active 두 Node가 같은 Site/Group Leader에 수렴했고,
  현재 Leader 전원 차단 후 적격 Node의 takeover와 전원 복구 후 정상 재합류를
  `site_leader_handover_smoke.py --handover`로 실기기에서 확인했습니다. 권위 있는 공유 설정
  commit과 replica 동기화는 5단계에 구현되어 있습니다.
- 단일 Group의 실제 5대 Site에서 Configuration Owner, Site Leader 및 Group Leader의
  전원 차단·복귀와 세 가지 역할 기기 두 대 부재 조합을 검증했습니다. 생존 과반수는 자동화를
  계속 제공했고 Group 승계에는 제한된 연속성이 적용됐습니다. 과거 권위 기기는 복귀 후 역할을
  되찾지 않았으며, 수집한 역할 sample 252개에서 Site Leader나 Group Leader 중복은 없었습니다.
  Configuration 수렴 중 writer도 두 대 이상 동시에 열리지 않았습니다.

산출물:

- 저빈도 Site topology와 Group-local heartbeat;
- 승인된 NAMO Node로 제한한 Group Leader 자격 및 결정적인 lease 선출;
- 승인된 NAMO Node로 제한한 설정 전용 저빈도 Site Leader;
- 활성 Fusion Group마다 NAMO Node 최소 한 대;
- schema와 runtime에서 Satellite leadership 및 설정 복제를 거절;
- term, lease, snapshot 및 오래된 권한 처리.

테스트 및 완료 조건:

- Leader 재시작, 격리, 복구 및 늦게 도착한 이전 패킷;
- 하나의 term에서 중복 Leader를 받아들이지 않습니다.
- Leader 전환 중 occupancy가 잘못 OFF 되지 않습니다.
- NAMO Node 한 대 Group도 같은 권한 계약을 따릅니다.

### 5단계: Geometry와 Calibration

현재 구현 상태:

| 하위 단계 | 상태 | 범위 |
| --- | --- | --- |
| 5A | `verified` | 버전이 있는 Floorplan identity, 권위 있는 manifest, 제한된 인증 chunk 복제 및 원자적 replica 저장 |
| 5B | `verified` | 한쪽에서 진행하는 연결 마법사, 공유 평면도 선택, 취소 후 선택 단계 재개, 로컬 pose·구역 보존 및 빈 평면도 동기화. 기존 4대 Site에 다섯 번째 기기를 추가하는 실기기 연결 workflow 통과 |
| 5C | `verified` | 두 Node snapshot, 제한된 이동·회전 보정안, 사용자 승인, readiness 저장, 양방향 활성화 및 기존 4대 Site에 새 기기를 보정·활성화하는 실기기 smoke |
| 5D | `implemented` | 결정적 자동 설정 원본 승계, 영속 authority epoch, 설정 digest fencing, fail-closed 분기 감지, 사용자가 원본을 지정하는 제한된 복구, roster·manifest 보존, 인증 수렴 및 저장 실패 반복 방지를 구현했습니다. 두 Node와 5대 단일 Group 승계·재합류 smoke는 통과했으며 정식 4·6·7대와 다중 구성원 인증 관계 검증이 남아 있습니다. |
| 5E | `in_progress` | Floorplan별 독립 Configuration Owner 한 대, 물리 NAMO Node당 소유 Floorplan 하나 및 제한된 원본 directory. codec, ESP32 transaction flash 저장소, 부팅 호환 mirror, 결정적인 단일 Floorplan runtime projection, 명시적 동기화 원본 해석기, manifest gate 및 대기열 없는 단일 follower 원본 lease를 구현했으며 다중 Floorplan 자동 routing이 남아 있습니다. |

Canonical Site Directory fork를 자동으로 복구하는 반복 경로는 제거했습니다. 정상 pairing, 자격 변경
및 역할 승계는 canonical 권한을 다시 쓰지 않고 기존 enrollment, 복제 및 선출 계약으로 수렴해야
합니다. 실제로 지속되는 충돌이 증명된 경우에만 사용자가 원본을 선택하는 `resolveConflict` 작업을
마지막 복구 경로로 유지합니다.

현재 single-Floorplan NAMO Node 연결 범위는 5B와 5C에서 닫습니다. 5E의 남은 작업은 향후
다중 Floorplan directory에 연결 결과를 게시하고 Floorplan별 승계를 수행하는 확장이며,
현재 연결 workflow를 다시 미완료로 돌리는 항목이 아닙니다.

현재 5D tracker는 영속 승계 실패 뒤 반복 쓰기를 억제하며 eligibility가 바뀌거나 재부팅될
때까지 terminal 상태에 남을 수 있습니다. P1 수정 gate는 이를 제한된 jitter와 authority fence가
있는 retry로 바꾸되 무한 flash-write loop는 허용하지 않습니다.

5A부터 5D까지는 현재 구현된 단일 Floorplan·단일 Configuration Owner Site schema를
설명하며 호환 기준선으로 유지합니다. 5E는 단수 owner/manifest 필드를 versioned 고정 크기
Floorplan directory로 교체합니다. 현재 API에서 이 구조를 추측해 사용하면 안 됩니다.

5E 기반으로 최대 Floorplan 8개와 Group 8개를 담는 별도
`SiteDirectorySnapshot`, checksum이 있는 4 KiB codec, 추상 two-slot commit
저장소와 A/B/commit용 ESP32 flash sector 3개를 추가했습니다. 부팅할 때 현재 단일
committed Floorplan을 Group Leader 추측 없이 호환 mirror로 변환합니다. 이 mirror는
기존 단일 Floorplan 원본이 달라졌을 때만 갱신하며, 이후 정식 다중 Floorplan
directory가 만들어지면 해당 경로가 덮어쓰지 않습니다. 네이티브 테스트는 최대 크기
round trip, 설정 원본·Group Leader 중복 거절, 손상, commit marker 복구, 중단된
commit, 기존 platform sector 주소 보존, Group당 participant 7개 및 8개 Group
전체의 participant reference 합계 56개 경계를 검증합니다. 이 합계는 평평한 Site
전체 기기 registry 제한이 아닙니다. 선출된 로컬 Site Leader는 Floorplan manifest,
모든 active 구성원의 공간 배정, Group 크기와 각 Group의 합의된 Leader가 완전할 때만
호환 mirror를 transaction 방식의 단일 Floorplan projection으로 승격합니다. 이 전환
record에는 출처가 명시되어 현재 설정의 갱신·삭제를 따라가지만, 이후 정식 다중
Floorplan directory를 덮어쓸 수 없습니다. 명시적 `sync`는 이 projection에서 로컬 Group에
배정된 Floorplan 원본을 선택하고 기존 인증 chunk 전송을 해당 manifest에 묶습니다.
명시한 원본이 다르거나 반환된 manifest가 다르면 닫힌 상태로 거부합니다. heartbeat 광고,
pairing 배포, Floorplan별 generation 자동 routing과 공개 directory API는 아직 남아 있습니다.

- 평면도 문서 schema version 2에 무작위 영속 `floorplanId`, 독립적인
  `geometryVersion`, Node별 레이더 `poseVersion`이 추가되었습니다.
- legacy 문서는 다음 전체 저장에서 migration되며, 로컬 geometry와 pose 편집은 각각의
  revision만 증가시킵니다.
- 플랫폼 도입 전 펌웨어에서 바로 업데이트해도 로컬 레이더 위치와 보정 sidecar는 유지합니다.
  로컬 설정에서 축척만 빠진 경우 펌웨어가 저장된 canonical 평면도에서 X/Y 축척만 복구한 뒤
  원본 기기의 canonical spatial profile을 갱신합니다. 이 migration은 재부팅 후에도 결정적이며
  평면도나 로컬 레이더 위치를 다시 쓰지 않습니다.
- 로컬 문서를 대시보드에서 비교해 양쪽을 활성화하던 임시 경로는 제거했습니다.
- 기존의 평평한 구성원 최대 20개, 설정 원본 소유자, roster revision, 향후 평면도
  manifest 헤더를 담는 고정 크기 Site configuration 저장소가 2-slot commit 방식으로 추가되었습니다.
- 단독 Node는 자신이 소유한 1대 roster로 시작하고, 기존 펌웨어에서 올라온 enrolled
  Node는 보이는 peer를 소유자로 추측하지 않고 미확정 상태로 닫힙니다.
- owner와 roster가 모두 미확정인 Node는 명시적인 사용자 동작으로 로컬 설정 소유권을 한 번만
  확정할 수 있습니다. 새 pairing을 Leader 역할로 확정할 때도 같은 제한 경로를 사용해 owner와
  canonical roster가 서로 비대칭이 되지 않게 합니다.
- 로컬 소유권 확정, 확인된 역할 교체, roster 갱신 및 마지막 연결 제거 후 standalone 복구는
  하나의 완성된 후보 roster를 만든 뒤 최종 snapshot만 검증합니다. owner만 먼저 바뀐 잘못된
  중간 상태는 저장하거나 검증하지 않습니다.
- 설정 원본 소유자는 `/api/platform/site-configuration`에서 현재 로컬의 revoked되지 않은
  participant 목록을 canonical roster로 확정할 수 있습니다.
- 설정 원본 소유자는 CONFIG/IMAGE를 다시 업로드하지 않고 현재 hash로 authoritative
  floorplan manifest를 확정할 수 있습니다.
- 연결된 Node는 owner에서 직접 인증된 최대 384 byte UDP chunk로 canonical roster와
  평면도를 복제합니다.
- 공유 평면도 저장이 성공하면 authoritative manifest를 자동 발행합니다. Node별 레이더 pose와
  zone/tuning 상태는 로컬에 남아 Site generation을 발행하지 않습니다.
- follower는 canonical roster 순서에 따른 2.5초 간격 slot과 제한된 전체 세션 backoff로
  새 generation을 가져오므로 19대가 owner에 동시에 요청하지 않습니다.
- 모든 기기는 확정된 roster에서 현재 설정 원본을 제외한 active·Leader 허용 NAMO 기기 중
  Node ID가 사전순으로 가장 빠른 한 대를 동일한 자동 후임으로 계산합니다. 후임은 현재 Site
  Leader여야 하고, 평면도 replica와 모든 active 구성원의 활성 인증 정보를 보유한 상태로
  30초 동안 조건을 유지해야 한 번의 원자적 owner 변경을 저장합니다.
- heartbeat v5는 authority epoch, generation과 설정 digest로 Site 상태를 fencing합니다.
  순차 업데이트 중에는 상대가 v5를 증명할 때까지 v4 생존 패킷도 주기적으로 유지합니다.
  online peer의 fence가 다르거나 없으면 선출, 공유 쓰기, directory reconciliation과
  fusion에 참여시키지 않습니다.
- 고정 크기 Site authority snapshot 하나가 로컬 상태를 준비 전, 단독, 일치, 수렴 중,
  충돌로 판정합니다. coordinator, 공유 쓰기, directory, 자동 승계, fusion과 API guard는
  heartbeat endpoint를 각자 다시 검사하지 않고 이 단일 판정만 사용합니다.
- 자동 Site 설정 복제의 대기, 진행, 차단 및 제한 재시도 상태는 PSRAM platform workspace의
  고정 크기 Site authority tracker 하나가 관리합니다. 기존 인증 전송과 transaction 저장
  경로는 그대로 유지합니다.
- `site_configuration_conflict_repair.py`는 legacy equal-generation 분기의 양쪽을 백업하고,
  사용자가 명시한 원본의 authority epoch만 올린 뒤 정확한 수렴을 확인합니다.
- owner 변경은 해당 결정적 후임에게 직접 인증된 자동 pull에서만 받아들입니다. 수동 sync나
  임의의 peer는 owner를 바꿀 수 없으며, 후임에서 가져올 때도 기존 2.5초 roster slot을
  사용해 요청을 분산합니다.
- 자동 후임이 있는 경우 현재 설정 원본은 그 후임이 온라인이며 같은 authority epoch,
  generation, 설정 digest와 평면도 manifest를 확인한 동안에만 공유 설정 쓰기를 허용합니다.
  네트워크가 갈려도 감지와 조회는
  계속되지만 공유 쓰기는 잠시 멈춰 이전 원본과 새 원본이 동시에 canonical 상태를
  진행하지 못하게 합니다. 고장 난 연결을 명시적으로 해제하는 복구 동작은 계속 허용합니다.
- CONFIG와 IMAGE는 비활성 flash slot에 기록한 뒤 한 번의 pair commit으로 함께 전환합니다.
  전송이 중단되어도 기존 평면도가 유지되며 전체 이미지를 RAM에 올리지 않습니다.
- heartbeat version 5는 authority epoch, 복제 generation, 설정 digest, 평면도 digest와
  로컬 레이더 pose를 보고합니다. 순차 업데이트를 위해 version 1부터 4까지 읽을 수 있지만,
  fence가 없는 구형 peer는 현재 admission 조건을 모두 충족하지는 못합니다.
- 같은 fenced revision의 내용 불일치와 오래된 authority epoch·generation은 닫힌 상태로
  거부합니다.
  NAMO Node는 인증된 online digest가 canonical manifest와 정확히 일치하기 전에는 active로
  전환할 수 없습니다.
- 공유 geometry를 복제해도 Node별 로컬 레이더 pose sidecar는 유지합니다.
- 호스트 테스트는 replication codec, 결정적 follower slot, 제한된 backoff, 오래되거나 같은
  generation의 충돌, digest admission, 제한된 전송 상태와 transaction replica 저장을 검증합니다.
- owner에서 follower로 전체 평면도를 받은 뒤 재부팅해 유지되고 대시보드에 반영되는
  2대 실기기 smoke는 완료했습니다.
- 기기 연결 화면은 한쪽에서 진행하는 단계형 흐름으로 기기 선택, 상대 기기의 명시적 연결
  허용 3분 대기, 코드 확인, 공유 평면도 선택 및 인증 복제 시작을 안내합니다.
- 두 기기의 평면도가 다르면 사용자가 하나를 직접 선택하며, 각 기기의 레이더 pose와 로컬
  구역은 유지합니다. 기존 연결을 고립시키는 이동은 차단하고, Site·owner·Node 같은 내부
  용어 대신 상대 기기로 바로 돌아가는 링크를 제공합니다.
- 연결 완료는 양쪽이 같은 Site, configuration owner, floorplan identity, geometry revision,
  CONFIG/IMAGE hash를 보고할 때만 표시합니다. `returnTo` query는 pairing session이 진행
  중이거나 해당 host가 초대 주소 또는 인증된 participant 주소와 일치할 때만 표시합니다.
- 공유 평면도를 제공하는 기기에서 삭제하면 빈 manifest를 발행하며, follower는 해당
  generation을 commit할 때 복제된 CONFIG와 IMAGE도 삭제합니다.
- 설정 원본 기기의 대시보드는 두 Node가 동시에 본 한 사람을 캡처합니다. 첫 스냅샷으로 제한된
  위치 보정안을 만들고, 충분히 떨어진 두 번째 스냅샷이 있으면 각도도 보정하며 잔여 오차를
  보여줍니다. 사용자가 승인한 뒤에만 양쪽 readiness를 설정하고 양쪽 participant를 활성화합니다.
- 승인된 유효 변환은 버전이 있는 readiness 데이터이며 각 Node의 로컬 평면도 레이더 위치를
  덮어쓰지 않습니다. 일부 활성화가 실패하면 양쪽을 다시 quarantine합니다.
- transform 메타데이터는 기존 readiness 계약을 대체하지 않고 확장합니다. 기존 참여자의
  현재 형식 기록은 transform이 없어도 읽을 수 있습니다. 다만 readiness version 1과 2는
  geometry와 양쪽 pose 기준이 없으므로 재보정 전까지 닫힌 상태로 처리합니다. 승인된
  transform은 일반 참여자 활성화가 아니라 이후 spatial fusion admission 단계에서 요구합니다.
- observation의 pose 또는 calibration version이 승인된 readiness와 다르면 spatial
  admission에서 거부합니다. active spatial 관계는 승인 당시의 geometry와 양쪽 radar
  pose revision을 저장합니다. 공유 geometry 또는 어느 한쪽의 radar pose가 바뀌면
  loop당 최대 한 관계의 calibration을 지우고 quarantine하며, 보안 연결은 유지합니다.
  승인 보정 재설정도 기존 fail-closed quarantine 경로를 사용합니다.
- 자동 승계 코어는 기존 구성원 전체를 유지하며, 후임이 필요한 pairwise credential을 하나라도
  보유하지 않으면 승계를 차단합니다. 따라서 2대 Site의 경로는 완성되었고, 더 큰 Site에서는
  무인 인증 관계 준비와 실기기 검증이 남아 있습니다. 현재 구현은 구성원을 조용히 누락시키는
  대신 닫힌 상태로 멈춥니다.
- 호스트 테스트는 사용자 확인을 거친 교차 owner 역할 교체, authoritative manifest 수렴,
  대상 주소를 확인하는 돌아가기 링크 정리, 변환 축, 저장·복원, solver 제한, 양방향 활성화 rollback 및
  수동 Leader handover smoke 도구를 검증합니다. 두 Node 보정·활성화와 Leader handover
  실기기 smoke는 통과했습니다. 자동 두 Node 설정 원본 승계와 정상 재합류도 실기기 smoke를
  통과했습니다. 기존 4대 Site에 다섯 번째 기기를 연결·보정·활성화하고 이후 역할 장애와 재합류까지
  확인했습니다. 단독 peer 역할 교체의 정식 실기기 smoke, 비슷한 시야각과 서로 다른 시야각 배치
  검증 및 6·7대 Site의 인증 관계 검증은 남아 있습니다.

5E의 목표 다중 Floorplan 확장:

- 모든 활성 Floorplan에는 영속 Configuration Owner가 정확히 한 대 있으며, 물리 NAMO Node
  한 대는 활성 Floorplan 하나만 소유합니다.
- Floorplan마다 원본 generation, payload digest, 배정, 쓰기 gate 및 자동 승계 상태를
  독립적으로 관리합니다.
- NAMO Node는 자신에게 배정된 Floorplan만 해당 Configuration Owner에서 직접 받아 자신의
  CONFIG/IMAGE 비활성 flash slot에 commit합니다.
- Configuration Owner는 활성 복제 session을 한 번에 하나만 처리합니다. 나머지 대상 Node는
  결정적인 제한 slot에서 기다리고 제한된 전체 session backoff로 재시도합니다.
- Site Leader는 크기가 제한된 Floorplan 원본/digest/경로/health와 Group 요약
  메타데이터만 저장하며 역할상 Floorplan payload를 저장하거나 중계하지 않습니다.
- Site Leader와 Floorplan Configuration Owner를 겸임하는 물리 Node도 payload는
  Configuration Owner 역할로만 제공합니다.
- 현재 단수 owner/manifest schema에서의 migration은 transaction으로 수행하고 실패 시
  기존 Floorplan, 로컬 레이더 pose, zone, calibration 및 마지막 committed replica를 보존합니다.

산출물:

- 버전이 있는 local-to-Floorplan transform;
- 공유 Site Floorplan 및 Space geometry;
- overlap snapshot capture와 residual report;
- 사용자 확인이 필요한 제한된 이동 및 회전 보정;
- pose, geometry 또는 calibration 변경 후 명시적 invalidation
  (`implemented`, 실기기 smoke 대기).

테스트 및 완료 조건:

- transform round trip과 axis fixture;
- 비슷한 각도 및 서로 다른 각도 배치;
- 큰 residual은 물리 설치 또는 UI 배치 수정을 요청합니다.
- 미보정 또는 오래된 버전 observation이 spatial fusion에 참여하지 않습니다.

양쪽 Node가 `활성`으로 표시된 뒤 다음과 같이 실기기 검증을 실행합니다.

```text
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B --handover
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B --configuration-owner-handover --timeout-seconds 90
python tools/site_configuration_conflict_repair.py --source http://SELECTED_SOURCE --peer http://OTHER_DEVICE
python tools/site_configuration_conflict_repair.py --source http://SELECTED_SOURCE --peer http://OTHER_DEVICE --confirm --timeout-seconds 90
```

handover 모드는 기기를 API로 재부팅하지 않습니다. 사용자가 현재 Leader의 전원을 끌 때까지
기다린 뒤 follower의 lease 전환을 확인하고, 다시 전원을 켠 뒤 두 기기의 정상 합의를 확인합니다.
설정 원본 승계 모드도 사용자가 직접 전원을 제어합니다. 30초 안정화 시간을 기다린 뒤 새 설정
원본을 확인하고, 기존 기기가 다시 연결되면 양쪽의 owner, generation, roster revision과
평면도 manifest가 같아졌는지 검증합니다.
충돌 복구 명령은 `--confirm`이 없으면 실제 authority를 바꾸지 않습니다. 이 경우에도 양쪽
기기를 먼저 백업하고 선택한 원본 digest를 출력합니다.

### 6단계: 7개 관측원 Shadow Fusion

상태: 구현 완료입니다. 호스트 테스트를 통과한 고정 메모리 코어가 admission을 이미 통과한
world-coordinate 관측원 최대 7개와 각 관측원의 타깃 최대 3개를 입력으로 받습니다. 제한된
spatial association, local track ID와 독립적인 fused identity, 같은 각도 관측의 상관 오차
할인 및 다른 각도 관측의 weighted geometry를 구현했습니다. follower는 인증 observation을
선출된 Group Leader로 직접 보내고, Leader는 local observation 하나와 admission을 통과한
remote observation 최대 6개를 제한된 런타임으로 연결합니다. 관측원 용량은 `static_assert`로
`GroupMembershipView::MAX_PARTICIPANTS`와 묶여 있습니다. `GET /api/platform/groups`에서
읽기 전용 진단을 제공합니다. 표준 NAMO 제품 빌드는 저장된 credential로 runtime을
활성화하며, 개발 smoke는 별도의 비production 권한 adapter를 사용합니다.

산출물:

- 고정 크기 spatial gating과 제한된 association: 호스트 코어에 구현;
- local track ID와 독립적인 fused-track lifecycle: 호스트 코어에 구현;
- 비슷한 시야각의 상관 오차 정책: 호스트 코어에 구현;
- 서로 다른 시야각의 weighted geometry: 호스트 코어에 구현;
- 선출된 Group Leader에서만 동작하는 opt-in 수신·런타임 연결: 구현;
- 고정 UDP 소켓 공유, 500ms 송신 제한, 무재전송 및 fail-closed readiness 검사를 적용한
  인증 NAMO 송신기: 구현;
- 그 자체로 production 권한을 부여하지 않는 제한된 Shadow output과 cycle 진단: 구현;
- 인증된 Site 복제에 Node별 승인 transform을 넣고 결정적 설정 원본 승계 후에도
  그대로 유지하는 canonical spatial profile: 구현;
- production presence 및 target count와 active Shadow output을 PC에서 읽기 전용으로
  비교하고 선택적으로 스키마 버전이 있는 모든 sample을 JSONL로 저장하는 도구:
  구현. 버전 2는 기기에 history를 추가하지 않고 production 좌표, 크기가 제한된
  Shadow track, 주기별 association 진단과 과다 검출 episode를 보존합니다.

테스트 및 완료 조건:

- 한 사람의 중복 observation을 하나의 fused candidate로 만든다: 호스트 테스트 통과;
- 충분한 근거 없이 가까운 두 사람을 하나로 합치지 않는다: 호스트 테스트 통과;
- 교차, 일시 소실, 좌표 튐, 비대칭 sensor quality, local ID 교체 및 고정 용량 동작:
  호스트 테스트 통과;
- 현재 production 상태와 다른 모든 결과를 외부 비교 수집에서 드러낸다:
  호스트 테스트와 첫 두 Node 실기기 수집 통과. 첫 120초 수집은 active sample
  101개, presence 불일치율 1.98%, target count 불일치율 16.83%였습니다. 연속성과
  단일 관측원 admission을 강화한 뒤 반복한 101개 sample 수집에서는 target count
  불일치율이 8.91%, 과다 검출이 1.98%로 줄었고 최대 fusion cycle도
  31,463마이크로초에서 8,744마이크로초로 감소했습니다. 관찰한 재실 구간에는 지속되는
  중복 episode가 남지 않았습니다.

인증 transport, 두 Node Leader 합의, 설정 원본 승계, 정상 재합류, 한 사람 정지 및
입실·퇴실 수집은 계속 회귀 gate로 유지합니다. 실제 두 사람이 교차하는 수집은 광범위한
production 배포 전에 여전히 필요하지만, 고정 크기 Shadow 코어의 구현 완료를 막지는 않습니다.

실기기 비교 명령:

```text
python tools/shadow_fusion_compare.py --device http://GROUP_LEADER --duration-seconds 120 --output shadow-compare.jsonl
```

### 7단계: Production Fusion과 자동화 상태

상태: 구현 완료. 표준 NAMO 제품 firmware는 인증 production observation transport를
기본으로 포함합니다. 고정 크기 production
adapter는 confirmed/coasting track만 내보내고 world 좌표를 선출된 Group Leader의
로컬 radar 좌표로 되돌린 뒤 기존 target, 구역, state 및 Home Assistant count 경로에
연결합니다. Presence와 motion은 authoritative fused 근거가 있을 때만 보강합니다.
runtime, transport, readiness, 관측원 수 또는 transform에 문제가 생기면 `degraded`로
표시하고 기존 local tracker 경로를 유지합니다. 단독 기기와 준비되지 않은 Group의 기존
local tracker 동작도 그대로 유지합니다.

현재 구현 상태:

- 선출된 Group Leader가 제한된 confirmed/coasting production target을 발행하며 기존
  presence, motion, target count, Zone 및 Home Assistant entity 경로가 이 권위 있는
  출력을 사용합니다.
- follower는 인증된 observation을 계속 전송하면서 로컬 감지와 호환 entity를 유지합니다.
- `GET /api/state`는 별도 좌표 stream을 만들지 않고 작은 `fusion` 런타임 정보와 target별
  `fused`/`sourceCount` 근거를 제공합니다.
- 대시보드는 fusion 빌드 또는 spatial 설정이 없으면 관련 UI를 숨깁니다. 설정된 경우
  active/sending/degraded 상태, 공유 평면도의 active peer 레이더 배치 및 여러 센서가
  확인한 target을 `T1 · 융합` / `T1 · Fused`로 표시합니다.
- 선출된 Group Leader는 고정 크기 `Fusion Snapshot V1`을 Group의 ready 기기 각각에
  pairwise 인증으로 직접 보냅니다. 융합 계산을 맡지 않는 기기는 최신 snapshot 하나만
  보관하고 오래된 권한·sequence를 거부하며 canonical world target을 자신의 승인된
  transform으로 재투영해 로컬 `/api/state`에서 제공합니다. 대시보드는 더 이상 다른 기기의
  IP로 `/api/state`를 직접 요청하지 않으며 snapshot이 없거나 만료되면 로컬 표시를 유지합니다.
- 이미 결합된 관측에는 제한된 해제 여유를 적용하고, fused track 위치에는 source 유실에
  민감한 고정 메모리 예측 보간을 적용합니다. 대시보드는 500ms 표본 사이를 300ms 동안만
  보간해 좌표 점프를 줄이면서 입력 지연을 제한합니다.
- 호스트 테스트는 fail-closed adapter 권한, 제한된 출력, 좌표 변환 및 상태 직렬화를
  검증합니다. 두 Node 실기기 smoke에서는 인증 송수신·admission과 권위 있는 fused track
  출력을 확인했습니다. `site_leader_handover_smoke.py --fusion-snapshot`은 융합 계산을 맡지
  않는 기기의 자체 `/api/state`가 authoritative로 바뀌는지와 인증 snapshot 원본 및
  거부 카운터까지 확인합니다.

구현 완료 근거:

- `Fusion Snapshot V1`을 완전한 고정 크기 자동화 상태로 정규화했고 모든 Node의 target,
  Zone, state 및 ESPHome entity 경로가 같은 출력을 사용합니다.
- transport loss와 오래된 권한·sequence는 잘못된 unoccupied가 아니라 unavailable로
  처리하며, 융합을 꺼도 canonical 논리 ID를 유지합니다.
- production fusion 자원 기준은 8단계에 보관합니다. 혼합 업데이트, 재시작 및 역할 변경
  실기기 smoke는 미완성 protocol이 아니라 릴리즈 검증으로 남깁니다.

산출물:

- opt-in Fusion Group output;
- occupancy, motion, fused target count, Zone state, transition 및 health;
- complete Group snapshot 이후 sequence가 있는 update;
- 안정적인 Site, Space 및 Zone 논리 식별자;
- 진단과 호환성을 위한 현재 local state 유지.

테스트 및 완료 조건:

- 오래된 term 또는 sequence가 자동화 상태를 바꾸지 못합니다.
- transport loss를 unoccupied가 아니라 `degraded` 또는 `unavailable`로 처리합니다.
- 참여자 1개에서 2개로 확장해도 Space ID를 유지합니다.
- migration 손실 없이 Fusion을 끌 수 있습니다.

### 8단계: 계층형 용량 및 자원 검증

현재 기반:

- 인증된 고정 크기 Fusion Snapshot 배포는 기존 UDP transport와 기기 간
  credential을 재사용합니다. 수신 기기는 PSRAM 우선 고정 cache에 최신 snapshot
  하나만 보관하고 자신의 `/api/state`로 제공하므로 브라우저가 계산 담당 기기에
  직접 접근하거나 크기 제한이 없는 queue를 둘 필요가 없습니다.
- 내부 RAM 회수의 첫 단계로 크기가 제한된 `ParticipantReadinessStore`
  workspace를 PSRAM으로 옮겼습니다. 작은 storage backend와 256바이트 flash I/O
  staging buffer는 내부 RAM에 유지합니다. PSRAM 할당 실패 시 내부 RAM을 조용히
  소비하지 않고 원격 readiness를 비활성화합니다. 첫 실기기 resource smoke에서는
  지속적인 heap 감소나 최대 연속 block 실패는 없었지만 standalone과 두 Node
  build 모두 HTTP task가 2 KiB stack gate 아래로 내려갔습니다. 고정 endpoint 귀속으로
  `/api/system/status`와 `/api/stats` handler가 최저 여유 경로임을 확인했습니다. system
  status는 PSRAM 우선 고정 workspace 하나로 JSON을 chunk 전송하고, 변하지 않는
  image metadata는 HTTP 등록 전에 cache합니다. 통계는 heatmap cell과 서로 분리된
  응답·저장 workspace를 PSRAM 우선 고정 수명 메모리에 두고, 네 개 기간 합계를
  daily history 한 번의 순회로 계산하며, 현재 heatmap RLE도 임시 문자열 없이
  바로 전송합니다. 펌웨어는 ESP-IDF에서 전달받은 4,352바이트 HTTP task stack을
  내부 RAM의 제한된 6,144바이트로 올립니다. 모든 로컬 HTTP route family는 고정
  code로 분류하며 요청 진입과 handler 완료 시점을 각각 측정합니다. 이 변경 전
  0.5.5-h 실기기 smoke에서는 HTTP task 여유가 1.2~1.3 KiB로 2 KiB gate를
  통과하지 못했습니다. 0.5.5-j는 standalone 한 대와 production fusion 중인 두 Node에서
  전체 gate를 통과했습니다. 최저 내부 heap은 각각 74.8, 53.5, 61.0 KiB, 최대 연속
  block은 모두 62 KiB, HTTP task 여유는 3.1, 3.1, 3.0 KiB였고 진행성 내부 heap
  감소는 8.4, 3.9, 7.1 KiB 이하였습니다. 세 기기 모두 재실행에서도 gate를 통과했고,
  일시적인 HTTP 연결 종료는 같은 기기의 즉시 재실행에서 재현되지 않았습니다.
  이 결과는 현재 standalone/두 Node 기준선이며
  4·6대 처리 용량을 증명하지는 않습니다. 제한된 Site directory를 추가한 0.5.5-l에서도
  standalone 기기는 30회·동시성 4 조건을 세 번 연속 통과했고 내부 heap 최저
  72.4 KiB, HTTP stack 여유 3.1 KiB를 유지했습니다. 융합 중인 두 Node도 각각
  52.4/59.8 KiB와 3.0/3.1 KiB로 통과했으며 연결이 유지됐습니다.
- 다음 내부 RAM 회수 단계로 원격 participant·readiness·Site configuration
  canonical snapshot, 현재 평평한 Site/Group coordinator 배열, Site configuration
  갱신 scratch, fusion snapshot 및 remote observation scratch, participant
  registry와 Site configuration 저장 workspace, heartbeat·replication·observation
  transport 본체와 고정 packet buffer, observation parser, inbox, Fusion Snapshot
  cache 및 Shadow Fusion runtime을 PSRAM 우선 고정 수명 객체로 옮겼습니다. 각 할당은
  구성요소, 메모리 영역, 크기, 내부·PSRAM 여유와 최대 연속 block을 부팅 로그에
  남깁니다. PSRAM 실패 후 내부 fallback과 완전한 할당 실패,
  transport dependency·socket·replication 실패도 단계별 로그로 구분합니다.
  0.5.5-j standalone 부팅에서는 platform runtime 19,040바이트, participant
  registry store 2,664바이트, Site configuration store 4,192바이트,
  readiness store 8,356바이트와 heartbeat·replication·observation·fusion
  workspace가 모두 PSRAM에 할당됨을 확인했습니다.
  FreeRTOS/HTTP task stack, lwIP socket 객체, UART/DMA 경로와 보안 credential
  snapshot·registry·저장 workspace는 내부 RAM에 유지합니다. 이후 통계와 Site
  configuration translation unit에서 생성된 feature define include가 빠져, 의도한
  PSRAM 분기 대신 내부 fallback이 컴파일된 사실을 확인했습니다. 0.5.5-i는 이 상태에서도
  통과했지만 두 translation unit은 이제 생성 define을 직접 include합니다. 다음 실기기
  부팅 로그에서 두 할당이 PSRAM으로 표시되어야 RAM 회수를 검증 완료로 봅니다.
- Site replication의 packet·제어 객체는 2 KiB 아래로 제한하고, 4 KiB configuration
  blob 두 개와 decode snapshot은 하나의 PSRAM 전용 workspace로 분리했습니다. PSRAM이
  없으면 내부 RAM으로 fallback하지 않고 단계가 구분된 로그와 함께 원격 복제만
  비활성화합니다. 로컬 감지와 현재 활성 Floorplan은 유지합니다.
- 고정 메모리 fusion 코어와 runtime은 로컬 한 대와 인증된 원격 NAMO 관측 최대 6개를
  처리합니다. 입력 추출, 원격 observation 6개, participant·readiness 선택 scratch,
  runtime input 및 7-source fusion scratch는 크기가 제한되어 있으며 PSRAM 상주
  workspace에 둡니다. source 순서는 결정적이고 중복 participant token은 거부하며,
  제한된 incremental candidate만 association에 사용합니다. 출력은 계속 track 최대
  6개로 제한합니다. 네이티브 회귀는 1·2·4·6·7개 입력, 초과 독립 관측의 결정적 drop,
  여덟 번째 입력의 배열 접근 전 fail-closed 거부를 검증했습니다. 물리 4·6·7대의 부하,
  처리 시간, packet loss 및 자원 검증은 남아 있습니다.
- 설정된 Group에서 온라인 Node가 한 대만 남아도 Group을 standalone으로 바꾸지 않습니다.
  결정적인 Leader 선출 뒤 해당 기기의 유효한 로컬 관측을 단일 source production 입력으로
  받아 automation을 계속 제공하고, Site health는 빠진 설정 구성원을 `degraded`로
  표시합니다. 네이티브 회귀는 로컬 단독 시작, 원격 손실 중 track identity 유지와 원격
  재합류를 검증합니다.
- Site Leader가 가능한 경우 설정 원본을 피하고, 한 Fusion Group의 NAMO Node 후보 최대
  현재 평평한 후보 집합에서 Group Leader가 두 역할을 모두 피하는 결정적 배치 규칙을
  고정 메모리 호스트 테스트로 검증했습니다.
- 일반 후보가 없으면 설정 원본, Site Leader 순으로 겸임을 허용해 standalone과 장애
  fallback을 유지합니다.
- Site configuration storage version 3은 각 NAMO Node의 canonical spatial
  profile에 `spaceId`와 `groupId`를 함께 저장합니다. coordinator, observation
  sender, fusion input 및 Fusion Snapshot 배포는 Site 전체 Group 수가 아니라
  로컬 기기의 canonical Group만 선택합니다. version 1·2 record는 계속 읽으며
  모든 유효 readiness 배정이 동일한 단일 Group을 가리킬 때만 다음 owner
  reconciliation에서 결정적으로 승격합니다. 여러 Group 중 로컬 배정이 불명확하면
  calibration revision 순서로 추측하지 않고 fail-closed로 남습니다. 현재 4 KiB 평평한
  저장 경계, 구형 record 복구와 다중 Group 분리를 네이티브 테스트로 검증했습니다.
- 각 Node는 예약된 platform sector 3개에 Group-local A/B roster 하나를 저장합니다.
  PSRAM의 고정 크기 runtime snapshot에는 로컬 Node와 peer 6대까지만 들어가며,
  coordinator, observation 송수신 admission, Fusion Snapshot 배포와 fusion 입력 추출이
  모두 같은 roster를 사용합니다. 직접 업데이트 시 구형의 평평한 Site configuration에서
  로컬 Group 하나만 최초 1회 이관합니다. 여덟 번째 감지 기기는 commit 전에 거부하고,
  구형 초과 roster는 모든 기기가 같은 7대를 선택하도록 결정적으로 제한합니다. 제외된
  Node는 서로 다른 로컬 view를 만들지 않고 fail-closed로 남습니다.
- 네이티브 matrix는 7대 Group 경계, roster commit 중단을 포함한 Group Leader handoff,
  독립적인 7대 Group 8개를 검증합니다. Site directory에는 계속 저빈도 Group 요약 8개만
  남습니다. 이는 저장 및 runtime 경계를 검증한 것이며, 실제 8개 Group pairing과 resource
  smoke는 8단계 release gate로 남아 있습니다.
- 현재 Group Leader가 2초마다 한 번 보내는 공간 단위 집계 상태를 Site Leader의 RAM 전용
  shadow cache에 최대 8개까지 보관합니다. 이 요약에는 재실, 움직임, 타깃 수, Group term과
  순서 정보만 들어가며 좌표, 원시 관측, 구역 상태, 이력은 들어가지 않습니다. 현재는 자동화
  권한으로 사용하지 않습니다. 이전 Leader, 오래된 term, 다른 Space, 역순 packet과 아홉 번째
  Space는 fail-closed로 거부합니다.
- 기존 Site directory에 저빈도 Group Leader 요약을 최대 8개 보관하는 RAM 전용 cache를
  연결했습니다. directory에 지정된 Group Leader가 Group ID, 배정 revision, term 및 구성원
  수를 정확히 맞춘 경우에만 보고를 받습니다. Site Leader는 Floorplan·Leader 경로,
  online/expected 수, health 및 제한된 lease만 저장하고 타깃 데이터는 저장하거나
  중계하지 않습니다. Site Leader가 바뀌면 cache를 즉시 비우며 flash에는 기록하지 않습니다.
  heartbeat v5가 선택적인 인증 Group 요약을 현재 Site Leader에만 전달하고, 주기적인 v4
  호환 패킷이 혼합 펌웨어의 liveness를 유지합니다. 네이티브 테스트는
  Group 1·8개 수용, 아홉 번째 Group 거부, 오래된 권한, 만료 및 Leader 전환 시 삭제를
  검증합니다. 다중 Group Site 실기기 smoke는 남아 있습니다.
- canonical Site directory 전달은 새 프로토콜이나 사용자 설정 화면을 만들지 않고 기존
  인증 UDP 복제와 384바이트 조각 전송을 재사용합니다. Site Leader가 아닌 active·Leader
  가능 후임 후보는 현재 Site Leader를 30초 저빈도로 확인하며 최초 요청은 결정적으로
  분산하고 실패 시 제한된 backoff를 사용합니다.
  수신 기기는 topology revision과 의미 해시를 비교한 뒤 기존 A/B directory 저장소에
  transaction으로 반영하며, 기기마다 다른 로컬 저장 generation은 분산 권한으로 사용하지
  않습니다. 평면도 CONFIG와 IMAGE는 계속 각 Floorplan의 Configuration Owner에서 직접
  받고 Site Leader는 중계하지 않습니다. codec, 인증, 오래된 revision, 같은 revision의
  불일치, compatibility mirror 승격 및 저장 회귀 네이티브 테스트를 통과했습니다.
  혼합 펌웨어와 다중 Group 실기기 자동 수렴 smoke는 남아 있습니다.
- 각 Configuration Owner는 고정 크기 2.5초 follower lease 하나로 평면도 CONFIG/IMAGE
  제공을 직렬화하며 RAM 대기열을 만들지 않습니다. 동시에 요청한 인증 follower는 구형
  펌웨어도 해석할 수 있는 busy 응답을 받고 기존 전체 session slot과 제한된 backoff로
  다시 시도합니다. lease는 현재 follower 요청마다 갱신되고 완료·종단 응답에서 해제되며
  중단되면 만료됩니다. 네이티브 테스트는 동시 경쟁 follower 6대, 소유권, 완료 및 만료를
  검증했으며 다중 follower 실기기 전송 smoke는 남아 있습니다.
- heartbeat v7은 확정 Site Directory revision과 digest를 광고합니다. Site 선출은 최신
  fence를 가진 Directory를 우선하며, 후보가 여러 개인 부팅에서는 peer를 offline으로
  판단하기 전 제한된 lease 한 번을 기다립니다. 같은 revision의 분기 상태는 결정적으로
  선출된 권위 원본으로 복구합니다. 초기 등록에서는 로컬 compatibility mirror를 역할 조정
  입력에서 제외하고, 보통 역할 권위를 얻기 전에 이미 인증된 Configuration Owner의 사본으로
  채웁니다. 같은 Site의 오래된 canonical 사본은 활성화를 막지 않으며 일반 선출 및 복제 fence가
  수렴 전까지 해당 Node를 권위 없는 상태로 유지합니다. active 구성원이 2대를 넘고 Owner가
  Site Leader가 아니라면 live quorum과 선출된 Site
  Leader의 정확히 같은 Directory revision/digest 광고가 있어야 제공할 수 있습니다. active
  1~2대의 초기화 복구 fallback은 Owner 또는 유일한 canonical peer로 제한합니다. canonical
  reconciliation은 제거된 Group과 Floorplan을
  영구 unavailable route로 남기지 않고 정리합니다. 영속 Directory를 포함한 네이티브 장애
  행렬은 오래된 상태로 재부팅과 같은 revision의 네트워크 분기 수렴을 검증하며, 집중
  네이티브 테스트는 삭제 정리와 v6/v7 혼합 heartbeat 호환을 검증합니다.

목표 계층:

- Site Leader 한 대는 저빈도 Group directory 및 health record를 최대 8개 관리합니다.
- 모든 활성 Floorplan에는 독립적인 Configuration Owner 한 대가 있으며, 물리 NAMO Node
  한 대는 활성 Floorplan 하나만 소유합니다.
- 각 Configuration Owner는 follower 복제를 한 session씩 처리하고 Site Leader는
  메타데이터만 보관합니다.
- Group Leader 한 대는 로컬 입력 하나와 원격 센싱 입력 최대 6개를 받아 융합합니다.
- 물리 ESP32-S3 한 대는 Fusion Group 하나의 Group Leader만 맡습니다.
- Site Leader는 raw observation이나 fused target stream을 받거나 Group 상태를 중계하지 않습니다.
- Site Leader의 제한된 siteGroups directory는 committed Group 식별자를 유지하고,
  현재 Group 보고가 일치할 때만 자동화 상태를 직접 읽을 수 있는 임시 경로를 제공합니다.
  로컬 Group Leader는 현재 Site Leader 주소를 재사용하고, 원격 Group Leader 주소는 인증
  heartbeat에서만 가져오며 flash에 저장하지 않습니다. Group과 canonical Space가 일치해야
  하며 보고나 경로가 만료되면 Group을 삭제하거나 미감지로 바꾸지 않고 사용할 수 없는
  상태로 처리합니다.
- Home Assistant와 SmartThings는 Site Leader에서 Group 경로를 확인한 뒤 각 논리 Space 상태를
  현재 Group Leader에서 직접 읽습니다.
- 설정 원본과 Site Leader, 설정 원본과 Group Leader의 겸임은 허용합니다. Site Leader와
  Group Leader의 겸임은 장애 fallback에서만 허용합니다. 해당 Group에 다른 적격 후보가
  없을 때에만 세 역할을 한 기기에 긴급 배정하며, 적격 후보가 생기면 역할을 넘깁니다.

산출물:

- 물리 NAMO Node 및 simulated Satellite를 사용하는 load harness;
- network, heap, CPU, queue overflow 및 handover 고정 metric;
- 고정 크기 Site Group-summary directory 및 Group-local participant registry;
- Floorplan별 generation·승계 상태를 포함하는 고정 크기 원본/digest/경로 directory;
- 결정적인 대기 slot과 backoff를 사용하는 Configuration Owner의 제한된 단일 session scheduler;
- Site 구성에 포함되는 canonical Node-to-Group 배정: 구현;
- 비싼 matching 이전 association candidate 축소;
- fused-track 및 cycle당 processing limit 문서화.

테스트 및 완료 조건:

- Fusion Group 하나에서 활성 센싱 입력 1·2·4·6·7개;
- Site 하나에서 활성 Group Leader 요약 1·6·8개;
- Group이 하나씩 있는 Floorplan 8개와 독립 Configuration Owner 8대를 가진 Site에서
  Site Leader 상태에 payload를 보관하지 않음;
- 동시에 들어온 replica 요청을 owner 측 단일 session으로 순차 처리하며 기존 로컬
  replica를 잃거나 크기 제한이 없는 queue를 만들지 않음;
- Group의 여덟 번째 센싱 participant는 UI에서 명시적으로 거절하고 firmware에서는 기존
  7개 구성원에 영향을 주지 않은 채 fail-closed로 무시;
- 아홉 번째 Group은 Site 상태를 늘리지 않고 거절;
- 모든 Node가 같은 canonical Group 배정과 역할 충돌 회피 결과에 수렴;
- seed를 기록한 1~7대 장애 시퀀스로 수렴 중 후보 장애, 같은 revision 충돌, stale 재시작,
  이전 Owner 반복 재합류, 일시적 partition 및 지연 packet을 재현;
- Group Leader와 Site Leader 전원 손실, DHCP 변경, packet loss 및 정상 재합류 후에도
  논리 Space identity 유지;
- 삼중 역할 장애 fallback이 다른 적격 Group 후보가 없을 때만 사용되고 후보 등장 시
  해제되며 같은 자원 gate 통과;
- peak resource 사용량이 승인된 safety margin 안에 있음;
- overload와 packet loss에서도 local tracking이 응답성을 유지.

이 단계를 통과해야 문서에서 검증된 7개 입력 Group 또는 8개 Group Site 지원을 선언할 수 있습니다.
이전 릴리즈에서는 설계 목표라는 표현만 사용합니다.

### 9단계: 대시보드 Workflow

상태: 구현 중입니다. 전용 연결 workflow, 안전한 community pairing, 양방향 제거, 공유 평면도
선택·복제, 보정·활성화 마법사, 대체 Leader 제어, 여러 레이더 배치, production fusion 상태 및
융합 target 표시를 구현했습니다. 사용자는 격자에 맞춘 metric canvas에서 집 전체의 다각형 외곽선,
방, 물리 벽, 여닫이문·미닫이문 및 레이더 위치를 직접 그릴 수 있습니다. 저장 화면과 편집 화면은
같은 물리 벽과 차폐 geometry를 사용합니다. 다중 센서 설정이 없으면 standalone 대시보드는 그대로
유지합니다.

`scripts/capture-pairing.ps1`과 `tools/pairing_trace_capture.py`는 브라우저 단계, API 작업,
participant 상태, Site 권한 및 최종 수렴을 함께 기록합니다. 따라서 최종 UI 오류 문구만 보고
원인을 추정하지 않고 지연되거나 실패한 pairing 흐름을 확인할 수 있습니다.

남은 대시보드 범위는 제한된 local observation과 fused 결과 비교, residual·health 진단,
공식 기기와 Satellite trust workflow 및 onboarding·restore·role-change 브라우저 회귀
검증입니다.

산출물:

- Site 생성 또는 가입;
- NAMO Node 및 Satellite 승인, 배정, quarantine, calibration, 비활성화 및 제거;
- protocol 호환성을 공식 제조로 오해하지 않도록 공식 인증, 커뮤니티 승인 및 Satellite trust 상태 표시;
- Community NAMO Node에 Leader 자격을 줄 때 별도 확인 요구;
- 하나의 Floorplan에 여러 sensor 배치;
- local observation, fused target, residual 및 health 비교;
- standalone 사용자에게 현재의 간결한 workflow 유지.

테스트 및 완료 조건:

- onboarding, refresh, unsaved-change, restore 및 role-change browser test;
- 연결 commit, 공용 Floorplan 복제, 보정, 양방향 활성화, 역할 장애 및 정상 재합류를 하나의
  browser-to-firmware 수직 workflow로 검증하며, 일시적인 poll 실패 뒤 접수된 mutation을
  다시 실행하지 않음;
- 연결 허용 안내와 암호학적 등록 session이 사용자에게 하나의 일관된 3분 countdown을 표시;
- mobile과 desktop layout 검증;
- 설정 화면에서 version 없는 Site 변경을 쓰지 않습니다.

### 10단계: SmartThings Edge

산출물:

- 기존 물리 장치 동작 유지;
- 안정적인 Site LAN device 하나 생성;
- 논리 ID 기반 Space 및 Zone child 생성;
- 가능한 경우 표준 occupancy와 motion capability 사용;
- 현재 Site Leader를 발견하고 크기가 제한된 Group directory 확인;
- 모든 참여자가 아니라 현재 Group Leader에서 직접 상태 확인;
- Site Leader를 observation 또는 fusion 상태 중계기로 사용하지 않음;
- SmartThings device를 다시 만들지 않고 endpoint 전환.

테스트 및 완료 조건:

- Leader 및 IP 변경 후에도 child key를 유지합니다.
- offline이 unoccupied를 발생시키지 않습니다.
- driver와 Hub 재시작 후 child를 보존합니다.
- 현재 단일 장치 사용자가 상태를 계속 받습니다.

### 11단계: Home Assistant Integration

구현 안내: [Home Assistant 연동](../home-assistant-integration.ko.md)

상태: 구현 완료, 실환경 검증 대기. `custom_components/namo`는 펌웨어에 중계 계층을 추가하지 않고 기존의
제한된 공간 경로 목록과 완성된 자동화 상태를 사용합니다. 자동 발견과 수동 등록은 같은
설치 단위로 중복을 막고, 공간 엔티티 ID는 주소나 담당 기기가 바뀌어도 유지합니다.
평면도가 없는 단독 기기도 명시적인 빈 평면도 상태로 정상 처리합니다.
기기 로컬 구역은 담당 기기 변경 후 같은 의미를 보장할 수 없어 Group 엔티티로 노출하지
않습니다. 대신 평면도 설정 원본에서 관리하고 기존 CONFIG 복제와 자동 승계를 그대로 따르는
공용 자동화 구역 계약을 추가했습니다. 공용 구역 ID는 Site, Floorplan, 구역 ID로 고정되어
설정 원본이나 Group 담당 기기가 바뀌어도 Home Assistant 엔티티가 유지됩니다. Entry
migration은 공간 엔티티를 유지하면서 개발 미리보기의 기존 로컬 구역 엔티티와 불필요한
Site 상태 기기를 제거합니다.
집중 호스트 계약·엔티티 테스트 57개가 통과합니다. 경로 상한, 평면도 유무와 무관한
단독 기기의 로컬 공간 처리, 한 Site의 여러 공간 등록, 안정적인 엔티티 ID, 오래되거나
잘못된 권한, 순서 역행, 동적 공간 추가, 기존 구역 정리, 연결 끊김 시 사용 불가 원인,
지연 Repairs와 복구 및 버전 3 registry migration을 고정했습니다. 이 범위로 11단계의 코드와
집중 테스트 구현은 완료입니다. 공간 진입·퇴실 이벤트는 정식 전환 권한 계약이 생길 때까지
의도적으로 보류하며 이번 완료 범위에 포함하지 않습니다.

각 Group 기기는 상태, 감지 방식, 융합 상태, 작동/설정 기기 수와 데이터 지연을 별도
진단 엔티티로 제공합니다. Site 전용 부모 기기와 전체 상태 엔티티는 만들지 않습니다.
짧은 재부팅에는 알림을 만들지 않고 30초 이상 비정상 상태가 계속될 때만 Repairs
알림 하나를 만들며 정상화되면 자동으로 지웁니다.

산출물:

- 개발 원본인 `custom_components/namo`를 별도의 `NAMO-Home-Assistant`
  저장소로 export하는 배포 구조;
- HACS 기본 목록 등록을 기다리지 않고 전용 Integration 저장소를 사용하는 Custom
  Repository 설치;
- `siteId`당 config entry 하나;
- 단독 Site의 로컬 공간을 포함해 안정적인 `spaceId`당 Home Assistant 기기 하나;
- Zeroconf discovery 및 수동 fallback;
- 안정적인 Space occupancy, motion, target count 및 health entity;
- Group별 진단 entity와 Site 지속 문제에 대한 지연 Repairs 알림;
- 평면도 기반 공용 자동화 구역 계약과 Group별 구역 재실 엔티티;
- 현재 Site Leader에서 크기가 제한된 Group directory 확인;
- 각 Space 상태를 현재 Group Leader에서 직접 읽음;
- Site Leader를 상태 중계기로 사용하지 않고 Site 또는 Group Leader 변경에
  맞춰 내부 source routing.

테스트 및 완료 조건:

구현 및 호스트 테스트 완료:

- 안정적인 Site identity 기준 중복 방지와 Site별 config entry 하나;
- single-to-multi 확장과 동적 Space 추가 후에도 entity unique ID 유지;
- 오래된 권한과 순서가 역행한 snapshot 무시;
- timeout을 잘못된 상태 transition 없이 unavailable로 처리;
- Configuration Owner와 Leader가 바뀌어도 공용 구역 identity 유지;
- migration으로 개발 미리보기의 Site 기기와 로컬 Zone entity 제거;
- 계약상 기존 ESPHome 물리 entity와 독립적으로 유지.

`verified` 전 실환경 검증:

- 대상 Home Assistant 컨테이너에서 HACS Custom Repository 신규 설치와 integration reload;
- 실기기 Zeroconf 발견, 수동 fallback, 중복 등록 거부 및 DHCP/IP 주소 변경;
- Site Leader, Group Leader 및 Configuration Owner의 짧고 긴 장애에서 기기와 entity ID,
  unavailable 처리, Repairs 지연 및 복구 유지;
- 기존 미리보기 설치를 제자리 업데이트한 뒤 중복, 고아 또는 이름이 바뀐 Space와 공용
  구역 entity가 없는지 확인.

### 12단계: 외부 Satellite SDK

산출물:

- Python reference Satellite 및 bridge 예제;
- 기계가 읽을 수 있는 schema와 protocol fixture;
- conformance server와 장애 simulation;
- protocol 안정화 이후 최소 C++ 또는 ESPHome reference;
- capability, pairing, calibration 및 security contributor guide.

테스트 및 완료 조건:

- 타사 simulator가 NAMO tracker code 없이 관측 전용 Satellite로 가입합니다.
- presence-only source가 spatial target을 만들 수 없습니다.
- presence-only source가 재실을 시작하거나 다시 준비할 수 없고 이미 준비된 세션만 유지합니다.
- Satellite가 Leader 또는 설정 replica 권한을 광고하거나 받을 수 없습니다.
- 호환되지 않는 required feature가 명확히 실패합니다.
- 예제가 firmware와 같은 fixture를 통과합니다.

### 13단계: 공개 Beta와 Migration

현재 기반:

- Developer 서명 package는 사용자가 최대 10분 동안 1회 설치를 허용하지 않으면 OTA partition을
  열기 전에 거부합니다. 허용 상태는 RAM에만 저장하고 알파 설치 성공 시 소모하며, Release 서명
  package의 무인 설치에는 영향을 주지 않습니다.
- 기존 연결 및 readiness와 구형 로컬 레이더 sidecar가 남은 직접 업데이트에서 부팅 시
  canonical 평면도의 축척으로 누락된 X/Y 축척을 복구합니다.
- 현재 대시보드 저장은 X/Y 축척을 모두 저장하며 구형 단일 평균 축척도 계속 읽을 수 있습니다.
- 프론트 fixture는 canonical profile이 유효한 순차 업데이트 표시를 검증하고, 네이티브 테스트는
  직접 업데이트 복구와 재부팅 재현을 검증합니다.
- signed OTA는 고정 P-256 패키지, 사전 trust 검증, 실제 업로드 일치 확인, 기록 파티션 digest
  검증, A/B Trust Store, 부팅 파티션 활성화 전 trust commit, Release 키의 epoch 고정 공격을
  푸는 Root 복구, 릴리즈별 anti-rollback epoch, 교체 P-256 공개키 ID 검증, 서명된 Developer
  빌드, 정확한 multipart 경로 제한, 대시보드 no-downgrade 분기로 구현했습니다. 호스트 테스트와
  비파괴 320바이트 변조·경로 혼동 실기기 smoke 도구는 완료했으며 공개키를 넣은 펌웨어
  컴파일과 실기기 보안 smoke는 릴리즈 gate로 남아 있습니다.
- 최종 호환성 확인은 같은 펌웨어를 순차 업데이트 기기 쌍과 직접 업데이트 기기 쌍에 올려 한 번의
  smoke에서 확인합니다. 두 쌍 모두 canonical profile, fusion 상태, 상대 레이더 위치 및 융합 타깃
  표시를 통과하기 전에는 이 실기기 gate를 완료로 보지 않습니다.

산출물:

- standalone Site에서 Fusion Group으로 opt-in migration;
- rollback 및 backup 규칙;
- API, dashboard, HA, SmartThings 및 Satellite compatibility matrix;
- 보안, 개인정보, attestation 발급, root rotation, revoke 및 Community NAMO Node 안내;
- 설계 목표와 검증된 지원을 구분하는 release note.
- 1회성 펌웨어 완료 팝업에 패키지 release note를 표시합니다. 완료 팝업을 먼저 제공하며,
  메타데이터 계약이 준비되기 전에는 빈 release note 영역을 표시하지 않습니다.

테스트 및 완료 조건:

- 두 시야각 구성의 최소 24시간 실제 방 soak test;
- upgrade, downgrade, backup, restore, participant 제거 및 factory reset;
- 공개키를 넣은 실기기에서 정상 Release·Developer 패키지, 전 바이트 변조, 알 수 없거나
  폐기된 키, rollback, 중단 업로드, 중단 Trust Store commit, Root 복구, 재부팅 및 자원 smoke;
- Developer 서명 알파 패키지는 RAM에만 저장되는 10분·1회 설치 허용이 필요하며,
  Release 패키지의 무인 설치 경로에는 영향을 주지 않습니다.
- 기존 HA 또는 SmartThings 자동화를 자동으로 다시 쓰지 않습니다.

### 14단계: Radar Map Manager 단독 센서 호환

상태: 계획됨, 구현은 명시적으로 보류합니다. NAMO production fusion이나 Home Assistant
지원의 선행 조건이 아닌 선택적 호환 계층입니다.

범위:

- 기존 ESPHome Target 1-3 X/Y 엔티티, mm 단위와 MAC suffix가 붙은 고유 prefix로
  단독 NAMO 레이더 한 대를 지원합니다.
- RMM은 2D 배치와 별도 tracking·smoothing 비활성 구성을 기준으로 검증합니다.
- 새 firmware HTTP 좌표 API나 NAMO Home Assistant Integration의 중복 좌표 stream은
  만들지 않습니다.
- 융합 Group을 여러 RMM 레이더로 광고하지 않습니다. 융합, 공용 구역, 승계 및 Site 진단은
  NAMO 전용 기능으로 유지합니다.
- 외부 프로젝트인 RMM은 검증한 버전을 명시하는 best-effort 호환 대상으로 다룹니다.

테스트 및 완료 조건:

- RMM이 단독 기기의 Target X/Y prefix를 자동 발견하거나 수동 선택할 수 있습니다.
- 타깃 생성, 이동, 소멸, 재부팅, 재연결 및 사용 불가 복구에서 0 좌표 오탐이 없습니다.
- 2D 방향과 mm 단위가 실제 레이더 화면과 일치합니다.
- 현재 2초 entity update와 1초 옵션을 비교하고, 시각 개선과 firmware·Home Assistant
  자원 회귀 gate를 함께 통과할 때만 주기를 변경합니다.
- 융합 Site는 이 호환 경로에서 지원하지 않음을 명확히 유지합니다.

### 15단계: Bounded Consensus Formal 검증

상태: 계획됨, 착수 조건이 있는 선택 단계입니다. 완료한 P0/P1 구현을 대신하는 단계가 아니며
네이티브, 브라우저, 가상 cluster 및 실기기 테스트를 대체하지 않습니다. 실행 가능한 역할과
권위 계약이 안정된 뒤, v1.0 또는 quorum, 생존 기기 재구성, 역할 가용성 정책을 변경하기
전에 다시 검토합니다.

범위:

- 1~7대 제어 평면을 작은 TLA+ 상태 머신으로 기술합니다.
- Configuration Owner, Site Leader, Group Leader, canonical membership,
  authority epoch·generation, term, lease, election-view digest, credential
  완비 여부, 네트워크 분할, 전원 차단, 후보 장애 및 stale 재합류를 모델링합니다.
- canonical view별 단일 권위, authority 역행 금지, stale mutation 금지, minority
  fail-closed, 다른 적격 Group 후보가 있을 때 삼중 역할 배정 금지 및 후보 등장 후
  삼중 역할 해제를 safety/liveness 속성으로 검사합니다.
- 명시적인 fairness 가정 아래 안정적인 적격 quorum의 역할 선출, 승계 후보 장애 후
  고착 방지 및 stale 복귀 기기의 follower 수렴을 liveness 속성으로 검사합니다.
- Alloy는 정적인 membership과 역할 배치 관계에만 선택적으로 사용합니다. 시간 모델로
  명확하게 표현할 수 없는 실제 구멍을 찾지 않는 한 두 번째 formal model을 추가하지 않습니다.
- 유효한 counterexample은 모두 실행 가능한 host fault-sequence test의 seed 또는 fixture로
  보존합니다.
- formal model에서 production firmware를 생성하지 않으며, model 통과를 HTTP, flash,
  stack, timing 또는 radio 정확성의 근거로 사용하지 않습니다.

착수 및 완료 조건:

- 같은 revision 복구, 역할 anti-affinity, pairing polling 및 bounded handover retry 회귀
  테스트가 계속 통과해야 합니다.
- safety, liveness 및 현재 생존 기기 가용성 계약이 실행 가능한 assertion과 seed 기반
  1~7대 host test로 존재해야 합니다.
- 선언한 bounded state space에서 safety counterexample이 없고, 문서화한 fairness 가정
  아래 liveness counterexample이 없어야 합니다.
- 발견한 모든 counterexample에 실행 가능한 회귀 테스트를 추가해야 해당 문제를 해결한
  것으로 봅니다.
- formal model 결과와 관계없이 다중 실기기 장애·재합류 smoke를 release gate로 유지합니다.

## 5. 릴리즈 단위

| 단위 | 포함 내용 | 사용자에게 보이는 변화 |
| --- | --- | --- |
| A | 0-3단계 | Fusion 없음. 플랫폼 기반만 추가. |
| B | 4-6단계 | 제한된 7개 관측원 Shadow Fusion과 진단. |
| C | 7단계 및 최소 대시보드 | Opt-in 제한형 7개 관측원 production fusion. |
| D | 10-11단계 | 안정적인 SmartThings 및 HA Space 자동화. |
| E | 8단계 및 12단계 | 검증된 계층형 용량과 개방형 Satellite SDK. |
| F | 13단계 | 공개 플랫폼 beta. |
| G | 14단계 | 선택적 단독 센서 Radar Map Manager 호환. |
| H | 15단계 | v1.0 또는 quorum 계약 변경 전 선택적 bounded consensus 보증. |

## 6. 중단 조건

다음 상황에서는 구현을 멈추고 설계를 다시 검토합니다.

- 명시적인 migration 없이 standalone 동작이 바뀜;
- 계약과 fixture 전에 계획 API를 사용함;
- Leader 전환이 잘못된 unoccupied를 발생시킴;
- 실시간 경로에 무제한 allocation 또는 queue가 생김;
- 타사 입력이 pairing 또는 trust를 우회함;
- Site 확장에 all-to-all 고빈도 통신이 필요하거나 Site Leader가 Group observation 또는
  fused target을 중계해야 함;
- ESP32-S3 한 대가 Fusion Group 둘 이상의 Group Leader를 맡아야 함;
- 제조사 attestation이 로컬 Community NAMO Node 동작의 암묵적인 필수 조건이 됨;
- attestation을 capability, membership 또는 calibration 증명으로 취급함;
- HA 또는 SmartThings identity가 현재 Leader나 IP에 의존함;
- 큰 보정값으로 잘못된 물리 설치를 숨김.

## 7. 문서 갱신 규칙

단계를 시작할 때 `in_progress`로 바꿉니다. 집중 테스트를 통과해야 `implemented`, 해당 단계에
명시한 하드웨어, replay 및 호환성 gate를 통과해야 `verified`로 바꿉니다. 같은 변경에서 두 언어
문서를 함께 갱신합니다.
