# NAMO 펌웨어 서명

영문 문서: [firmware-signing.md](firmware-signing.md)

## 보안 경계

서명 OTA는 서명되지 않았거나 잘못 서명된 펌웨어가 업데이트되는 것을 막습니다. Secure
Boot, Flash Encryption, eFuse, HTTPS, 모바일 앱, Home Assistant를 신뢰 조건으로 두지
않습니다.

| 키 | 보관 | 용도 | 유출 시 복구 |
| --- | --- | --- | --- |
| Root | 오프라인 | 릴리즈 또는 개발자 키 교체 승인 | 유출되면 USB 복구 필요 |
| Release | 릴리즈 호스트 | 공식 릴리즈 패키지 서명 | Root 서명 복구 패키지로 폐기·교체 |
| Developer | 개발 호스트 | 통제된 커스텀 빌드 서명 | Root 서명 복구 패키지로 폐기·교체 |

펌웨어에는 공개키와 키 ID만 들어갑니다. 개인키는 암호화된 PKCS#8 파일로 보관하며 저장소,
빌드 산출물, 펌웨어 이미지, 기기에 넣지 않습니다.

## 패키지와 기기 규칙

- 패키지는 고정 320바이트 서명 envelope 뒤에 정확한 ESP32 OTA 이미지를 붙입니다.
- 서명은 ECDSA P-256, SHA-256, IEEE P1363 `r || s` 형식을 사용합니다.
- 제품, 하드웨어, 이미지 크기, trust revision, security epoch, 서명자, 이미지 digest,
  버전, build ID, 키 복구 필드를 모두 서명합니다.
- 기기는 OTA 파티션을 열기 전에 envelope 서명과 신뢰 상태를 검증합니다.
- 실제 업로드된 envelope는 사전 검증한 envelope와 바이트 단위로 같아야 합니다.
- 기기는 기록된 파티션을 다시 SHA-256으로 읽어 서명된 digest와 일치한 뒤에만 활성화합니다.
- signed 빌드는 ESPHome Native OTA를 포함하지 않고 raw web OTA도 끕니다. 대시보드는 구형
  개발 펌웨어가 `/api/firmware/trust`에 HTTP 404를 반환할 때만 `/update`를 사용합니다.
  500, 503, 잘못된 응답은 절대로 구형 OTA로 간주하지 않습니다.
- 릴리즈 및 수동 서명 도구는 내부 이미지에 signed OTA 구현 marker와 Root, Release,
  Developer 공개키 전체가 없으면 거부합니다. 선언한 역할은 입력한 개인키와 일치해야 합니다.
  Root 복구 서명은 교체 공개키도 복구 이미지에 포함되어 있지 않으면 거부합니다.
- Trust Store는 NVS A/B 슬롯과 commit record를 사용합니다. commit된 trust가 깨지면
  이전의 폐기된 키를 되살리지 않고 OTA를 닫습니다.
- trust 변경은 비활성 NVS 슬롯에 먼저 stage하고, OTA backend가 검증된 image를 활성화한 뒤에만
  commit합니다. commit 실패 시 현재 실행 파티션을 다시 선택합니다. stage 뒤 중단된 업데이트는 이전에
  commit된 trust를 계속 사용합니다. 더 높은 Root trust revision은 유출된 Release 키가 epoch를
  최댓값으로 고정한 경우에도 security epoch를 새 기준으로 복구할 수 있습니다.
- `--release` 빌드는 매번 `firmwareSecurityEpoch`를 정확히 1 올려 저장합니다. 따라서 새 정식
  릴리즈 설치 후에는 이전 정식 패키지를 재생할 수 없습니다. Developer 패키지는 현재 epoch와
  같아야 하며 올릴 수 없습니다.
- signed OTA 작업공간은 PSRAM에만 둡니다. 할당 실패 시 내부 RAM fallback 없이 OTA를 닫습니다.

## 키 생성

