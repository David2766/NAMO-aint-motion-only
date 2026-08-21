# Home Assistant 연동

상태: 개발 미리보기입니다. 이 연동은 아직 릴리즈 검증을 완료하지 않았습니다.

## 개발 환경에 설치

1. `custom_components/namo`를 Home Assistant 설정 디렉터리의
   `custom_components/namo` 경로에 복사합니다.
2. Home Assistant를 다시 시작합니다.
3. **설정 > 기기 및 서비스 > 통합 구성요소 추가**에서 NAMO를 선택합니다.
4. 자동 발견 항목이 나타나지 않으면 같은 NAMO 설치에 이미 연결된 기기 중 하나의
   주소를 입력합니다.

설정 항목은 IP 주소가 아닌 안정적인 Site 식별자를 기준으로 생성됩니다. 따라서 기기
주소가 바뀌어도 자동 발견으로 기존 항목을 갱신할 수 있으며, 같은 Site의 물리 Node가
중복 설정 항목을 만들지 않습니다.

## Home Assistant 모델

- NAMO 설정 항목 하나가 Site 하나를 나타냅니다.
- Site 아래의 각 논리 Space는 Home Assistant 기기 하나로 표시됩니다.
- 여러 Group이 있는 Site는 하나의 설정 항목 아래에 여러 Space 기기를 표시합니다.
- Node 한 대로 동작하는 standalone Site는 Group을 설정하기 전에도 로컬 Space 하나를
  표시합니다.
- 현재 Site Leader와 Group Leader 식별자는 전송 경로 정보이므로 entity 고유 ID에
  포함하지 않습니다.
- 새 Site 설정 항목은 발견한 기기 이름을 사용합니다. 기존 항목 제목이 기본값인
  `NAMO`로 남아 있고 Space가 하나뿐이면 사용 가능한 Space 이름을 한 번 적용합니다.
  사용자가 직접 수정한 제목은 유지합니다.

예를 들어 standalone Node 한 대와 융합 Node 두 대가 두 Site를 구성하면 Home Assistant는
NAMO 설정 항목 두 개를 저장합니다. Standalone Site는 Space 기기 하나를 표시하고, 융합
Site는 설정된 각 Space를 별도 기기로 표시합니다.

## 상태 모델

- Home Assistant는 낮은 빈도로 현재 관리 기기를 확인합니다.
- 해당 기기에서 크기가 제한된 Space 경로 디렉터리를 읽습니다.
- 각 Space의 현재 계산 기기를 직접 polling합니다.
- Timeout, 오래된 권한 또는 경로 변경이 발생하면 entity를 unavailable로 바꿉니다.
  인위적인 미재실 상태나 0 값을 게시하지 않습니다.
- Entity 고유 ID에는 안정적인 설치 및 Space 식별자를 사용하며, 기기 주소나 현재 Leader
  식별자를 넣지 않습니다.

연동은 Space 재실, 움직임, 타깃 수 및 진단 health entity를 생성합니다. 복제된 평면도에
저장된 활성 canonical 자동화 영역에도 재실 entity를 생성합니다. 고유 ID는 안정적인 Site,
Floorplan 및 영역 식별자를 사용하므로 Configuration Owner 또는 Group Leader가 승계되어도
entity를 다시 만들지 않습니다. 권한이 있는 원본을 사용할 수 없으면 영역 entity는 clear를
보고하지 않고 unavailable 상태가 됩니다.

기기 로컬 감지 Zone, 축소 Zone, 제외 Zone 및 보정 Zone은 의도적으로 연동에서 제외합니다.
이 항목들은 센서 내부 필터로 유지하며 canonical Home Assistant 영역과 합치지 않습니다.
기존 ESPHome 물리 entity도 독립적으로 유지합니다.

각 Space 기기는 상태, 감지 모드, 융합 상태, online 및 configured 기기 수, 원본 데이터
경과 시간을 진단 entity로 표시합니다. 필요한 경우 unavailable 원인을 Space 상태 entity에
함께 표시합니다. Site용 상위 기기를 별도로 만들지 않으며, Site 전체의 지속적인 문제는
Home Assistant Repairs로 보고합니다. 일시적인 재시작은 경고를 만들지 않습니다. 정상 상태가
아닌 Site가 30초 동안 지속되면 Repairs issue 하나를 만들고, 복구 후 자동으로 해제합니다.

평면도에 방 이름이 있으면 기기 이름으로 사용합니다. 방 이름을 바꾸더라도 안정적인 Site와
Space ID는 유지합니다.

연동 설정 항목 버전 2는 개발 미리보기 버전 1에서 생성한 혼동을 주는 Zone 재실 entity를
제거합니다. 버전 3은 사용하지 않는 Site 상태 entity와 상위 전용 기기를 제거한 뒤 기존
Space 기기와의 연결을 해제합니다. Space 재실, 움직임, 타깃 수 및 health entity ID는 유지하므로
기존 기록과 자동화도 보존됩니다.

## 로컬 계약 테스트

Home Assistant를 설치하지 않고 크기가 제한된 경로와 entity 등록 테스트를 실행할 수 있습니다.

```powershell
python -m unittest `
  tools.platform-sim.test_namo_ha_contract `
  tools.platform-sim.test_namo_ha_entities
```

이 테스트는 standalone fallback, Site 하나의 여러 Space, 안정적인 Space 및 canonical 영역
식별자, Configuration Owner 승계 식별자, 기존 Zone 및 Site 기기 정리, 표시 가능한 제한형
Space 진단, 지연된 Repairs 보고와 복구, unavailable 원인 보고, 동적 Space와 영역 추가,
번역 및 패키지 메타데이터를 검사합니다. Home Assistant container, 자동 발견, DHCP 및 실제
Leader 승계 검증은 아직 릴리즈 gate로 남아 있습니다.
