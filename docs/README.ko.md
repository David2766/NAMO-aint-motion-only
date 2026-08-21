# 문서 인덱스

프로젝트 문서를 읽거나 수정할 때 여기서 시작합니다.

## 현재 계약

- [api-contract.md](api-contract.md): 안정된 HTTP API 응답 구조, 상태 코드, 오류 코드와 frontend/firmware 호환 규칙 영문 문서.
- [firmware-signing.ko.md](firmware-signing.ko.md): 서명 OTA 패키지, 키 보관, 교체, 유출 복구, 전환 및 릴리즈 gate 문서.
- [api-contract.ko.md](api-contract.ko.md): API 계약 한국어 문서.
- [home-assistant-integration.md](home-assistant-integration.md): 현재 Home Assistant Site·Space 모델, entity 동작, 진단, migration 및 남은 릴리즈 gate를 설명하는 영문 문서.
- [home-assistant-integration.ko.md](home-assistant-integration.ko.md): Home Assistant 연동 한국어 문서.

## 공간 플랫폼 설계

- [platform/architecture.md](platform/architecture.md): 분산 Site, Floorplan별 Configuration Owner, 제한된 Fusion Group, 메타데이터 전용 Site Leader, 고빈도 Group Leader, 제한된 quorum·lease 권한 계약, 보정, 플랫폼 연동 및 현재 구현 경계를 설명하는 영문 문서.
- [platform/architecture.ko.md](platform/architecture.ko.md): 공간 플랫폼 아키텍처 한국어 문서.
- [platform/satellite-protocol.md](platform/satellite-protocol.md): NAMO Node 및 관측 전용 타사 Satellite의 observation, trust, attestation 및 conformance 계약 영문 초안.
- [platform/satellite-protocol.ko.md](platform/satellite-protocol.ko.md): Satellite 규약 한국어 문서.
- [platform/implementation-roadmap.md](platform/implementation-roadmap.md): 구현 단계, 완료한 분산 제어 P0/P1 수정, 남은 실기기 검증 gate, 릴리즈 단위 및 선택적인 v1.0 전 formal model checkpoint를 설명하는 영문 문서.
- [platform/implementation-roadmap.ko.md](platform/implementation-roadmap.ko.md): 구현 로드맵 한국어 문서.
- [adr/0001-distributed-spatial-platform.md](adr/0001-distributed-spatial-platform.md): 분산 NAMO Node Leader, 제한된 제어 평면, 개방형 관측 Satellite 및 폐쇄형 Node gate를 만들지 않는 제조사 attestation의 승인된 결정 영문 문서.
- [adr/0001-distributed-spatial-platform.ko.md](adr/0001-distributed-spatial-platform.ko.md): ADR-0001 한국어 문서.
- [adr/0002-authenticated-observation-transport.md](adr/0002-authenticated-observation-transport.md): 3B단계 UDP unicast, MessagePack, HMAC 및 임베디드 자원 경계 ADR 영문 문서.
- [adr/0002-authenticated-observation-transport.ko.md](adr/0002-authenticated-observation-transport.ko.md): ADR-0002 한국어 문서.
- [adr/0003-participant-credential-storage.md](adr/0003-participant-credential-storage.md): 구현 및 native 검증된 3C단계 참가자 자격 증명 transaction 저장과 임베디드 자원 경계 ADR 영문 문서.
- [adr/0003-participant-credential-storage.ko.md](adr/0003-participant-credential-storage.ko.md): ADR-0003 한국어 문서.
- [adr/0004-secure-community-pairing.md](adr/0004-secure-community-pairing.md): 3C단계 제한형 P-256 community pairing과 로컬 확인 흐름 ADR 영문 문서.
- [adr/0004-secure-community-pairing.ko.md](adr/0004-secure-community-pairing.ko.md): ADR-0004 한국어 문서.

## 대체된 설계 메모

- [multi-sensor-contract.md](multi-sensor-contract.md): 기존 고정 Primary 제안에서 현재 플랫폼 설계로 안내하는 영문 문서.
- [multi-sensor-contract.ko.md](multi-sensor-contract.ko.md): 대체된 메모의 한국어 문서.

## 재실 추적

- [presence-exit-zone.md](presence-exit-zone.md): Production tracker가 사용하는 현재 퇴실 지점과 방 경계 근거 영문 문서.
- [presence-exit-zone.ko.md](presence-exit-zone.ko.md): 현재 퇴실 근거 동작 한국어 문서.
- [presence-tracker.md](presence-tracker.md): 현재 tracker 책임, 경계 및 구현 메모 영문 문서.
- [presence-tracker.ko.md](presence-tracker.ko.md): 현재 tracker 메모 한국어 문서.
- [presence-tracker-future.md](presence-tracker-future.md): 현재 획득 경계를 유지하는 향후 검증 및 튜닝 작업 영문 문서.
- [presence-tracker-future.ko.md](presence-tracker-future.ko.md): 향후 tracker 메모 한국어 문서.

## 리플레이와 시뮬레이션

- [presence-replay.md](presence-replay.md): 리플레이 로그, ground-truth 라벨링, 점수와 도구 구조 영문 문서.
- [presence-replay.ko.md](presence-replay.ko.md): 리플레이 메모 한국어 문서.
- [presence-simulation.md](presence-simulation.md): 자연스러운 움직임 및 Monte Carlo 시뮬레이션 영문 문서.
- [presence-simulation.ko.md](presence-simulation.ko.md): 시뮬레이션 메모 한국어 문서.
- `tools/platform-sim`: V1 공간 protocol fixture, validation, 크기가 제한된 인증 datagram, golden vector, stream simulation 및 MessagePack benchmark. Network service나 Home Assistant integration은 아닙니다.
- `tools/namo-cluster`: production coordinator, 영속 설정, UDP 장애, 재시작, 분할 및 재합류를 2~7개 프로세스로 검증하는 host integration 도구입니다. 아직 firmware HTTP pairing·보정 handler를 실행하지 않으며 실기기 검증을 대체하지 않습니다.

## 수정 규칙

- `api-contract.md`에는 현재 코드가 지켜야 하는 규칙만 둡니다.
- 미래 설계는 별도 future/design 문서에 둡니다.
- 계획된 플랫폼 동작을 명확히 표시하고 초안 endpoint를 구현된 API처럼 나열하지 않습니다.
- 영어와 한국어 문서는 별도 파일로 유지합니다.
- 코드 변경이 API 응답에 영향을 주면 API 계약을 먼저 또는 같은 변경에서 갱신합니다.
