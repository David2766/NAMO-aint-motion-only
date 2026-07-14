# NAMO

**NAMO Ain't Motion-Only.**<br>
*Still here, even when you're still.*

A fully local mmWave spatial presence platform with live floorplan tracking, visual zones, and Home Assistant integration.

![NAMO zone-based lighting automation demo](docs/img/0.webp)
*As I move between detection zones, NAMO automatically switches the corresponding room lights using fully local presence detection.*

## English

NAMO is a fully local mmWave spatial presence platform with live floorplan tracking, visual zones, and Home Assistant integration.

It provides open-source ESPHome firmware for an XIAO ESP32S3 and LD2450-based presence sensor, external components, and a web dashboard embedded directly into the ESP32 firmware.

### What's Included

- ESPHome firmware entry file: `presence-sensor-xiao-s3.yaml`
- Shared ESPHome package: `packages/presence-sensor-xiao-s3/`
- ESPHome external component: `components/radar_api_server/`
- Embedded web dashboard source: `dashboard/`
- Release binaries: `release-assets/`

### Demo

A browser-only demo is available so you can try the embedded dashboard UI without an ESP32 device.

- Dashboard demo: https://david2766.github.io/NAMO-aint-motion-only/
- Initial Wi-Fi setup demo: https://david2766.github.io/NAMO-aint-motion-only/?setup=1

The demo uses mock data. It does not change real device settings, save Wi-Fi credentials, or perform firmware updates.

To run the dashboard locally:

```powershell
cd dashboard
npm ci
npm run dev:web
```

Then open one of the following URLs:

```text
http://localhost:5173/dashboard/?demo=1
http://localhost:5173/dashboard/?setup=1
```

### Project Status

NAMO v0.5.4 is currently a public beta.

The LD2450 is the primary presence and spatial tracking sensor. NAMO is designed to retain presence more reliably than motion-only sensors, but perfectly motionless presence detection is not guaranteed in every room or installation. Detection performance depends on placement, range, orientation, and the surrounding environment.

### Quick Start

Most users should use the prebuilt release binaries instead of building the firmware manually.

1. Download the firmware files from `release-assets/v<version>/` or from GitHub Releases.
2. For the first installation, follow the USB flashing instructions below and install the `factory` image.
3. For updating an already installed device, use the `ota` image.
4. After boot, connect to the device setup AP.
5. Open `http://192.168.4.1/setup` in a browser.
6. Select your Wi-Fi network and enter the password.
7. Copy the ESPHome API key shown on the setup screen.
8. After setup is complete, choose your integration mode from the dashboard.

The setup AP password is:

```text
psensor7777
```

The setup AP is a temporary network used only for initial provisioning. After Wi-Fi setup is complete, the device runs on your home Wi-Fi network.

### First USB Flash

