# 재실 트래커와 센서 융합

이 문서는 필터링된 LD2450 관측값에서 최종 재실·움직임 출력까지 이어지는 현재
production 경로를 설명한다.

## 1. 런타임 경로

- 트래커 경로가 기본값이다. 기존 lambda 경로는 고급 설정의 legacy 재실 옵션을
  통한 명시적인 fallback으로 남아 있다.
- 500ms ESPHome lambda는 필터링된 LD2450 관측값, PIR 상태, 방/퇴실 근거와
  선택적인 LD2410C 근거를 C++ 컴포넌트로 전달한다.
- `PresenceTracker`는 LD2450 track identity, 생명주기, 위치, 속도, 방향, 방 경계
  통과와 퇴실 근거를 소유한다.
- `PresenceFusion`은 PIR과 트래커 근거를 결합하고, 선택적인 LD2410C는 PIR 또는
  확정된 LD2450 트랙이 이미 시작한 세션을 유지할 때만 사용한다.
- 출력은 `/api/state`, Home Assistant 엔티티, 진단 및 리플레이 로그에 노출된다.

## 2. LD2450 추적

트래커는 LD2450 하드웨어 한도에 맞춰 관측값과 유지 중인 트랙을 각각 최대 3개
처리한다. 일반적인 Hungarian 구현 대신 가능한 작은 assignment 조합을 직접
평가한다.

트랙 상태는 다음과 같다.

- `tentative`: 최근 관측 횟수가 충분해질 때까지 대기한다.
- `confirmed`: 재실과 움직임/정지 개수에 직접 반영된다.
- `coasting`: 관측이 사라진 뒤 제한된 시간 동안 유지된다.
- `idle`: 사용하지 않는 슬롯이다.

위치와 속도는 시간 간격, process noise, measurement noise와 covariance를 제한한
등속도 Kalman filter를 사용한다. 순간 이동에 가까운 큰 관측 변화는 이전 추정치를
억지로 끌고 가지 않고 필터를 초기화한다. confirmed/coasting 트랙은 새 tentative
트랙보다 넓은 재연결 범위를 사용한다.

`trackScore`는 최근 hit, miss와 생명주기 상태에서 계산한 진단 점수다. 확률이나
사용자용 신뢰도 퍼센트가 아니다.

## 3. 퇴실 및 방 근거

각 트랙은 최근 퇴실 지점 진입과 방 경계 상태를 기록한다.

- 최근 설정된 퇴실 지점을 지난 트랙은 coasting이 짧아지고
  `lost_after_exit`으로 종료될 수 있다.
- 신뢰할 수 있는 방 안쪽에서 바깥쪽으로의 경계 통과는
  `lost_after_room_exit`이 될 수 있다.
- 두 근거 없이 사라지면 `lost_without_exit`이 되고 더 긴 비퇴실 coasting 정책을
  사용한다.
- `filter_blocked`는 관측 miss 한 프레임으로 처리하며 모든 트랙을 즉시
  초기화하지 않는다.

현재 drop 이벤트는 `PresenceFusion`이 소비한다. 확인된 퇴실 drop은 해당 소실에
대한 새로운 LD2410C hold를 막는다.

## 4. 재실 융합

PIR과 LD2450은 서로 보완하는 재실 획득 근거다. PIR 움직임 또는 confirmed/coasting
트래커 결과가 base presence를 유지할 수 있다. 필터가 막지 않는 한 움직임은 PIR
움직임과 트래커 움직임의 OR이다.

LD2410C 보조 규칙은 다음과 같다.

- 하드웨어가 없으면 `available: false`로 조용히 무시한다.
- LD2410C 단독으로 재실, 움직임, 타깃 개수, 방·구역 상태 또는 히트맵을 만들지
  않는다.
- 기존 PIR/트래커 세션과 정적 레이더 재실이 2초 동안 겹쳐야 보조가 armed된다.
- 확인된 퇴실 근거 없이 LD2450이 사라지면 armed된 LD2410C가 재실을 유지할 수
  있다.
- 퇴실/filter veto 없이 안정화 재실이 끝나면 이미 armed된 보조에 한해 30초 동안
  같은 세션의 재진입 기회를 한 번 허용한다.
- PIR 또는 LD2450이 다시 감지되면 primary/secondary 전환 없이 원래의 결합 근거로
  돌아간다.

표시 목적으로만, 보조가 명확한 단일 confirmed/coasting LD2450 트랙의 마지막
좌표를 유지할 수 있다. 이 좌표는 활성 타깃이 아니며 타깃 개수, 방, 구역, 보정,
히트맵 계산에서 제외한다.

## 5. 정적 레이더 거리 안정화

held-target 표시 신뢰도에는 LD2410C의 `detectionDistanceMm`만 사용한다.

- 1초에 최대 한 개의 유효 거리만 최근 3개 중앙값 이력에 넣는다.
- 첫 유효 샘플은 즉시 표시한다.
- 한 번의 튐은 안정값을 바꾸지 못한다.
- 정적 재실이 유지되는 동안 짧은 유효하지 않은 거리값은 마지막 안정값을
  유지한다.
- 정적 재실이 해제되거나 센서가 사용 불가가 되면 거리 이력을 초기화한다.
- 이동/정지 거리와 energy는 원시 진단값이며 위치 추론에 조합하지 않는다.

## 6. 검증과 한계

- 네이티브 테스트는 생명주기, assignment, Kalman smoothing, 퇴실/방 drop, 융합
  arming, exit veto, 재진입, 하드웨어 없음, held-target 신뢰도와 거리 안정화를
  다룬다.
- 리플레이 로그는 raw 관측, production 필터 관측, 트래커 출력, 퇴실 근거,
  정적 레이더 입력과 융합 상태를 구분한다.
- BLE nearby와 수면 시간 설정은 재실 획득 근거가 아니다.
- 트랙 정책 상수와 LD2410C 거리 일치는 다양한 실제 설치 환경에서 계속 검증해야
  한다.

향후 실험은 [presence-tracker-future.ko.md](presence-tracker-future.ko.md)에 둔다.
