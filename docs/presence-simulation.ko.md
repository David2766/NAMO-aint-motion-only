# 재실 시뮬레이션 계획

[presence-replay.ko.md](presence-replay.ko.md)에서 분리한 자연스러운 움직임 및 Monte Carlo 시뮬레이션 메모입니다.

## 1. 자연스러운 시뮬레이션

랜덤 테스트는 target 좌표를 아무 곳으로 튀기는 방식이면 안 됩니다.
먼저 자연스러운 사람 움직임을 만들고, 그 위에 센서 노이즈와 dropout을 얹습니다.

### 1.1 시나리오 모델

시나리오는 사람이 읽을 수 있는 상태들의 시퀀스입니다.

예시:

```json
{
  "name": "desk_to_bathroom_and_back",
  "durationSeconds": 600,
  "room": {"widthMm": 3600, "depthMm": 5200},
  "segments": [
    {"state": "enter", "from": [0, 4800], "to": [1500, 2600], "durationSeconds": 8},
    {"state": "sit", "at": [1500, 2600], "durationSeconds": 240},
    {"state": "walk", "from": [1500, 2600], "to": [3000, 4600], "durationSeconds": 10},
    {"state": "away", "durationSeconds": 60},
    {"state": "walk", "from": [3000, 4600], "to": [1500, 2600], "durationSeconds": 10},
    {"state": "sit", "at": [1500, 2600], "durationSeconds": 240}
  ]
}
```

시뮬레이터는 이 시나리오에서 부드러운 truth trajectory를 만듭니다.

### 1.2 자연스러운 움직임 규칙

움직임은 제한된 규칙을 따릅니다.

- 걷기는 속도와 가속도 제한이 있는 연속 경로를 사용합니다.
- 앉아있기는 작은 몸 흔들림과 가끔 손/상체 움직임을 포함합니다.
- 수면은 아주 작은 drift와 긴 정지 구간을 포함합니다.
- 식사는 한 위치 주변의 반복적인 작은 움직임을 포함합니다.
- 방을 나가면 exit boundary를 통과한 뒤 target이 사라집니다.
- 다시 들어오면 내부 랜덤 좌표가 아니라 entrance boundary에서 시작합니다.

간단한 모델 후보:

- 가감속이 들어간 piecewise linear path
- 몸 흔들림용 correlated noise
- 앉음/수면 posture drift용 저주파 random walk
- 식사/책상 작업용 짧은 movement burst

피해야 할 것:

- target을 관계없는 좌표로 순간이동시키기
- 매 프레임 독립적인 랜덤 x/y 생성
- 특정 glitch 테스트가 아닌데 speed spike를 무작위로 넣기

### 1.3 센서 모델

truth motion을 만든 뒤 LD2450과 비슷한 측정값으로 변환합니다.

적용할 항목:

- 거리에 따른 위치 노이즈
- 짧은 random dropout
- target slot swap
- 가끔 duplicate target report
- 6-8m range gate 주변 동작
- 반사 환경의 ghost target 삽입
- PIR 지연, hold, miss

이것들은 사람 움직임이 아니라 측정 효과입니다.

### 1.4 Monte Carlo

Monte Carlo는 같은 시나리오를 다른 random seed로 여러 번 돌리는 방식입니다.

예시:

```text
scenario: seated_10min
runs: 200
position_noise_mm: 100
dropout_probability: 0.03
max_dropout_frames: 6
ghost_probability: 0.01
```

출력은 평균, percentile, worst case를 보여줍니다.

예시:

```text
runs: 200
false_off_seconds_avg: 0.4
false_off_seconds_p95: 2.0
false_off_seconds_worst: 6.5
presence_fragment_count_avg: 0.2
motion_flip_count_avg: 4.8
```