1. Download `presence-sensor-xiao-s3-v<version>-factory.bin` from GitHub Releases.
2. Connect the XIAO ESP32S3 to your computer with a data-capable USB cable.
3. Open [ESPHome Web](https://web.esphome.io/) in Chrome or Edge.
4. Click **Connect** and select the XIAO serial port.
5. Click **Install** and select the downloaded `factory` binary.
6. Wait for installation and reboot to complete.
7. Connect to the device setup AP and continue at `http://192.168.4.1/setup`.

Use the `ota` binary only for updates from the installed device's web dashboard. Do not use the `factory` binary for OTA updates.

### Hardware

NAMO requires a Seeed Studio XIAO ESP32S3 with 8MB Flash / 8MB PSRAM and an LD2450-series radar. The PIR, light, and environmental sensors are optional.

See the [hardware guide](hardware/README.md) for the complete parts list, pinout, wiring notes, PCB manufacturing files, enclosure models, and DIY build instructions.

### Device Name

The firmware uses ESPHome's `name_add_mac_suffix`.

This prevents name conflicts when flashing the same firmware to multiple devices. Devices will be registered with names similar to:

```text
presence-sensor-aabbcc
Presence Sensor AABBCC
```

The last six characters are generated from the last three bytes of the ESP32 Wi-Fi MAC address.

### Initial Setup

This firmware does not ship with a preconfigured Wi-Fi SSID or password.

Initial setup is done through the embedded `/setup` page. The user selects a Wi-Fi network and enters the password, then the device connects to that network and becomes available through the web dashboard.

The ESPHome API encryption key is generated automatically during the initial Wi-Fi setup flow. Copy the key shown on the screen and use it when adding the device to Home Assistant.

If you only use the SmartThings Edge Driver, you still need to complete the initial setup handoff once. The dashboard will ask you to choose the intended integration mode.

### Build

To build the firmware manually, validate and compile the ESPHome configuration from the repository root:

```powershell
esphome config presence-sensor-xiao-s3.yaml
esphome compile presence-sensor-xiao-s3.yaml
```

To generate the embedded web dashboard asset first:

```powershell
cd dashboard
npm install
npm run build:dashboard
```

The dashboard build generates:

```text
components/radar_api_server/dashboard_assets.h
```

To run dashboard tests from a downloaded source archive:

```powershell
cd dashboard
npm test
```

The native PresenceTracker test requires `cl`, `g++`, or `clang++`. The normal
`npm test` command reports that test as skipped when no host C++ compiler is
installed and continues with the remaining tests. Use `npm run test:native` to
require the native test and fail when a compiler is unavailable.

### Release Files

Release binaries are included under:

```text
release-assets/v<firmware-version>/
```

The files are split by purpose:

- `factory`: use this for the first USB flash
- `ota`: use this for updating an already installed device from the web dashboard

Use `checksums.txt` to verify downloaded file integrity.

### Web Dashboard

After flashing the device, open the dashboard in a browser:

```text
http://<device-ip>/dashboard
```

Main dashboard features:

- Real-time presence and motion status
- Radar target map
- Floorplan setup and editing
- Detection zones and false-positive correction zones
- Backup and restore
- Firmware update UI
- System information
- Home Assistant / SmartThings Edge mode selection
- Statistics and heatmap views

### Core Workflow

#### 1. Dashboard

View real-time presence, motion, and target status together with room-level presence and system information in one place.

![NAMO dashboard](docs/img/01.dashboard.webp)

#### 2. Radar Map

Track the position and movement of up to three LD2450 targets in real time using the radar's own coordinate system.

![NAMO radar map](docs/img/02.radar_map.webp)

#### 3. Floorplan Onboarding

Upload a floorplan, set its physical size and scale, analyze wall and room candidates, and place the radar through a guided workflow.

![NAMO floorplan onboarding](docs/img/03.floorplan_onboarding.webp)

#### 4. Room Editing

Refine automatically detected rooms with vertex editing, split and merge operations, or manually created room boundaries.

![NAMO room editing](docs/img/04.floorplan_room_edit.webp)

#### 5. Radar Position & Zone Editing

Match the radar's position and rotation to the floorplan, then edit detection, exclusion, correction, and exit areas while viewing the actual room layout.

![NAMO radar position and zone editing](docs/img/05.radar_position_edit%26edit_zone.webp)

#### 6. Furniture Layout

Select a room, add furniture, and move, rotate, or resize each object so the floorplan reflects the real space more closely.

![NAMO furniture layout](docs/img/06.furniture_layout.webp)

### Security Notes

Each device should use its own ESPHome API encryption key.

During initial setup, this firmware detects the default demo key, generates a new API key, and shows it to the user for copying.

Do not publish firmware images built with your personal settings. Before sharing a ROM file, make sure it does not contain your real Wi-Fi credentials or private API key.

The embedded dashboard is designed for local-network use. Avoid exposing the device directly to the public internet.

### Documentation

- [Documentation index](docs/README.md)
- [HTTP API contract](docs/api-contract.md)
- [Multi-sensor API design](docs/multi-sensor-contract.md)
- [Exit-zone design](docs/presence-exit-zone.md)
- [Presence tracker](docs/presence-tracker.md)
- [Future tracker work](docs/presence-tracker-future.md)
- [Replay and validation plan](docs/presence-replay.md)
- [Presence simulation plan](docs/presence-simulation.md)

### Related Repositories

This project works together with the following repositories:

- NAMO firmware / embedded web dashboard: https://github.com/David2766/NAMO-aint-motion-only
- Home Assistant custom card: https://github.com/David2766/radar-zone-card-for-LD2450
- SmartThings Edge Driver: https://github.com/David2766/presence-sensor-smartthings-edge

### License

Copyright (c) 2026 David2766 and contributors.

The software and firmware are licensed under AGPL-3.0-or-later.

The hardware design and manufacturing files under [`hardware/`](hardware/) are licensed separately under [CC BY-NC-SA 4.0](hardware/LICENSE).

Third-party components retain their respective licenses. See [Third-Party Notices](THIRD_PARTY_NOTICES.md) and [Third-Party License Texts](THIRD_PARTY_LICENSES.txt).

### Notes

This project was planned and tested by the author, with OpenAI Codex used for code generation and refactoring throughout development.

---

## 한국어

NAMO는 실시간 평면도 추적, 시각적 감지 구역과 Home Assistant 연동을 제공하는 완전 로컬 방식의 mmWave 공간 재실 플랫폼입니다.

이 저장소에는 XIAO ESP32S3와 LD2450 기반 재실 센서용 오픈소스 ESPHome 펌웨어, 외부 컴포넌트, 그리고 ESP32 펌웨어에 직접 내장되는 웹 대시보드 소스가 포함되어 있습니다.

### 포함된 것

- ESPHome 펌웨어 진입 파일: `presence-sensor-xiao-s3.yaml`
- ESPHome 공통 패키지: `packages/presence-sensor-xiao-s3/`
- ESPHome 외부 컴포넌트: `components/radar_api_server/`
- 내장 웹 대시보드 소스: `dashboard/`
- 릴리즈 바이너리: `release-assets/`

### 데모 웹 페이지

실제 ESP32 기기 없이 내장 웹 대시보드 화면을 확인할 수 있는 데모 페이지를 제공합니다.

- 데모 주소: https://david2766.github.io/NAMO-aint-motion-only/
- 초기 Wi-Fi 설정 화면 데모: https://david2766.github.io/NAMO-aint-motion-only/?setup=1

데모에서는 데모 데이터를 사용하며 실제 기기 설정, Wi-Fi 저장, 펌웨어 업데이트는 수행하지 않습니다.

로컬에서 확인하려면 아래 명령을 사용합니다.

```powershell
cd dashboard
npm ci
npm run dev:web
```

그 다음 브라우저에서 아래 주소로 접속합니다.

```text
http://localhost:5173/dashboard/?demo=1
http://localhost:5173/dashboard/?setup=1
```

### 프로젝트 상태

NAMO v0.5.4는 현재 공개 베타 단계입니다.

LD2450은 재실 및 공간 추적에 사용하는 핵심 센서입니다. NAMO는 움직임만 감지하는 센서보다 재실 상태를 안정적으로 유지하도록 설계되었지만, 모든 방과 설치 환경에서 완전히 움직이지 않는 사람의 감지를 보장하지는 않습니다. 감지 성능은 센서 배치, 거리, 방향 및 주변 환경의 영향을 받습니다.

### 빠른 시작

일반 사용자는 직접 빌드하지 않고 릴리즈 바이너리를 사용하는 것을 권장합니다.

1. `release-assets/v<version>/` 또는 GitHub Releases에서 펌웨어 파일을 받습니다.
2. 처음 설치할 때는 아래 USB 플래싱 절차에 따라 `factory` 이미지를 설치합니다.
3. 이미 설치된 기기를 업데이트할 때는 `ota` 이미지를 사용합니다.
4. 부팅 후 기기의 설정 AP에 접속합니다.
5. 브라우저에서 `http://192.168.4.1/setup`으로 접속합니다.
6. 사용할 Wi-Fi를 선택하고 비밀번호를 입력합니다.
7. 화면에 표시되는 ESPHome API 키를 복사합니다.
8. 설정 완료 후 대시보드에서 사용 환경을 선택합니다.

설정 AP 비밀번호는 아래와 같습니다.

```text
psensor7777
```

설정 AP는 초기 설정을 위한 임시 네트워크입니다. Wi-Fi 설정이 완료되면 기기는 집 Wi-Fi로 동작합니다.

### 최초 USB 플래싱

1. GitHub Releases에서 `presence-sensor-xiao-s3-v<version>-factory.bin`을 받습니다.
2. 데이터 전송이 가능한 USB 케이블로 XIAO ESP32S3를 컴퓨터에 연결합니다.
3. Chrome 또는 Edge에서 [ESPHome Web](https://web.esphome.io/)을 엽니다.
4. **Connect**를 누르고 XIAO의 시리얼 포트를 선택합니다.
5. **Install**을 누르고 다운로드한 `factory` 바이너리를 선택합니다.
6. 설치와 재부팅이 끝날 때까지 기다립니다.
7. 기기의 설정 AP에 연결한 뒤 `http://192.168.4.1/setup`에서 설정을 계속합니다.

`ota` 바이너리는 설치된 기기의 웹 대시보드에서 업데이트할 때만 사용합니다. OTA 업데이트에 `factory` 바이너리를 사용하지 마세요.

### 하드웨어

NAMO에는 8MB Flash / 8MB PSRAM 사양의 Seeed Studio XIAO ESP32S3와 LD2450 계열 레이더가 필요합니다. PIR, 조도 및 온습도 센서는 선택 사항입니다.

전체 부품 목록, 핀맵, 배선 주의사항, PCB 제작 파일, 케이스 모델 및 자작 안내는 [하드웨어 가이드](hardware/README.ko.md)를 참고하세요.

### 기기 이름

ESPHome의 `name_add_mac_suffix`를 사용합니다.

따라서 같은 펌웨어를 여러 기기에 넣어도 이름 충돌이 나지 않습니다. 기기는 대략 아래와 같은 이름으로 등록됩니다.

```text
presence-sensor-aabbcc
Presence Sensor AABBCC
```

뒤의 6자리는 ESP32 Wi-Fi MAC 주소의 마지막 3바이트를 기준으로 생성됩니다.

### 초기 설정

이 펌웨어는 Wi-Fi SSID와 비밀번호를 펌웨어에 미리 넣지 않습니다.

초기 설정은 기기 안에 내장된 `/setup` 화면에서 진행합니다. 사용자가 Wi-Fi를 선택하고 비밀번호를 입력하면 기기가 해당 네트워크에 연결되고, 이후 웹 대시보드에서 사용할 수 있습니다.

ESPHome API 보안 키는 초기 Wi-Fi 설정 과정에서 자동으로 생성됩니다. 화면에 표시되는 키를 복사해 두었다가 Home Assistant에 기기를 추가할 때 사용합니다.

SmartThings Edge Driver만 사용하는 경우에도 초기 설정 마무리 과정은 한 번 진행해야 합니다. 대시보드에서 사용 환경을 선택하면 됩니다.

### 빌드

직접 빌드하려면 저장소 루트에서 ESPHome으로 설정을 확인하고 빌드합니다.

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

다운로드한 소스 압축 파일에서 대시보드 테스트를 실행하려면 다음 명령을 사용합니다.

```powershell
cd dashboard
npm test
```

네이티브 PresenceTracker 테스트에는 `cl`, `g++`, `clang++` 중 하나가 필요합니다.
일반 `npm test`는 호스트 C++ 컴파일러가 없으면 해당 테스트를 건너뛴 것으로
표시하고 나머지 테스트를 계속 실행합니다. 네이티브 테스트를 필수로 검사하려면
`npm run test:native`를 사용하며, 이 명령은 컴파일러가 없을 때 실패합니다.

### 릴리즈 파일

릴리즈용 바이너리는 아래 폴더에 포함됩니다.

```text
release-assets/v<firmware-version>/
```

파일 이름은 용도에 따라 나뉩니다.

- `factory`: 처음 USB로 플래시할 때 사용
- `ota`: 이미 설치된 기기를 웹 대시보드에서 업데이트할 때 사용

`checksums.txt`로 다운로드한 파일의 무결성을 확인할 수 있습니다.

### 웹 대시보드

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
- Home Assistant / SmartThings Edge 사용 모드 선택
- 통계 및 히트맵 보기

### 핵심 사용 흐름

#### 1. 대시보드

실시간 재실·움직임·타깃 상태와 방별 재실 상태 및 시스템 정보를 한 화면에서 확인합니다.

![NAMO 대시보드](docs/img/01.dashboard.webp)

#### 2. 레이더맵

최대 3개 LD2450 타깃의 위치와 움직임을 레이더 좌표계에서 실시간으로 확인합니다.

![NAMO 레이더맵](docs/img/02.radar_map.webp)

#### 3. 평면도 온보딩

평면도를 업로드하고 실제 크기와 축척을 설정한 뒤, 벽과 방 후보 분석 및 레이더 초기 배치를 단계별로 진행합니다.

![NAMO 평면도 온보딩](docs/img/03.floorplan_onboarding.webp)

#### 4. 방 편집

자동 인식된 방을 꼭짓점 편집, 분할, 병합으로 다듬거나 방 경계를 직접 생성할 수 있습니다.

![NAMO 방 편집](docs/img/04.floorplan_room_edit.webp)

#### 5. 레이더 위치 및 구역 편집

평면도 위에서 레이더 위치와 회전을 맞추고, 실제 공간을 보면서 감지 구역, 제외·오탐 보정 구역 및 퇴실 지점을 편집합니다.

![NAMO 레이더 위치 및 구역 편집](docs/img/05.radar_position_edit%26edit_zone.webp)

#### 6. 가구 배치

방을 선택해 가구를 추가하고 이동, 회전, 크기 조절을 통해 평면도를 실제 공간과 비슷하게 구성합니다.

![NAMO 가구 배치](docs/img/06.furniture_layout.webp)

### 보안 주의

ESPHome API 보안 키는 기기마다 고유하게 사용하는 것을 권장합니다.

이 펌웨어는 초기 설정 과정에서 기본 데모 키를 감지하면 새 API 키를 자동 생성하고, 사용자가 복사할 수 있도록 안내합니다.

개인 설정을 넣어 직접 빌드한 펌웨어 이미지를 공개하지 마세요. ROM 파일을 공유하고 싶다면 실제 사용 중인 Wi-Fi 정보나 개인 API 키가 포함되지 않았는지 확인해야 합니다.

내장 대시보드는 로컬 네트워크 사용을 전제로 합니다. 기기를 인터넷에 직접 노출하지 않는 것을 권장합니다.

### 관련 문서

- [문서 인덱스](docs/README.ko.md)
- [HTTP API 계약](docs/api-contract.ko.md)
- [다중 센서 API 설계](docs/multi-sensor-contract.ko.md)
- [퇴실 지점 설계](docs/presence-exit-zone.ko.md)
- [재실 트래커](docs/presence-tracker.ko.md)
- [향후 트래커 작업](docs/presence-tracker-future.ko.md)
- [리플레이 및 검증 계획](docs/presence-replay.ko.md)
- [재실 시뮬레이션 계획](docs/presence-simulation.ko.md)

### 관련 저장소

이 프로젝트는 아래 저장소들과 함께 동작합니다.

- NAMO 펌웨어 / 내장 웹 대시보드: https://github.com/David2766/NAMO-aint-motion-only
- Home Assistant 커스텀 카드: https://github.com/David2766/radar-zone-card-for-LD2450
- SmartThings Edge Driver: https://github.com/David2766/presence-sensor-smartthings-edge

### 라이선스

Copyright (c) 2026 David2766 and contributors.

소프트웨어와 펌웨어는 AGPL-3.0-or-later 라이선스로 배포됩니다.

[`hardware/`](hardware/) 아래의 하드웨어 설계 및 제작 파일은 별도의 [CC BY-NC-SA 4.0](hardware/LICENSE) 라이선스로 배포됩니다.

제3자 구성요소에는 각 구성요소의 라이선스가 적용됩니다. 자세한 내용은 [제3자 고지](THIRD_PARTY_NOTICES.md)와 [제3자 라이선스 전문](THIRD_PARTY_LICENSES.txt)을 참고하세요.

### 기타

이 프로젝트는 제작자가 직접 기획하고 테스트하면서, 코드 작성과 리팩토링 과정에 OpenAI Codex를 활용해 개발했습니다.