저장소 밖의 새 디렉터리에 생성합니다.

```powershell
$keyDirectory = Join-Path $HOME "namo-signing-keys-v1"
$env:NAMO_ROOT_KEY_PASSPHRASE='길고-고유한-root-passphrase'
$env:NAMO_RELEASE_KEY_PASSPHRASE='서로-다른-release-passphrase'
$env:NAMO_DEVELOPER_KEY_PASSPHRASE='서로-다른-developer-passphrase'
node scripts/firmware-keygen.mjs --out $keyDirectory
$env:NAMO_FIRMWARE_KEY_DIRECTORY=$keyDirectory
```

`root.private.pem`은 오프라인에 둡니다. Root 키와 passphrase의 두 번째 오프라인 백업은
서로 분리해 보관합니다. Root 키를 잃으면 원격 키 교체 경로도 잃습니다.

## 빌드 비밀번호 등록

Developer와 Release 개인키 비밀번호는 최초 한 번만 등록합니다.

```powershell
.\scripts\register-firmware-secret.ps1 -Role developer
.\scripts\register-firmware-secret.ps1 -Role release
```

등록기는 `NAMO_FIRMWARE_KEY_DIRECTORY` 또는 `-KeyDirectory`로 지정한 디렉터리에서 키를
읽습니다. 입력한 비밀번호로 개인키를 실제 복호화하고 같은 역할의 공개키와 일치하는지 확인한
뒤에만 해당 디렉터리의 `.secrets`에 Windows DPAPI 암호문을 저장합니다. 틀린
비밀번호는 저장하지도 자동 재시도하지도 않습니다. 기존 캐시 교체는 `-Replace`를 명시해야
하며, 새 비밀번호 검증이 실패하면 기존 정상 캐시를 그대로 유지합니다. DPAPI 캐시는 현재
Windows 사용자에게 묶이므로 PC나 계정을 바꾸면 인쇄해 둔 비밀번호로 다시 등록합니다. Root
비밀번호는 캐시하지 않습니다.

## 공식 릴리즈

릴리즈 빌드는 외부 키를 읽어 검증된 `.namo-ota` 패키지를 만듭니다. 생성되는 C++ 헤더에는
개인키가 들어가지 않습니다.

```powershell
.\scripts\build-firmware.ps1 -Release -Signer release
```

patch 버전만 올리는 핫픽스 릴리즈는 명시적인 별칭을 사용합니다.

```powershell
.\scripts\build-firmware.ps1 -Hotfix -Signer release
```

`-Hotfix`는 patch 버전 증가로 고정됩니다. 일반 Release와 같은 서명 패키지를 만들고 security
epoch를 올리며 `-Release` 또는 `-ReleaseType`과 함께 사용할 수 없습니다. 다음 명령으로
내보냅니다.

```powershell
node .\scripts\export-firmware-dashboard.mjs --hotfix --allow-dirty-target
```

`-Release`는 버전과 security epoch를 올리고, `-Signer release`는 Release 키 사용을
선택합니다. 둘은 별도 옵션이며 하나만 지정하면 빌드 전에 거부합니다. 현재 셸에서
`NAMO_FIRMWARE_KEY_DIRECTORY`를 한 번 설정하거나 `-KeyDirectory`를 명시합니다. 캐시는 한
번만 복호화·검증하며 실패하면 컴파일을 시작하지 않습니다. 빌드가 끝나거나 실패하면 임시
환경변수를 복원합니다.

`firmwareSecurityEpoch`를 낮추지 않습니다. 릴리즈 명령이 자동으로 올리며 빌드 실패 시 이전값을
복원합니다.

릴리즈 업로드 전 비파괴 실기기 사전검증 smoke를 실행합니다.

```powershell
python tools\firmware_update_security_smoke.py `
  --device http://192.168.29.218 `
  --package .esphome\build\presence-sensor\.pioenvs\presence-sensor-build\firmware-release-<version>.namo-ota
