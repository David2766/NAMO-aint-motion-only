# ADR-0003: 참가자 자격 증명 저장 경계

상태: **3C단계 저장 기반 구현 및 native 검증 완료**

날짜: 2026-07-21

## 배경

3B단계에서는 공개 개발 키로 인증 observation envelope를 검증했습니다. Production 전송에는
원격 참가자별 비밀키가 필요하며, 기존 Site 한도인 원격 참가자 19개를 넘으면 안 됩니다.
기기의 내부 RAM은 제한적이고 과거에 큰 지역 snapshot 때문에 task stack overflow가
발생했습니다. Registry/readiness 저장소는 이미 radar data partition의 마지막 4 KiB sector
6개를 사용하므로 배포된 주소도 이동하면 안 됩니다.

## 결정

- 원격 참가자 19개로 제한된 heap-free `ParticipantCredentialRegistry`를 추가합니다.
- Record에는 participant ID, 불투명한 64-bit 조회 token, key version, enabled 상태,
  32-byte transport key 하나를 저장합니다.
- 두 slot과 checksum commit sector 하나를 사용해 transaction으로 저장합니다.
- 새 sector 3개는 기존 platform sector 6개 바로 앞에 배치합니다. 기존
  registry/readiness의 절대 주소는 그대로 유지합니다.
- Credential은 registry membership, readiness profile, Home Assistant API key,
  제조사 attestation key와 분리합니다.
- Observation transport는 credential owner를 참조하며 전체 key를 다시 복사하지 않습니다.
  참가자별 rate 상태만 RAM에 별도로 둡니다.
- 설정 원본은 새로운 장기 key 계층을 만들지 않습니다. 이미 합의된 pair credential을
  재사용하고 기존 Site replication transport에서 인증·암호화·generation 결합된 bundle을
  제공합니다. 수신 기기는 설정 원본, 같은 Group의 peer, active Site Leader 후보와 canonical
  역할의 승계에 필요한 credential만 받습니다.
- Pair credential은 방향이 있는 송신 신원입니다. 설정 원본이 아닌 Node는 모든 허용 peer에
  보낼 때 원본과의 pair record를 사용하고, 설정 원본은 목적지의 직접 record를 사용합니다.
  이 구조는 모든 기기 쌍을 사용자가 따로 연결하지 않아도 원본이 배포하는 제한된 Site
  credential mesh를 만듭니다.
- Configuration Owner 승계 시에는 `HMAC(key, localNodeId)`로 인증한 고정 상한 key
  탐색으로 방향성 신원을 다시 묶습니다. follower는 대체 bundle 저장이 끝날 때까지 인증
  control/data 송신을 멈추며, discovery 주소는 bundle 권한을 부여하지 않습니다. 저장할
  bundle은 필요한 record가 모두 있어야 하고 현재 Owner record가 요청 credential과
  정확히 같아야 합니다.
- `revoke`는 해당 credential을 비활성화하고 `forget`은 삭제합니다. 보조 정리 저장이
  실패해도 membership 차단이 최종 권한을 가집니다.
- 공개 API에는 `provisioned`, `enabled`, `keyVersion`, generation, count만 노출합니다.
  Token과 key byte는 절대 직렬화하지 않습니다.
- 개발용 smoke key는 RAM에서만 사용하고 credential transaction에 저장하지 않습니다.
- Raw key를 받는 HTTP endpoint는 만들지 않습니다. 안전한 community key agreement는
  3C단계에서 담당하고 rotation, recovery와 attestation transcript 결합은 3D단계에서
  구현합니다.

## 임베디드 제약

- Codec과 store workspace는 고정 크기이며 `static_assert`로 제한합니다.
- Decode와 복구는 caller/store 소유 buffer를 재사용하며 수 KB snapshot을 setup 또는
  HTTP task stack에 올리지 않습니다.
- Transactional bundle 병합의 registry 복사본은 main-loop stack이 아니라 PSRAM
  replication workspace를 사용합니다.
- Observation rate limiter는 512 byte 미만의 별도 고정 테이블을 사용합니다.
- Platform 저장 영역 확장으로 floorplan image 최대 용량만 12 KiB 줄어듭니다. 현재
  floorplan upload 제한은 이 경계보다 훨씬 작습니다.

## 실패 처리

- 잘못된 ID, 0인 token/version, token 충돌, 손상 record와 checksum은 차단합니다.
- Slot data를 먼저 동기화한 뒤 commit marker를 기록합니다.
- Commit 실패 시 새 runtime generation을 공개하지 않습니다.
- Commit된 slot이 손상되면 이전 정상 slot로 복구하고 commit marker를 고칩니다.
- Credential 저장소 실패는 인증 원격 전송만 비활성화하며 단독 재실 감지는 유지합니다.

## 검증

Native test는 용량 한도, token 충돌, enabled/disabled 조회, codec round trip, 손상,
transaction write 순서, commit 실패 후 재부팅, commit 복구, 기존 platform sector 6개의
주소 보존을 확인합니다. Platform test는 공개 참가자 응답에 credential metadata만 있고
token이나 key field가 없는지 확인합니다.

Credential topology test는 구성원 7개인 Fusion Group, Group 사이의 저빈도 Leader 인증,
방향성 송신 key 선택, 일부 source 누락과 관계없는 record 거부, 배정된 Leader credential이
없을 때의 canonical Directory 차단도 확인합니다.
