# ADR 0004: 안전한 Community 기기 연결

- 상태: 구현 및 host test 완료, 실제 다중 Node 복원력 검증 진행 중
- 날짜: 2026-07-21

## 배경

NAMO Node는 HACS, 클라우드, 중앙 Coordinator 없이 로컬에서 서로의 관측
자격증명을 만들어야 합니다. 과거 ESP32 HTTP task에서 stack overflow가 발생했기
때문에 타원곡선 연산과 영속 transaction을 request handler 안에서 직접 실행하면
안 됩니다. 또한 제조사 attestation은 아직 없으므로 이 경로가 공식 기기 출처를
보증한다고 표시해서도 안 됩니다.

## 결정

Community 연결은 두 기기에서 명시적으로 여는 3분 등록 session, 서로 반대인
Leader/Joiner 역할, 임시 P-256 ECDH, SHA-256 transcript 결합, HKDF-SHA256 키
파생, 역할별 HMAC proof, 6자리 확인 코드를 사용합니다. 시작 브라우저가 코드를
비교해 보여주고 사용자가 일치를 확인하면 두 기기의 confirm API를 모두 호출합니다.

RAM에는 고정 크기 session 하나와 command slot 하나만 둡니다. HTTP POST는 작업을
queue에 넣고 `accepted: true`와 `operationId`를 포함한 `200`을 반환하며, 실제 암호
연산과 저장은 component loop에서 처리합니다. 공개 응답에는 임시 offer, proof, 확인
코드와 lifecycle metadata만 포함합니다. private key와 파생 secret은 취소, 만료 또는
정상 commit 시 지웁니다.

대시보드의 연결 허용 안내, 암호학적 등록 session 및 화면 countdown은 동일한 180초
수명을 사용합니다. invitation은 계속 RAM에만 존재하며 그 자체로 등록을 열거나 신뢰를
부여하지 않습니다.

확인이 끝나면 상대 Node를 `community_unverified`, 인증된 제어 경로에 사용하는 활성
pairwise credential, quarantined membership으로 저장합니다. Joiner는 registry와 credential
저장이 먼저 성공한 뒤에만 Leader의 Site로 이동합니다. Readiness와 명시적인 activate는
별도 권한 gate로 남깁니다. 따라서 pairing만으로 상대 기기가 production 권한을 얻지는
않고 quarantine에 남습니다. canonical Floorplan 복제, radar pose와 calibration, readiness,
roster commit, 양방향 activation gate가 모두 통과한 뒤에만 production observation
authority를 활성화합니다.

## 결과

- 기기 연결이나 HACS 없이도 standalone 감지와 Home Assistant는 계속 동작합니다.
- 일부 저장이 실패해도 원격 participant가 active가 되지는 않습니다.
- 수동 IP 입력을 지원하므로 multicast discovery에 의존하지 않습니다.
- Community 연결은 임시 transcript key의 소유를 확인하지만 NAMO 제조 출처를
  증명하지는 않습니다.
- 로컬 HTTP를 사용하므로 attestation과 더 강한 channel 보호가 추가되기 전에는
  신뢰할 수 있는 LAN에서 사용자가 표시된 코드를 비교해야 합니다.
- key rotation, 교체 복구, 제조사 attestation은 이후 Phase 3C 작업으로 남습니다.
