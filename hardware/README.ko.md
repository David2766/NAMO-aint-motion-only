# NAMO 하드웨어

[English](README.md)

이 폴더에는 NAMO의 기준 하드웨어 정보와 제작 파일이 들어 있습니다.

## 제작 사진

| 1. PCB | 2. BH1750FVI 조도 센서 |
| --- | --- |
| ![NAMO PCB](../docs/img/hardware/1.jpg) | ![NAMO BH1750FVI 조도 센서](../docs/img/hardware/2.jpg) |
| 3. FPC-SHT40 온습도 센서 | 4. LD2450 레이더 |
| ![NAMO FPC-SHT40 온습도 센서](../docs/img/hardware/3.jpg) | ![NAMO LD2450 레이더](../docs/img/hardware/4.jpg) |
| 5. 조립 완료 후 후면을 닫기 전 상태 | 6. 최종 조립 상태 |
| ![NAMO 조립 완료 후 후면을 닫기 전 상태](../docs/img/hardware/5.jpg) | ![NAMO 최종 조립 상태](../docs/img/hardware/6.jpg) |

## 지원 하드웨어

| 하드웨어 | 필수 여부 | 설명 |
| --- | ---: | --- |
| Seeed Studio XIAO ESP32S3, 8MB Flash / 8MB PSRAM | 필수 | 현재 파티션 및 메모리 구성에 필요 |
| LD2450 계열 mmWave 레이더 | 필수 | 재실 및 공간 추적에 사용하는 핵심 센서 |
| Panasonic EKMC1603111 PIR 센서 | 선택 | GPIO1 사용; 생략할 경우 입력이 전기적으로 안정되어야 함 |
| BH1750 조도 센서 | 선택 | I2C 버스 공유 |
| SHT4x 온습도 센서 | 선택 | I2C 버스 공유 |

이 펌웨어는 8MB Flash / 8MB PSRAM 모델과 커스텀 파티션 구성을 기준으로 합니다. Flash 용량이 작거나 PSRAM이 없는 ESP32-S3 보드, 또는 ESP32-C6 같은 다른 칩 계열에서는 그대로 동작하지 않을 수 있습니다.

## 핀 연결

기준 펌웨어와 PCB는 Seeed Studio XIAO ESP32S3의 아래 핀 배열을 사용합니다.

| 기능 | XIAO 표기 | ESP32 GPIO | 연결 대상 |
| --- | --- | --- | --- |
| PIR 입력 | D0 | GPIO1 | PIR 센서 OUT |
| I2C SDA | D4 | GPIO5 | BH1750 SDA, SHT4x SDA |
| I2C SCL | D5 | GPIO6 | BH1750 SCL, SHT4x SCL |
| LD2450 UART TX | D6 | GPIO43 | LD2450 RX |
| LD2450 UART RX | D7 | GPIO44 | LD2450 TX |
| 상태 LED | 내장 LED | GPIO21 | XIAO 내장 사용자 LED |
| 초기화 버튼 | BOOT | GPIO0 | XIAO BOOT 버튼 |

## 배선 주의사항

LD2450의 TX/RX는 XIAO와 교차 연결합니다. XIAO의 TX는 LD2450의 RX로, XIAO의 RX는 LD2450의 TX로 연결합니다.

BH1750과 SHT4x는 같은 I2C 버스를 공유합니다. 각 모듈의 동작 전압을 확인한 뒤 전원을 연결하고, 모든 모듈의 GND를 XIAO의 GND와 공통으로 묶어야 합니다.

LD2450은 재실, 타깃 추적 및 구역 감지에 반드시 필요합니다. LD2450이 없어도 펌웨어가 부팅될 수는 있지만 해당 기능은 정상적으로 동작하지 않습니다.

BH1750, SHT4x 및 PIR 센서는 생략할 수 있습니다. 이 경우 해당 센서 값이 표시되지 않고 로그에 경고가 남을 수 있습니다. PIR 센서를 생략한다면 GPIO1 입력이 떠서 오탐이 발생하지 않도록 회로에서 안정적으로 처리하거나 YAML의 PIR 관련 설정을 제거하세요.

## PCB 제작 파일

- [Gerber 압축 파일](pcb/Gerber.zip)
- [회로도](pcb/schematic.png)

주문하기 전에 제작 압축 파일과 보드 요구사항을 직접 확인하세요. Gerber 파일은 이 문서의 핀맵에 맞춘 기준 제작 출력물입니다.

## 케이스 모델

- [상단 케이스](enclosure/top.3mf)
- [하단 케이스](enclosure/bottom.3mf)

3MF 파일을 슬라이서에서 열고 크기, 출력 방향, 서포트 및 재료 설정을 확인한 뒤 출력하세요. 프린터 보정 상태와 재료 수축에 따라 최종 결합 상태가 달라질 수 있습니다.

## 자작 구성 참고

동일한 XIAO ESP32S3 보드와 위 핀맵을 사용하면 호환 기기를 직접 제작할 수 있습니다. 전원을 넣기 전에 각 모듈의 전원 전압과 핀 방향을 반드시 확인하세요.

PCB와 케이스 파일은 위에 명시된 펌웨어 핀맵을 기준으로 사용해야 합니다. 회로나 커넥터 배치가 바뀌면 펌웨어 설정과 이 문서를 함께 갱신해야 합니다.

## 라이선스

Copyright (c) 2026 David2766.

이 디렉터리의 하드웨어 설계 및 제작 파일은 [Creative Commons 저작자표시-비영리-동일조건변경허락 4.0 국제 라이선스](LICENSE)(`CC BY-NC-SA 4.0`)로 배포됩니다. 상업적 사용에는 프로젝트 제작자의 별도 허가가 필요합니다.
