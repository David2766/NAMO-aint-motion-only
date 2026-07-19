# 문서 인덱스

어떤 프로젝트 문서를 읽거나 수정해야 하는지 판단할 때 여기서 시작한다.

## 현재 계약

- [api-contract.md](api-contract.md): 안정화된 HTTP API 응답 구조, 상태 코드 규칙, 오류 코드 registry, 프론트엔드/펌웨어 호환성 규칙.
- [api-contract.ko.md](api-contract.ko.md): API 계약의 한국어 버전.

## 향후 API 설계

- [multi-sensor-contract.md](multi-sensor-contract.md): 향후 다중 센서 및 coordinator API 메모.
- [multi-sensor-contract.ko.md](multi-sensor-contract.ko.md): 다중 센서 메모의 한국어 버전.

## 재실 추적

- [presence-exit-zone.md](presence-exit-zone.md): production 트래커가 사용하는 현재 퇴실 지점 및 방 경계 근거.
- [presence-exit-zone.ko.md](presence-exit-zone.ko.md): 현재 퇴실 근거 동작의 한국어 문서.
- [presence-tracker.md](presence-tracker.md): 현재 tracker의 책임, 경계, 구현 메모.
- [presence-tracker.ko.md](presence-tracker.ko.md): 현재 tracker 메모의 한국어 버전.
- [presence-tracker-future.md](presence-tracker-future.md): 현재 획득 경계를 유지하는 향후 검증 및 튜닝 작업.
- [presence-tracker-future.ko.md](presence-tracker-future.ko.md): 향후 tracker 메모의 한국어 버전.

## 리플레이 및 시뮬레이션

- [presence-replay.md](presence-replay.md): 리플레이 로그, ground-truth 라벨링, 점수화, 도구 형태.
- [presence-replay.ko.md](presence-replay.ko.md): 리플레이 메모의 한국어 버전.
- [presence-simulation.md](presence-simulation.md): 자연스러운 움직임 및 Monte Carlo 시뮬레이션 메모.
- [presence-simulation.ko.md](presence-simulation.ko.md): 시뮬레이션 메모의 한국어 버전.

## 수정 규칙

- `api-contract.md`에는 현재 코드가 지켜야 하는 규칙만 둔다.
- 미래 설계는 별도의 future/design 문서에 둔다.
- 영어와 한국어 버전은 별도 파일로 유지한다.
- 코드 변경이 API 응답에 영향을 주면 API 계약을 먼저 또는 같은 변경에서 갱신한다.