```

정상 session 하나를 열고 취소하며, 잘못된 취소 token이 session을 종료하지 못하는지 확인합니다.
또한 header 잘림, 초과 바이트, envelope 320바이트 각각의 1바이트 변조를 모두 검사합니다.
OTA 이미지는 기록하지 않습니다.

`.sha256` 파일은 다운로드 손상을 확인하는 보조 수단입니다. 기기가 검증하는 서명을 대신하지
않습니다.

## 서명된 Developer 빌드

Developer 패키지는 현재 security epoch를 그대로 사용하며 올릴 수 없습니다.

```powershell
.\scripts\build-firmware.ps1 -Signer developer
```

이 명령은 공개 trust 키 전체를 내장하고 raw OTA를 끄며 Developer 서명자 일치를 검증한 뒤
`firmware-developer-<version>.namo-ota`를 만듭니다.

키를 전혀 사용하지 않는 기존 개발 빌드는 다음과 같습니다.

```powershell
.\scripts\build-firmware.ps1 -Signer none
```

래퍼는 세 모드 모두 대시보드 빌드와 clean을 기본 실행합니다. 필요할 때만 `-NoDashboard` 또는
`-NoClean`으로 생략합니다. 내부 컴파일 CLI를 직접 사용할 때의 대응 옵션은
`--signer none`, `--signer developer`, `--release --signer release`,
`--hotfix --signer release`입니다.

## Release 또는 Developer 키 유출 복구

1. 유출된 개인키 사용을 중단합니다.
2. 기존 키와 다른 새 키 쌍을 만듭니다.
3. 새 공개키를 초기 trust로 포함한 OTA 이미지를 만듭니다.
4. `trustRevision`을 올립니다. 구형 펌웨어까지 차단해야 하면
   `firmwareSecurityEpoch`도 올립니다.
5. 오프라인 Root 개인키로 복구 패키지 하나를 서명합니다. 패키지에는 폐기할 키 ID, 새 키
   역할, 새 키 ID, 새 공개키가 모두 들어가야 합니다.
6. 패키지를 오프라인 검증한 뒤 배포하고 재부팅 후 `/api/firmware/trust`에서 새 revision과
   키 ID를 확인합니다.
7. 폐기된 키로 서명한 패키지가 거부되는지 확인합니다.

Trust revision과 security epoch는 낮아지지 않습니다. Root 키가 유출되면 공격자도 Root
서명을 만들 수 있으므로 다른 원격 서명으로 복구할 수 없습니다. 이 경우 통제된 USB 복구로 새
신뢰 기준을 설치합니다.

## 이전 개발 펌웨어에서 전환

아직 공개하지 않은 구형 개발 펌웨어는 기존 raw web OTA로 최초 signed 빌드를 한 번 받을 수
있습니다. 이 bootstrap `.bin`은 로컬에서만 사용하고 일반 릴리즈 산출물로 공개하지 않습니다.
signed 빌드가 부팅된 뒤에는 `.namo-ota`만 허용하며 trust 오류에서 raw OTA로 내려가지 않습니다.

## 릴리즈 Gate

- 호스트의 서명, 전 바이트 변조, 잘못된 패키지, rollback, 키 교체, 저장 중단, chunk 순서,
  timeout, sink 실패 테스트 통과
- 대시보드 signed/legacy 분기와 no-downgrade 테스트 통과
- `tools/firmware_update_security_smoke.py`에서 OTA 기록 없이 envelope 320바이트의 1바이트
  변조 320개가 모두 거부됨
- 공개키를 넣은 펌웨어 컴파일과 raw OTA 경로 부재 확인
- 실기기에서 정상 Release, 정상 Developer, 위조 패키지, 폐기 키, 중단 업로드, 재부팅,
  Root 복구 검증
- 업로드 및 기록 파티션 재검증 중 자원 smoke 통과
