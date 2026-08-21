# 다중 센서 API 계약

상태: **대체된 설계 메모. 구현된 API 계약이 아님.**

영문 문서: [multi-sensor-contract.md](multi-sensor-contract.md)

이 문서는 이전에 고정 `primary` 및 `secondary` 센서 모델을 제안했습니다. 이 모델은 분산형
NAMO 공간 플랫폼 설계로 대체되었습니다.

향후 작업은 다음 문서를 사용합니다.

- [공간 플랫폼 아키텍처](platform/architecture.ko.md)
- [Satellite 규약](platform/satellite-protocol.ko.md)
- [구현 로드맵](platform/implementation-roadmap.ko.md)
- [ADR-0001: 분산 공간 플랫폼](adr/0001-distributed-spatial-platform.ko.md)
- [ADR-0002: 인증된 Observation Transport](adr/0002-authenticated-observation-transport.ko.md)

새 설계도 이전 메모의 유효한 제약은 유지합니다.

- 사용자 표시 문장이 아닌 구조화된 observation을 교환합니다.
- 명시적인 TTL과 ordering으로 오래된 observation을 만료합니다.
- standalone 로컬 동작을 유지합니다.
- 공간 fusion 전에 명시적인 sensor pose, Floorplan transform 및 calibration을 요구합니다.
- confidence 병합과 충돌 해결을 HA 및 SmartThings에 맡기지 않습니다.

다음 가정은 교체합니다.

- 물리 NAMO Node를 자동화용 영구 `primary`로 설정하지 않습니다.
- 각 Fusion Group에 NAMO Node가 최소 한 대 있으며, 승인된 NAMO Node 중에서 `term`과
  `lease`를 가진 일시적인 Group Leader를 선출합니다.
- Site, Space, Group 및 플랫폼 entity 식별자는 Leader나 IP 주소에 의존하지 않습니다.
- capability 기반 개방형 Satellite 계약으로 타사 센서를 관측 전용으로 허용하되 Leader나
  설정 권한을 부여하지 않습니다.
- 공식 NAMO 하드웨어는 기기별 제조사 attestation을 사용하고, 명시적으로 승인된 community
  NAMO Node도 계속 지원합니다.
- presence-only 근거는 이미 준비된 재실 세션을 유지할 수 있지만 재실을 시작하거나 공간
  타깃을 만들 수 없습니다.
- 아키텍처는 저빈도 Site Leader와 고빈도 Group Leader를 분리하고 Site당 Group 최대 8개,
  Group당 센싱 participant 최대 7개를 목표로 하며 두 한도 모두 roadmap의 자원 gate를
  통과해야 지원을 선언합니다.
- 모든 활성 Floorplan에는 독립적인 Configuration Owner 한 대가 있습니다. 배정된 Node는
  원본에서 인증된 chunk를 한 session씩 직접 받고 replica를 로컬에 commit합니다.
- Site Leader는 크기가 제한된 Floorplan/Group 원본, revision, 경로 및 health 메타데이터만
  저장하며 역할상 Floorplan payload를 중계하지 않습니다.
- 플랫폼 integration은 Site Leader를 Group directory로 사용하고 최종 논리 Space 상태는
  각 Group Leader에서 직접 소비합니다.

구현된 펌웨어 endpoint의 기준은 [현재 API 계약](api-contract.ko.md)입니다. 계획된 cluster 및
Satellite endpoint는 구현과 테스트를 갖추고 현재 API 계약에 추가되기 전까지 사용할 수 있다고
간주하면 안 됩니다.
