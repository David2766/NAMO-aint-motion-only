# Presence Sensor XIAO S3

XIAO ESP32S3 기반 mmWave 재실 센서용 ESPHome 펌웨어와 내장 웹 대시보드입니다.

이 저장소에는 기기 펌웨어 설정, ESPHome 외부 컴포넌트, 그리고 ESP32 안에 내장되는 웹 대시보드 소스가 포함되어 있습니다.

## 포함된 것

- ESPHome 펌웨어 진입 파일: `presence-sensor-xiao-s3.yaml`
- ESPHome 공통 패키지: `packages/presence-sensor-xiao-s3/`
- ESPHome 외부 컴포넌트: `components/radar_api_server/`
- 내장 웹 대시보드 소스: `dashboard/`
- 시크릿 예시 파일: `secrets.example.yaml`

Home Assistant 커스텀 카드는 별도 저장소에서 관리합니다.

https://github.com/David2766/radar-zone-card-for-LD2450

## 대상 하드웨어

- Seeed Studio XIAO ESP32S3
- 8MB Flash / 8MB PSRAM 모델
- LD2450 계열 mmWave 레이더 모듈
- PIR 센서
- BH1750 조도 센서
- SHT4x 온습도 센서

## 핀 연결

이 펌웨어는 Seeed Studio XIAO ESP32S3의 아래 핀 배열을 기준으로 작성되어 있습니다.

| 기능 | XIAO 표기 | ESP32 GPIO | 연결 대상 |
| --- | --- | --- | --- |
| PIR 입력 | D0 | GPIO1 | PIR 센서 OUT |
| I2C SDA | D4 | GPIO5 | BH1750 SDA, SHT4x SDA |
| I2C SCL | D5 | GPIO6 | BH1750 SCL, SHT4x SCL |
| LD2450 UART TX | D6 | GPIO43 | LD2450 RX |
| LD2450 UART RX | D7 | GPIO44 | LD2450 TX |
| 상태 LED | 내장 LED | GPIO21 | XIAO 내장 사용자 LED |
| 초기화 버튼 | BOOT | GPIO0 | XIAO BOOT 버튼 |

LD2450의 TX/RX는 XIAO와 교차 연결합니다. 즉, XIAO의 TX는 LD2450의 RX로, XIAO의 RX는 LD2450의 TX로 연결합니다.

BH1750과 SHT4x는 같은 I2C 버스를 공유합니다. 전원은 각 모듈의 동작 전압을 확인한 뒤 연결하고, 모든 모듈의 GND는 XIAO의 GND와 공통으로 묶어야 합니다.

## 자작 구성 참고

같은 Seeed Studio XIAO ESP32S3 보드라면 위 핀맵을 맞춰 자작 구성이 가능합니다.

LD2450은 재실 감지의 핵심 센서이므로 반드시 연결해야 합니다. LD2450이 없으면 펌웨어는 올라가더라도 재실, 타깃, 구역 감지 기능이 정상적으로 동작하지 않습니다.

BH1750, SHT4x, PIR 센서는 구성에 따라 생략할 수 있지만, 해당 센서 값이 표시되지 않거나 로그에 경고가 남을 수 있습니다. PIR을 사용하지 않는 경우에는 GPIO1 입력이 떠서 오탐이 생기지 않도록 회로에서 안정적으로 처리하거나, YAML에서 PIR 관련 설정을 제거하는 편이 좋습니다.

이 펌웨어는 8MB Flash / 8MB PSRAM 모델과 커스텀 파티션 구성을 기준으로 합니다. Flash 용량이 작거나 PSRAM이 없는 ESP32-S3 보드, 또는 ESP32-C6 같은 다른 칩 계열에서는 그대로 동작하지 않을 수 있습니다.

## 기기 이름

배포용 펌웨어는 ESPHome의 `name_add_mac_suffix`를 사용합니다.

따라서 같은 펌웨어를 여러 기기에 넣어도 이름 충돌이 나지 않습니다. 기기는 대략 아래와 같은 이름으로 등록됩니다.

```text
presence-sensor-aabbcc
Presence Sensor AABBCC
```

뒤의 6자리는 ESP32 Wi-Fi MAC 주소의 마지막 3바이트를 기준으로 생성됩니다.

## 설정

먼저 예시 시크릿 파일을 복사합니다.

```powershell
Copy-Item secrets.example.yaml secrets.yaml
```

그 다음 `secrets.yaml` 값을 채웁니다.

```yaml
wifi_ssid: "YOUR_WIFI"
wifi_password: "YOUR_WIFI_PASSWORD"
api_encryption_key: "YOUR_ESPHOME_API_KEY"
ap_password: "YOUR_FALLBACK_AP_PASSWORD"
```

`secrets.yaml`에는 실제 Wi-Fi 비밀번호와 API 키가 들어가므로 외부에 공유하면 절대 안 됩니다.

## 빌드

저장소 루트에서 ESPHome으로 설정을 확인하고 빌드합니다.

```powershell
esphome config presence-sensor-xiao-s3.yaml
esphome compile presence-sensor-xiao-s3.yaml
```

내장 웹 대시보드 asset을 먼저 생성하려면 아래 명령을 실행합니다.

```powershell
cd dashboard
npm install
npm run build:dashboard
```

대시보드 빌드는 아래 파일을 생성합니다.

```text
components/radar_api_server/dashboard_assets.h
```

## Export / Release Workflow

개발 작업 폴더에서 이 저장소로 export할 때는 아래 스크립트를 사용합니다.

```powershell
node scripts\export-firmware-dashboard.mjs C:\repos\presence-sensor-xiao-s3
```

릴리즈용 빌드 결과물을 모으려면 `--release` 옵션을 사용합니다.

```powershell
node scripts\export-firmware-dashboard.mjs C:\repos\presence-sensor-xiao-s3 --release
```

릴리즈용 바이너리는 아래 폴더에 생성됩니다.

```text
release-assets/v<firmware-version>/
```

## 웹 대시보드

기기 플래싱 후 브라우저에서 아래 주소로 접속합니다.

```text
http://<device-ip>/dashboard
```

웹 대시보드에서 제공하는 주요 기능은 다음과 같습니다.

- 실시간 재실 / 움직임 상태 확인
- 레이더 타깃 맵
- 평면도 설정 및 편집
- 감지 구역 / 오탐 보정 구역 설정
- 백업 및 복원
- 펌웨어 업데이트 UI
- 시스템 정보 확인
- 통계 및 히트맵 보기

## 보안 주의

ESPHome API encryption key는 기기마다 고유하게 사용하는 것을 권장합니다.

개인 `secrets.yaml`로 빌드한 펌웨어 이미지를 공개하지 마세요. ROM 파일을 공유하고 싶다면 별도의 provisioning 정책을 세우고, 실제 사용 중인 API 키나 Wi-Fi 정보가 포함되지 않도록 주의해야 합니다.

## 라이선스

AGPL-3.0-or-later.

## 기타

이 프로젝트는 제작자가 직접 기획하고 테스트하면서, 코드 작성과 리팩토링 과정에 OpenAI Codex를 활용해 개발했습니다.
