# ADR-0001: 분산 공간 플랫폼, NAMO Node 및 개방형 Satellite

상태: **아키텍처 승인됨, 제한된 다중 Node 검증 진행 중**

날짜: 2026-07-21

영문 문서: [0001-distributed-spatial-platform.md](0001-distributed-spatial-platform.md)

## 배경

현재 NAMO는 로컬 tracking, presence logic, 내장 대시보드, Home Assistant entity 및
SmartThings Edge Driver를 가진 독립적인 ESPHome 기기로 동작합니다. 향후 spatial fusion은 같은
방을 비슷하거나 서로 다른 각도에서 보는 센서를 지원해야 합니다. 여러 Floorplan, 여러 Space 및
서로 독립적으로 제한된 여러 Fusion Group을 지원하면서 고빈도 작업을 ESP32-S3 한 대에
몰지 않아야 합니다.

고정 Primary 기기를 사용하면 플랫폼 identity와 가용성이 하나의 물리 센서에 의존합니다. 필수
server는 fusion을 단순하게 만들지만 NAMO의 standalone 및 local-first 특성을 없앱니다. 모든 타사
driver를 펌웨어에 넣으면 플랫폼이 제품별 code와 memory 사용량에 묶입니다.

또한 NAMO는 공식 제조 하드웨어와 호환되는 community build를 구분하면서도 AGPL 플랫폼을
제조사 잠금 protocol로 만들면 안 됩니다. 따라서 기기 출처, protocol 역할 및 observation
capability를 서로 분리해 정의해야 합니다.

## 결정

NAMO는 다음 특성을 가진 분산 로컬 공간 플랫폼으로 설계합니다.

- 안정적인 Site, Floorplan, Space, Fusion Group, NAMO Node, Satellite 및 공통 participant 식별자;
- NAMO Core Runtime과 Node 계약을 실행하는 기기에만 사용하는 NAMO Node 명칭;
- 관측만 제공하는 승인된 외부 센서 또는 bridge에 사용하는 Satellite 명칭;
- 모든 활성 Fusion Group에 최소 한 대의 NAMO Node;
- 승인된 NAMO Node 중에서만 선출하고 해당 Group의 고빈도 observation을 받아 융합하는
  Group별 동적 Group Leader 한 대;
- 승인된 NAMO Node로 제한하고 크기가 제한된 Group directory와 Site health를 담당하는
  독립 저빈도 Site Leader 한 대;
- 활성 Floorplan마다 정확히 한 대의 Configuration Owner와 Floorplan별 독립 소유권,
  generation 및 승계, 물리 NAMO Node 한 대당 활성 Floorplan 소유권 최대 하나;
- 각 Floorplan의 Configuration Owner에서 배정된 NAMO Node로 직접 전송하는 인증 chunk
  복제, 원본 측 활성 전송 한 개 및 결정적인 제한 대기 slot;
- NAMO Node 한 대인 Group의 standalone 동작;
- Site 전체 raw data broadcast가 아닌 Group-local Observation 교환;
- 타사 센서를 위한 개방형 capability 기반 Satellite 계약;
- Home Assistant와 SmartThings용 안정적인 논리 Space 식별자;
- 명시적인 pairing, trust, versioning, replay 방지 및 calibration;
- Site당 Fusion Group 최대 8개, Group당 Group Leader 한 대와 다른 센싱 participant 최대
  6대라는 설계 목표 및 지원 선언 전 검증;
- 플랫폼 integration이 Site Leader directory에서 현재 Group Leader를 확인한 뒤 Group 상태를
  해당 Group Leader에서 직접 읽는 구조.

공식 production 하드웨어에는 기기별 고유 private key와 NAMO manufacturer root로 서명한 device
certificate를 넣습니다. 로컬 pairing 중 challenge-response로 `official_verified` 출처를 확인합니다.
Private key는 기기 밖으로 내보내지 않으며 attestation만으로 Site membership, capability,
Group 배치 또는 calibration을 부여하지 않습니다.

NAMO Core Runtime을 실행하는 community build는 로컬 관리자의 명시적인 승인 후
`community_unverified`로 등록할 수 있습니다. Leader 자격은 별도의 opt-in 승인을 받아야 합니다.
외부 Satellite는 vendor certificate 또는 protocol conformance 여부와 관계없이 관측 전용입니다.

