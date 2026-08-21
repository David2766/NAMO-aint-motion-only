# ADR-0002: 인증된 Observation Transport

상태: **3B단계 shadow 구현 및 실기기 검증 완료**

날짜: 2026-07-21

영문 문서: [0002-authenticated-observation-transport.md](0002-authenticated-observation-transport.md)

## 배경

NAMO는 Home Assistant, SmartThings, broker 또는 고정 coordinator를 실행 필수 조건으로
만들지 않으면서 Group Leader 한 대와 다른 센싱 participant 최대 6대의 observation을
제한된 비용으로 전달해야 합니다.
Transport가 로컬 감지를 막거나 제한 없이 queue를 키워서는 안 되며, 인증되지 않은 LAN
송신자가 observation admission까지 도달해서도 안 됩니다. ESP32-S3는 PSRAM이 있더라도
internal RAM과 task stack 여유가 작습니다.

논리 Observation V1 contract와 MessagePack codec은 이미 호스트 simulator에 있습니다.
Participant identity, membership, readiness 및 fail-closed admission은 transport와 분리된
권한 경계로 유지합니다.

## 결정

- 발견은 기존 `_namo._tcp` mDNS와 주소 수동 입력 fallback을 유지합니다.
- Pairing과 저빈도 설정은 기존 HTTP를 유지합니다.
- Observation V1은 현재 Group 수신자에게 보내는 UDP unicast를 사용합니다.
- Observation에는 UDP multicast, broadcast, ACK 및 재전송을 사용하지 않습니다. 더 새로운
  observation이 대기 중인 이전 observation을 대체합니다.
- 원격 participant 하나는 초당 최대 2개 observation을 보냅니다. 원격 participant 6대의
  Group Leader 수신 상한은 초당 observation datagram 12개입니다.
- 인증된 datagram 전체 크기는 최대 768바이트입니다. Network byte order의 고정 24바이트
  prefix, 최대 712바이트 MessagePack과 전체 32바이트 HMAC-SHA256 tag로 구성합니다.
- Prefix에는 magic, envelope version, message kind, flags, prefix 크기, key version,
  64비트 participant lookup token, payload 길이와 reserved field를 넣습니다. 지원하지 않는
  값이나 0이 아닌 reserved field는 fail-closed로 거절합니다.
- Lookup token은 `participantId`의 SHA-256 앞 64비트입니다. 이는 크기가 제한된 key 조회용
  보조값일 뿐 identity나 인증 수단이 아닙니다. 인증된 MessagePack payload의 participant
  identity가 최종 기준입니다.
- HMAC은 prefix와 전체 MessagePack payload를 보호합니다. 수신자는 packet 크기와 prefix를
  검사하고, 활성화된 participant key를 찾고, HMAC을 검증한 다음에만 MessagePack을
  파싱하고 descriptor/readiness/admission 규칙을 순서대로 적용합니다.
- Transport key는 정확히 32바이트이며 Site의 participant 하나에만 유효합니다. Home
  Assistant API key 및 제조사 attestation private key와 분리합니다. Key 발급, 저장, rotation
  및 redaction은 3C단계에서 구현하며 유효한 key가 없으면 transport를 활성화하지 않습니다.
- 3B단계 출력은 shadow 전용입니다. 수락한 원격 observation은 진단 상태만 갱신할 수 있고
  production presence, target, Zone, Home Assistant entity 또는 SmartThings 상태를 바꿀 수 없습니다.

호스트 구현과 golden vector는 `tools/platform-sim`에 둡니다. Firmware에는 같은 envelope,
HMAC, MessagePack schema와 admission core를 사용하는 명시적 수신 전용 smoke 경로를
구현했습니다. 일반, beta, release build는 UDP 수신기를 생성하지 않습니다. Smoke image는 공개
테스트 key 하나를 사용하므로 배포하면 안 됩니다.

## 결과

장점:

- packet, 전송률, participant 및 latest-wins 제한이 임베디드의 고정 저장 경계로 직접 이어집니다.
- 인증되지 않은 MessagePack을 파싱하기 전에 HMAC을 검증합니다.
- Standalone 동작에 broker나 외부 integration이 필요하지 않습니다.
- Packet loss가 재전송 queue를 만들거나 로컬 tracking을 지연시키지 않습니다.
- 외부 Satellite도 같은 논리 MessagePack payload를 사용할 수 있습니다.

비용:

- UDP는 전달, 순서 및 기밀성을 보장하지 않습니다.
- Firmware traffic을 인증하려면 membership credential 발급을 먼저 완성해야 합니다.
- 수신 진단과 실기기 테스트에서 packet loss, 인증 실패, 만료 및 participant admission
  실패를 구분해야 합니다.
- LAN packet을 수동으로 수집할 수 있는 사용자는 payload를 볼 수 있습니다. 기밀성 추가는
  별도 ADR이 필요하며 V1 envelope를 조용히 바꾸면 안 됩니다.

## 제외한 대안

### 인증되지 않은 UDP

Site와 participant 식별자는 secret이 아니며 위조를 막지 못하므로 제외했습니다.

### Observation마다 HTTP 사용

연결 및 request 처리 비용이 고빈도 traffic을 기존 HTTP task에 올리고, 실기기 검증에서 이미
발견한 자원 압박을 반복할 수 있어 제외했습니다.

### Participant별 WebSocket 또는 지속 TCP

연결 상태, buffering, 재연결 동작과 head-of-line blocking이 RAM 및 lifecycle 복잡성을
늘리므로 V1에서는 제외했습니다. 측정 결과 UDP가 부적합하면 다시 검토할 수 있습니다.

### MQTT 또는 필수 Broker

NAMO Node 한 대가 broker나 home automation platform 없이 완전히 동작해야 하므로 제외했습니다.

## 검증 조건

- 잘못된 key, 변조 payload, 알 수 없는 key version, 잘못된 prefix, 잘린 packet 및 크기 초과
  packet을 논리 admission 전에 거절합니다.
- 중복, 역순, 이전 boot 재사용, 만료 및 재시작 stream에 기존의 제한된 동작을 유지합니다.
- Target 3개 packet이 768바이트 안에 들어갑니다.
- 원격 participant 6대가 초당 packet 2개를 보내도 participant별 최신 observation 하나만 유지합니다.
- Firmware는 고정 RX buffer 하나와 loop당 제한된 packet 처리 예산만 추가합니다.
- 3B단계를 verified로 바꾸기 전에 실기기에서 internal heap, task stack, parse 시간, packet
  drop, standalone 재시작 및 로컬 감지를 측정합니다.

호스트 검증과 firmware 구현을 완료했습니다. 실기기에서 정상 observation 수락, 인증 실패,
rate limit, 중복 거부, 정리, 단독 재실, soft restart와 재시작 후 lifecycle smoke를 확인했고
stack overflow나 rollback이 없었습니다. 따라서 3B단계를 verified로 닫으며 production credential
교환과 권한 부여는 3C단계에 남깁니다.