Site와 Group 권한은 범용 복제 로그 합의 프로토콜 대신 제한된 제어 평면 계약을 사용합니다.
역할은 canonical 설정 구성원의 엄격한 과반수, 일치하는 election-view digest, 최신 term과
lease fence, 완전한 credential을 모두 만족할 때만 권한을 가질 수 있습니다. 소수 파티션은 로컬
감지는 유지하지만 분산 권한은 해제합니다. 역할 배치는 결정론적이며 anti-affinity를 우선합니다.
현재 Site와 Group election digest는 설정된 전체 canonical roster, roster revision 및 선택된
winner를 식별합니다. 생존 상태, Directory 배정 및 credential 준비 상태는 변하는 election
identity가 아니라 별도의 admission fence로 사용합니다. 따라서 Configuration Owner 승계나
구성원 복귀 중 replica와 heartbeat가 서로 다른 시점에 수렴하더라도 살아 있는 Leader를
흔들면 안 됩니다. heartbeat v12가 이 계약을 전달하며 순차 업데이트 중에는 winner가 결합된
v11 호환 계약도 유지합니다.
용량상 필요하면 Configuration Owner가 Leader 역할 하나를 겸할 수 있고, 다른 적격 Node가
있으면 Site Leader와 Group Leader를 분리합니다. 해당 Group에 다른 적격 Node가 전혀 없을
때에만 한 Node의 세 역할 겸임을 긴급 fallback으로 허용하고, 적격 Node가 생기면 즉시 역할을
넘깁니다. Site Leader 교체 후보에서는 현재 canonical Group Leader를 역할 없는 후보와
Owner만 맡은 후보보다 뒤에 두어 무관한 Site 장애가 Group 권위 변경으로 연쇄되지 않게 합니다.

Raft나 Paxos를 사용하지 않는 것은 의도적인 결정입니다. Site 설정에는 단일 writer가 있고,
Site와 Group leadership은 현재 관측에서 다시 계산할 수 있는 파생 runtime 상태를 보호합니다.
대신 같은 revision 충돌, 오래된 Node 재합류, 후보 장애, 역할 배치, 영속 저장 실패, 일시적인
전송 실패를 결정론적 모델과 장애 시퀀스 테스트로 검증해야 합니다. 실행 가능한 계약이 안정된
뒤 작은 TLA+ 모델을 추가할 수 있지만, native, 다중 프로세스, 실기기 테스트를 대체하지 않습니다.

Presence-only 근거는 재실을 시작하거나 다시 준비하거나, 공간 타깃을 만들거나, 타깃 수를
늘리거나, 공간 Zone을 재실로 바꿀 수 없습니다. 허용된 point track 또는 motion 근거로 이미 준비된
세션을 확인하거나 유지하는 데만 사용할 수 있습니다. 이 규칙은 LD2410C 보조와 같은 외부 Satellite
근거에 모두 적용합니다.

현재 로컬 tracker는 로컬 sensor acquisition과 track 안정화 책임을 유지합니다. Fusion은 adapter를
통해 크기가 제한된 local observation을 받습니다. Platform integration은 최종 Group 상태를 소비하며
fusion을 다시 구현하지 않습니다.

## 결과

장점:

- 단일 기기와 다중 기기 설치가 하나의 모델을 사용합니다.
- 로컬 동작에 영구 coordinator가 필요하지 않습니다.
- Leader 교체가 자동화 entity를 다시 만들지 않습니다.
- 타사 센서가 NAMO tracker 내부를 수정하지 않고 연동할 수 있습니다.
- 고빈도 통신과 계산을 Fusion Group 범위로 제한합니다.
- Site Leader가 Floorplan payload를 저장하거나 중계하지 않아도 Site coordination이 가능합니다.
- 각 NAMO Node는 자신에게 배정된 Floorplan만 영속 저장합니다.
- 공식 기기는 검증 가능한 출처와 간소화된 로컬 pairing을 제공합니다.
- 같은 오픈소스 계약 아래 community NAMO build를 계속 허용합니다.
- 관측 전용 Satellite가 Site 설정이나 최종 자동화 상태의 제어권을 조용히 가져갈 수 없습니다.

비용:

- identity, election, expiry, 설정 충돌 및 security 규칙이 핵심 구현 작업이 됩니다.
- 제조 공정에 기기별 key 발급과 certificate 기록이 필요합니다.
- root rotation, revoke, 교체, 복구 및 안전한 key 저장에 lifecycle 절차가 필요합니다.
- 안정적인 논리 Site 및 Space entity를 위해 Home Assistant NAMO Integration이 필요합니다.
- SmartThings Edge가 물리 participant와 논리 Site/Space device를 분리해야 합니다.
- 좌표를 fusion하기 전에 calibration과 time quality를 명시해야 합니다.
- 다중 Floorplan 설정에는 크기가 제한된 versioned 원본 directory와 Floorplan별 독립
  원본 승계 상태가 필요합니다.
- 7개 입력 Group과 8개 Group Site 목표에는 서로 분리된 simulator 및 하드웨어 부하 검증이 필요합니다.
- 제한된 권한 프로토콜에는 명시적인 safety/liveness 불변식과 재현 가능한 장애/재합류
  검증이 필요하며, 결정론적 선출만으로는 충분한 증거가 아닙니다.

## 거절한 대안

### 고정 Primary와 Secondary 기기

물리 Primary가 자동화 identity, 장애 지점 및 확장 병목이 되므로 장기 모델로 사용하지 않습니다.
일시적인 Group Leader는 사용하지만 영구 식별자가 아니라 한 term의 권한자입니다.

### 필수 중앙 Server 또는 Host Coordinator

NAMO 한 대가 Home Assistant, SmartThings, Docker host 또는 cloud service 없이 완전히 동작해야 하므로
기본 구조로 사용하지 않습니다. 일반 host는 fusion code를 실행할 수 있다는 이유만으로 NAMO Node가
되지 않습니다. 향후 host 권한을 추가하려면 별도 ADR이 필요하며 NAMO Node trust 경계를 조용히
약화시키면 안 됩니다.

### Full Peer Mesh

Site 전체 고빈도 all-to-all 통신은 확장성이 낮고 ESP32 자원을 낭비하므로 사용하지 않습니다.
발견과 설정은 Site 전체 범위일 수 있지만 observation은 Group-local이며 현재 Group Leader로 보냅니다.

### 모든 센서용 Firmware Plugin

제품별 dependency가 펌웨어 크기를 늘리고 릴리즈를 결합하며 안정적인 compatibility 경계를
우회하므로 사용하지 않습니다. 외부 adapter는 Satellite 계약을 출력해야 합니다.

### HA 또는 SmartThings에서 Fusion

내장 대시보드, HA 및 SmartThings가 서로 다른 답과 availability 의미를 만들기 때문에 사용하지 않습니다.
로컬 Group state가 source of truth이며 platform adapter는 이를 노출만 합니다.

### 제조사 Certificate를 유일한 Node 허가증으로 사용

암호학적 출처와 protocol 권한은 서로 다른 문제이므로 사용하지 않습니다. 모든 로컬 build에 NAMO 발급
certificate를 요구하면 AGPL 소스 공개와 개방형 플랫폼 목표의 실질적 의미가 약해집니다. 공식
attestation은 검증 상태, 간소화된 등록 및 기본 Leader 자격을 제공합니다. Community Node는 수동이며
인증되지 않았음을 명확히 표시합니다.

### Attestation을 자동 Trust로 사용

정품 기기도 잘못된 Site, Space 또는 Group에 배정될 수 있고, 오래된 firmware를 사용하거나 실제로
없는 sensor capability를 광고할 수 있으므로 사용하지 않습니다. 명시적인 로컬 pairing, 배정,
capability 검증 및 calibration은 계속 필수입니다.

## 재검토 조건

실측 결과 ESP32급 Group Leader가 현실적인 Group에서 승인된 safety margin을 만족하지 못하거나
지원 LAN 환경에서 신뢰할 수 있는 Leader 선출이 불가능하다면 이 결정을 재검토합니다. 먼저 Group
크기 축소, 더 엄격하게 제한된 workload 또는 하드웨어 목표 변경을 검토합니다. Host 권한 도입,
NAMO Node 자격 변경 또는 community 동작에 제조사 attestation을 필수로 만드는 결정은 구현 중
편법으로 넣지 않고 새 ADR로 남겨야 합니다.
