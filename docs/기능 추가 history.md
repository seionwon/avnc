# AVNC 기능 추가 히스토리

이 문서는 AVNC 프로젝트에 추가·변경된 주요 기능의 구현 내용, 사용 방법, 검증 결과 및 현재 배포 상태를 시간순으로 기록합니다.

---

<a id="hist-20261121-001"></a>
### 2026-11-21 11:51:05 +09:00 — macOS 내장 VNC 멀티 디스플레이 전환 및 가상 디스플레이 지원

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-001` |
| 변경 종류 | 기능 추가 (Feature) |
| 상태 | 구현 및 로컬 빌드·테스트 검증 완료 (미배포) |

#### 1. 기능 목적
macOS 내장 화면 공유(VNC) 서버(`screensharingd`)에 여러 대의 외장 모니터가 연결된 환경에서, 모든 모니터가 하나로 합쳐져 보이는 문제를 해결하고 macOS 내장 Screen Sharing 클라이언트의 **고성능 가상 디스플레이(1개/2개)** 및 **특정 디스플레이 선택(`View -> Display 1` 등)** 기능을 AVNC에서도 사용할 수 있도록 구현했습니다.

#### 2. 열기 위치
- **프로필 설정(사전 구성)**: 홈 화면 → 서버 프로필 추가/편집 → **고급(Advanced)** 편집기 → **macOS 디스플레이(`macOS Display`)** / **초기 디스플레이(`Initial display`)**
- **실시간 디스플레이 전환**: macOS 서버 접속 후 뷰어 화면의 **도구 모음(Toolbar)** → **디스플레이 전환(모니터 아이콘)** 버튼 (`서버가 2개 이상의 디스플레이와 전환 명령을 지원할 때만 자동 표시`)
- **URI 실행 파라미터(선택)**: `vnc://host/?AppleDisplayMode=2&AppleInitialDisplay=1`

#### 3. 사용 방법
macOS 내장 VNC 클라이언트의 `"High Performance" -> "Display Type: 2 Virtual Displays" -> "View -> Display 1"` 흐름과 동일하게 다음 순서로 사용합니다.

1. 서버 프로필 편집에서 **고급** 편집기를 엽니다.
2. **보안(Security)**은 `자동(Automatic)` 또는 `macOS`로 두고, macOS **사용자 이름**과 **암호**를 입력합니다.
3. **macOS 디스플레이**에서 원하는 모드를 선택합니다.
   - **표준 (물리 디스플레이)**: Mac에 연결된 실제 물리 모니터 중 하나만 선택해 볼 때 사용
   - **고성능: 가상 디스플레이 1개**: 가상 디스플레이 1개 생성 (`0x1d SetDisplayConfiguration`, `display_count=1`)
   - **고성능: 가상 디스플레이 2개**: 가상 디스플레이 2개 생성 (`0x1d SetDisplayConfiguration`, `display_count=2`)
4. **초기 디스플레이**를 `디스플레이 1`로 지정하면 접속 직후 자동으로 1번 디스플레이로 전환됩니다(`전체 디스플레이`를 두면 합쳐진 화면으로 시작).
5. 접속 후에도 뷰어 도구 모음의 **디스플레이 전환** 버튼을 눌러 **전체 디스플레이 / 디스플레이 1 (해상도) / 디스플레이 2 (해상도)** 사이를 실시간으로 전환할 수 있습니다.

#### 4. 동작 및 오류 처리
- **Apple RFB 3.889 네이티브 엔진 (`app/src/main/cpp/AppleRfb.h`, `AppleRfb.cpp`)**:
  - `MSG_PEEK`로 서버 배너(`RFB 003.889\n` / `003.888\n`)를 비파괴 감지하여 비-macOS VNC 서버에는 추가 TCP 연결이나 부작용 없이 기존 `libvncclient` 경로를 그대로 사용합니다.
  - Apple 계정 인증 **Type 36(Direct SRP)**, **Type 33(RSA1 / RSA-SRP)**, **Type 30(Apple DH/ARD)** 을 지원하며, `ClientInit(0xC1)`, 확장 `ServerInit`, `SessionSelect`, `0x44f(1103)` 재키(Rekey) 및 지속 `AES-128-CBC + SHA-1` 레코드 레이어를 처리합니다.
  - `1105(0x451)` `AppleDisplayLayout`으로 디스플레이 목록·해상도·활성 디스플레이를 수신하고, `0x0d` `SetDisplayMessage`로 실시간 디스플레이 전환을 수행합니다.
  - `Raw`, `CopyRect`, `Zlib`, `ZRLE`, `AppleThousands(1002, RGB555)` 프레임 디코딩, `1104` 알파 채널 커서 캐시, `0x1f` UTF-8 Apple 클립보드, macOS 마우스 가운데/오른쪽 버튼 비트 스왑을 지원합니다.
- **폴백 및 오류 처리**:
  - 서버가 Apple 계정 인증(36/33/30) 없이 VNC 전용 비밀번호(Type 2)만 제공하거나, 사용자가 사용자 이름을 비워둔 경우 자동으로 기존 `libvncclient`로 폴백합니다.
  - 인증 실패, 자격 증명 취소, 네트워크 끊김 시 `errno` 기반 오류 메시지를 기존 AVNC 연결 오류 UI로 전달합니다.
  - Room 데이터베이스는 버전 `7 -> 8` `AutoMigration`(`defaultValue = "0"`)으로 기존 프로필 데이터를 유지합니다.

#### 5. 검증 결과
- **네이티브 C++ 단위·종단간(E2E) 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror`, OpenSSL 3 + zlib):
  - 총 8개 테스트 전부 통과: 와이어 빌더/파서(`0x0d`, `0x1d` 가상 1·2개, `0x451`, 버튼 스왑), `MSG_PEEK` 비파괴 감지, Type 30 DH + 가상 디스플레이 2개, Type 36 Direct SRP + 가상 디스플레이 2개, Type 33 RSA1-Plain + 가상 디스플레이 1개, Type 30 DH + 물리 디스플레이, 단일 소켓 NoAuth 호환(`RFB 003.889`), Type 2 VncAuth 폴백.
  - `Zlib`, `ZRLE`, `AppleThousands(RGB555)`, `CopyRect`, `1104` 커서 STORE/SELECT, `0x1f` 한글 UTF-8 클립보드, 자동 초기 디스플레이 1 선택 및 실시간 디스플레이 2/전체 전환까지 검증 완료.
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest`):
  - `RemoteDisplayTest`(디스플레이 전환 버튼 표시 조건, `ServerProfile` 기본값, 레거시/신규 JSON 직렬화 호환성) 통과.
  - Room 스키마 `app/roomSchema/com.gaurav.avnc.model.db.MainDb/8.json` 자동 생성 및 `7.json` 대비 스키마 diff 검증 완료.
- **Android 계측 테스트 컴파일** (`./gradlew :app:compileDebugAndroidTestKotlin`): 통과.
- **Android NDK + Debug APK 빌드** (`./gradlew :app:assembleDebug -Pandroid.injected.build.abi=arm64-v8a`):
  - Android NDK `28.2.13676358`, CMake `3.22.1`, vcpkg(`libressl 4.2.1`, `libjpeg-turbo 3.2.0`), `-DANDROID_STL=none` 환경에서 `libnative-vnc.so`, `libvncclient.a` 및 `app/build/intermediates/apk/debug/app-debug.apk` 빌드 성공.

#### 6. 실제 배포 상태 및 제한 사항
- **배포 상태**: 로컬 워크스페이스에서 `arm64-v8a` Debug APK 빌드 및 자동화 테스트까지만 완료되었으며, 실제 Android 기기 설치, 실기 macOS `screensharingd` 연동 테스트, 릴리스 서명/배포는 아직 수행되지 않았습니다.
- **제한 사항**:
  - **전송 프로파일**: Apple RFB 3.889의 TCP 프레임버퍼 모드(Profile A: Zlib/ZRLE/AppleThousands over AES-128-CBC)를 사용합니다. UDP 기반 HEVC 하드웨어 스트리밍은 포함되지 않으며, 대신 LAN·VPN·SSH 터널 환경에서도 단일 TCP 포트로 동작합니다.
  - **가상 디스플레이 조건**: 가상 디스플레이(`0x1d`)는 macOS 서버가 이를 지원해야 하며(Apple Silicon Mac, macOS Sonoma 이상), macOS 사용자 계정 로그인(Type 36/33/30)이 필요합니다. VNC 전용 비밀번호(Type 2) 로그인에서는 일반 VNC 모드로 동작합니다.
  - **멀티 ABI 릴리스 빌드**: 이번 검증에서는 빠른 검증을 위해 `arm64-v8a` 단일 ABI로 APK를 빌드했습니다. 전체 ABI(`armeabi-v7a`, `arm64-v8a`, `x86`, `x86_64`) 패키징은 ABI 옵션 없이 `./gradlew :app:assembleDebug` 또는 `assembleRelease`를 실행해야 합니다.

---

<a id="hist-20261121-002"></a>
### 2026-11-21 13:49:30 +09:00 — 실행 화면 APK 다운로드 링크 및 버전 번호(`v3.3.2`) 적용

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-002` |
| 변경 종류 | 기능 추가 및 빌드 개선 (Feature / Build) |
| 상태 | 구현 및 로컬 빌드·브라우저 검증 완료 (미배포) |

#### 1. 기능 목적
CodeMemo에서 프로젝트의 **실행** 버튼을 눌렀을 때 열리는 화면에서 빌드된 Android APK 파일을 버전 번호와 함께 바로 다운로드할 수 있도록 웹 다운로드 화면을 추가하고, Gradle 빌드 출력 APK 파일명과 앱 버전에도 버전 번호(`v3.3.2`, `versionCode 54`)가 반영되도록 구성했습니다.

#### 2. 열기 위치
- **실행 화면(웹 다운로드 포털)**: CodeMemo 프로젝트 **실행** 버튼(또는 로컬 `npm run dev` / `npm run start` → `http://127.0.0.1:4173/`)
- **Gradle 공식 APK 출력 경로**: `app/build/outputs/apk/debug/avnc-v3.3.2-debug.apk`
- **웹 다운로드 정적 파일 경로**: `public/downloads/avnc-v3.3.2-debug.apk`, `dist/downloads/avnc-v3.3.2-debug.apk`

#### 3. 사용 방법
1. CodeMemo에서 **실행** 버튼을 누르면 `AVNC v3.3.2` APK 다운로드 화면이 열립니다.
2. 상단 카드의 **APK 다운로드 (v3.3.2)** 버튼, **직접 다운로드 링크**, 또는 하단 **빌드된 APK 파일 목록**의 `avnc-v3.3.2-debug.apk` 링크를 클릭하면 `application/vnd.android.package-archive` 헤더로 즉시 APK 파일이 다운로드됩니다.
3. 앱 코드를 수정해 새 APK를 빌드한 뒤에는 `./gradlew :app:assembleDebug && npm run build`(또는 `npm run sync:apk`)를 실행하면 `scripts/sync-apk.mjs`가 최신 버전 번호, 파일 크기, ABI 목록, SHA-256 체크섬, 빌드 시각을 자동으로 갱신합니다.

#### 4. 동작 및 오류 처리
- **Gradle 버전 및 파일명 설정 (`app/build.gradle.kts`)**:
  - `versionCode`를 `54`, `versionName`을 `"3.3.2"`로 갱신하고 `base { archivesName.set("avnc-v${android.defaultConfig.versionName}") }`을 설정해 공식 출력 경로 `app/build/outputs/apk/debug/avnc-v3.3.2-debug.apk`로 생성되도록 했습니다.
  - Apple Silicon macOS(Rosetta 미설치 환경)에서 NDK의 `x86_64` `yasm` 실행 오류를 방지하기 위해 기본 ARM ABI(`arm64-v8a`, `armeabi-v7a`)를 빌드하도록 구성했으며, `-PavncAllAbis` 또는 Linux CI에서는 전체 ABI를 빌드합니다.
- **APK 동기화 및 웹 다운로드 화면 (`scripts/sync-apk.mjs`, `index.html`, `src/main.js`, `src/style.css`, `vite.config.js`, `package.json`)**:
  - Tailwind CSS `4.3.3` + `@tailwindcss/vite@4.3.3` 로컬 컴파일 자산과 CSS-first `@theme` 토큰을 사용합니다(CDN 미사용).
  - `base: './'` 상대 경로를 사용해 단독 실행(`http://127.0.0.1:4173/`) 및 서브패스 리버스 프록시 환경 모두에서 APK 링크와 정적 리소스가 정상 동작합니다.
  - `vite.config.js` 미들웨어가 `.apk` 요청에 `Content-Type: application/vnd.android.package-archive` 및 `Content-Disposition: attachment` 헤더를 자동 지정합니다.
  - 빌드된 APK가 없는 경우 경고 상태 배지와 안내 문구를 표시해 깨진 화면을 방지합니다.

#### 5. 검증 결과
- **Gradle APK 빌드 (`./gradlew :app:assembleDebug`)**: `app/build/outputs/apk/debug/avnc-v3.3.2-debug.apk` (`12.50 MB`, `versionCode 54`, `versionName "3.3.2 (debug)"`, ABI: `arm64-v8a`, `armeabi-v7a`) 생성 확인.
- **Android JVM 단위 테스트 (`./gradlew :app:testDebugUnitTest`)**: 통과.
- **APK 동기화 및 웹 프로덕션 빌드 (`npm run build`)**: `public/downloads/avnc-v3.3.2-debug.apk`, `dist/index.html`, 로컬 컴파일 CSS(`dist/assets/index-*.css`), `dist/downloads/avnc-v3.3.2-debug.apk` 생성 확인.
- **브라우저 및 HTTP 다운로드 검증 (`codememo-browser`, `curl -sI`)**:
  - `vite preview` 및 `vite dev` 모두에서 `/downloads/avnc-v3.3.2-debug.apk`가 `HTTP/1.1 200 OK`, `Content-Type: application/vnd.android.package-archive`, `Content-Disposition: attachment; filename="avnc-v3.3.2-debug.apk"`, `Content-Length: 13107805`로 응답함 확인.
  - `codememo-browser` 렌더링 검증 통과(`status: 200`, `errors: []`, 버전 배지 `v3.3.2 · Build 54`, 파일명 `avnc-v3.3.2-debug.apk`, 크기 `12.50 MB`, ABI `arm64-v8a, armeabi-v7a` 표시 및 스크린샷 확인).

#### 6. 실제 배포 상태 및 제한 사항
- **배포 상태**: 로컬 워크스페이스에서 Debug APK 빌드 및 실행 화면 웹 포털 빌드·브라우저 검증까지 완료되었으며, 외부 호스팅이나 앱 스토어 배포는 수행되지 않았습니다.
- **제한 사항**:
  - 현재 Apple Silicon macOS(Rosetta 미설치) 로컬 빌드 APK에는 `arm64-v8a`와 `armeabi-v7a`가 포함됩니다(실제 Android 스마트폰·태블릿 대부분 지원). `x86`/`x86_64` 에뮬레이터용 ABI까지 포함한 4-ABI 빌드는 Linux CI 또는 Rosetta/x86_64 환경에서 `-PavncAllAbis`로 빌드해야 합니다.
  - Debug 서명 키로 서명된 Debug APK(`com.gaurav.avnc.debug`)이므로, 정식 배포용 릴리스 APK를 제공하려면 서명 키를 구성해 `assembleRelease` 후 `npm run build`를 실행해야 합니다.

---

<a id="hist-20261121-003"></a>
### 2026-11-21 15:48:04 +09:00 — FocusTempo 방식에 맞춘 순수 Android 실행 탭 APK 제공(`AVNC-v3.3.2.apk`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-003` |
| 변경 종류 | 빌드 산출물 및 실행 탭 연동 방식 정리 (Build / Refactor) |
| 상태 | 구현 및 로컬 Gradle 테스트·APK 빌드 검증 완료 (미배포) |

#### 1. 기능 목적
CodeMemo의 **실행 탭**이 `AVNC`를 웹 프로젝트가 아닌 `FocusTempo`와 동일한 **순수 Android 프로젝트**로 인식해 내장 실행 화면에 APK 다운로드 링크를 바로 표시하도록 구조를 통일하고, APK 파일명에 앱 이름과 버전 번호(`AVNC-v3.3.2.apk`)가 붙도록 구성했습니다.

#### 2. 열기 위치
- **CodeMemo 실행 탭**: `avnc` 프로젝트의 **실행** 탭(내장 Android 빌드·APK 다운로드 화면)
- **Gradle 디버그 출력 APK**: `app/build/outputs/apk/debug/AVNC-v3.3.2.apk`
- **프로젝트 루트 APK 사본**: `AVNC-v3.3.2.apk`

#### 3. 사용 방법
1. CodeMemo에서 `avnc` 프로젝트의 **실행** 탭을 열면 `FocusTempo`와 동일하게 Android 프로젝트로 인식되어 `AVNC-v3.3.2.apk` 빌드 및 다운로드 링크가 제공됩니다.
2. 터미널이나 실행 탭에서 `./gradlew :app:assembleDebug`를 실행하면 `app/build/outputs/apk/debug/AVNC-v3.3.2.apk` 생성과 동시에 프로젝트 루트의 `AVNC-v3.3.2.apk`도 최신 빌드본으로 자동 갱신됩니다(이전 버전의 루트 `AVNC*.apk` 파일은 자동 정리).

#### 4. 동작 및 변경 내용
- **루트 `build.gradle.kts` 추가 (`build.gradle.kts`, `app/build.gradle.kts`)**:
  - 기존 `avnc`는 프로젝트 루트에 `build.gradle.kts`가 없고 `app/build.gradle.kts`에 플러그인 버전이 직접 선언되어 있어, 루트 `build.gradle.kts` 유무로 Android/Gradle 프로젝트를 판별하는 실행 탭에서 인식되지 않을 수 있었습니다.
  - `FocusTempo/build.gradle.kts`와 동일하게 프로젝트 루트에 `build.gradle.kts`(`id("com.android.application") version "9.2.1" apply false` 등)를 두고 `app/build.gradle.kts`에서 버전 없이 플러그인을 적용하도록 구조를 일치시켰습니다.
- **임시 웹 다운로드 포털 제거**: `FocusTempo`와 동일하게 순수 Android 프로젝트 구조를 유지하기 위해 이전 단계에서 추가한 `package.json`, `package-lock.json`, `vite.config.js`, `index.html`, `src/`, `scripts/`, `public/`, `dist/`, `node_modules/`를 제거하고 `.gitignore`에서 웹 포털 항목을 정리해 `.codememo-build/`만 제외하도록 맞췄습니다.
- **`androidComponents` 기반 APK 파일명 지정 (`app/build.gradle.kts`)**:
  - `FocusTempo`와 동일한 `androidComponents { onVariants(selector().withBuildType("debug")) { ... } }` 방식을 적용해 디버그 산출물 파일명을 `AVNC-v${android.defaultConfig.versionName}.apk` (`AVNC-v3.3.2.apk`)로 설정했습니다.
  - `assembleDebug` 완료 시 `app/build/outputs/apk/debug/AVNC-v3.3.2.apk`를 프로젝트 루트 `AVNC-v3.3.2.apk`로 자동 복사하도록 `doLast` 훅을 추가하고, 이전 중간 산출물(`app/build/intermediates/apk/`)을 정리했습니다.

#### 5. 검증 결과
- `./gradlew :app:testDebugUnitTest :app:assembleDebug`: 통과 (`BUILD SUCCESSFUL`).
- CodeMemo 신뢰 경로 `gradle :app:assembleDebug --no-daemon` (`2026-11-21 16:08:10 +09:00`): 통과 (`BUILD SUCCESSFUL`).
- 산출물 및 메타데이터 확인:
  - 루트 `build.gradle.kts`, `app/build/outputs/apk/debug/AVNC-v3.3.2.apk` (`12.50 MB`), 루트 `AVNC-v3.3.2.apk` 생성 완료.
  - 두 APK 파일의 SHA-256 해시가 `3f949b5cb6ef61f5d1801c2c512202184c75de089719746f24dc2e0f28403333`로 완전히 일치함 확인.
  - `app/build/outputs/apk/debug/output-metadata.json`에 `"outputFile": "AVNC-v3.3.2.apk"`, `"versionName": "3.3.2 (debug)"`, `"versionCode": 54` 기록 확인.

#### 6. 실제 배포 상태 및 제한 사항
- **배포 상태**: 로컬 워크스페이스에서 Gradle 단위 테스트 통과 및 `AVNC-v3.3.2.apk` 생성(Gradle 출력 경로 및 프로젝트 루트)까지 완료되었으며, 실제 기기 설치 및 외부 배포는 수행되지 않았습니다.
- **제한 사항**: Apple Silicon macOS(Rosetta 미설치) 로컬 빌드 환경에서는 `arm64-v8a`, `armeabi-v7a` ABI가 포함됩니다(`-PavncAllAbis` 또는 Linux CI에서 전체 ABI 빌드 가능).

---

<a id="hist-20261121-004"></a>
### 2026-11-21 18:01:33 +09:00 — macOS 한영 입력 동기화, 터치 정확도 개선 및 화면 갱신 멈춤 수정 (`v3.3.3`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-004` |
| 변경 종류 | 버그 수정 및 기능 개선 (Bugfix / Enhancement) |
| 상태 | 구현 및 로컬 네이티브·JVM 테스트·APK 빌드 검증 완료 (미배포) |

#### 1. 기능 목적
macOS 내장 Screen Sharing(`RFB 3.889`) 접속 환경에서 발생한 세 가지 실사용 문제를 한꺼번에 해결했습니다.
1. **스크린 키보드 한/영 전환 및 접속 직후 초기 한/영 동기화**: Android 스크린 키보드(Gboard·삼성 키보드 등)에서 선택한 언어(한글/영문)가 macOS 입력 프롬프트의 입력 소스(`ABC`/`한`)에 즉시 반영되고, 접속 직후에도 양쪽 한/영 상태가 일치하도록 구현했습니다.
2. **화면 갱신 멈춤(프레임 정지) 수정**: 입력창 터치·타이핑 이후나 두 대의 클라이언트가 동시에 접속한 상황에서 macOS 입력은 실제로 들어가는데 Android 화면이 이전 프레임에 멈춰 갱신되지 않던 문제를 해결했습니다.
3. **터치 클릭·좌표 전달 안정화**: Android 터치 탭이 macOS 입력창과 다른 UI 요소에 확실히 전달되도록 Apple 암호화 포인터 이벤트(`0x10`)와 사전 커서 이동, 선택 디스플레이 스케일 반영, 키보드 표시 시 뷰포트·포커스 동기화를 적용했습니다.

#### 2. 열기 위치
- **뷰어 화면 (`VncActivity`)**: macOS 서버 접속 직후 자동 동작하며, 별도 설정 없이 스크린 키보드 표시/한영 전환 및 화면 터치에 바로 반영됩니다.
- **빌드 APK 산출물**: `app/build/outputs/apk/debug/AVNC-v3.3.3.apk` 및 프로젝트 루트 `AVNC-v3.3.3.apk` (`versionCode 55`, `versionName "3.3.3 (debug)"`)

#### 3. 사용 방법
1. CodeMemo 실행 탭에서 `AVNC-v3.3.3.apk`를 빌드·다운로드해 Android 기기에 설치합니다.
2. macOS 서버에 접속하면 `1109(KeyboardSource)` 수신과 동시에 현재 Android 스크린 키보드 언어에 맞춰 macOS 입력 소스가 자동 동기화됩니다.
3. 뷰어에서 입력창을 터치하고 Android 스크린 키보드를 연 뒤 한/영 키를 전환하거나 한글·영문을 입력하면 macOS 입력 소스와 입력 텍스트가 함께 반영되며, 다른 위치 터치 및 화면 변화도 실시간으로 갱신됩니다.
4. 두 대의 클라이언트(예: Mac Screen Sharing + Android AVNC)가 같은 Mac에 동시 접속한 경우, 두 클라이언트는 동일한 Mac 세션·커서·포커스를 공유하며 각 클라이언트에서 같은 디스플레이(`Display 1` 등)를 선택하면 동일한 실시간 화면이 유지됩니다.

#### 4. 동작 및 오류 처리
- **macOS 입력 소스 동기화 (`AppleRfb.h`, `AppleRfb.cpp`, `RemoteKeyboardSource.kt`, `MacKeyboardSync.kt`, `VncClient.kt`, `VncViewModel.kt`)**:
  - 서버가 보내는 `1109(0x455)` `KeyboardSource`를 파싱해 현재 macOS 입력 소스 ID(`com.apple.keylayout.ABC`, `com.apple.inputmethod.Korean.2SetKorean`, 구름 입력기 등)와 보안 입력(`secureInput`, 암호창) 여부를 추적합니다.
  - `MacKeyboardSync` 상태 머신(`TOGGLING -> SELECTING -> LEAVING -> RETURNING -> PINNING`)이 `0x10` 암호화 물리 키코드 기반 `Control+Space`, `0x1a` `SelectKeyboardSource` + `VoidSymbol` 키업, 그리고 `Control+Space`가 비활성화된 Mac을 위한 `0x1a` 고정 폴백(`PINNING`)까지 처리합니다.
  - 암호 입력 필드(`secureInput == true`)에서는 강제 전환을 보류하고, 암호 입력을 벗어나면 원하는 언어로 자동 복구합니다.
  - Android `InputView`는 `BaseInputConnection(this, true)`와 `SpannableStringBuilder`, `TYPE_CLASS_TEXT | TYPE_TEXT_FLAG_MULTI_LINE | TYPE_TEXT_FLAG_NO_SUGGESTIONS`를 사용해 Gboard의 서브타입 변경(키보드 표시 중 250ms 엣지 감지)과 삼성 키보드의 단일 서브타입(입력 문자열 스크립트 감지)을 모두 지원합니다.
  - 한글/조합 문자나 macOS IME 전환 과도기의 영문 입력은 일시적(`org.nspasteboard.TransientType`) `0x1f` UTF-8 페이스트보드 + `0x10` `Cmd+V`로 전달해 자모 분리나 `ㅁㅠㅊ` 오입력을 방지하고, 자체 에코(`0x14 cmd=2`)를 억제한 뒤 입력 종료 1.2초 후 기존 Mac 클립보드를 자동 복원합니다. 영문 레이아웃(`ABC`/`US`) 및 암호창·공백·엔터는 지연 없는 직접 키 이벤트(`0x04`)로 전달합니다.
- **화면 갱신 멈춤(프레임 정지) 수정 (`AppleRfb.cpp`, `native-vnc.cpp`, `LayoutManager.kt`, `Renderer.kt`, `VncViewModel.kt`)**:
  - Rekey 중 `1105 DisplayLayout`이 먼저 도착해 `c->running == false`였던 경우 연결 직후 `sendUpdateArm(c, true)`가 누락되던 문제를 수정했습니다.
  - `zlibDecoderInflate`에서 `avail_in == 0`이지만 `avail_out == 0`인 경계 조건을 수정하고, 프레임버퍼 크기 변경 시 `hasReceivedPixels`를 초기화하며 `VncViewModel.onFramebufferSizeChanged`에서도 즉시 `refreshFrameView()`를 호출하도록 수정했습니다.
  - `appleRfbWaitForMessage` 타임아웃을 250ms로 단축하고 워치독(`maybeKickStalledFramebuffer`)을 추가했습니다: 입력 후 180ms 내 픽셀 갱신이 없으면 즉시 증분 `0x03` + `0x09`를 재요청하며, `0x09` 자동 푸시 정지가 2회 감지되면 세션 전체를 연속 증분 `0x03` 풀 모드로 승격합니다. 또한 유휴 상태에서도 1.5초마다 경량 증분 `0x03`을 보내 다른 클라이언트에서 발생한 화면 변화도 즉시 반영합니다.
  - Android API 30+ `LayoutManager.hookInsetsListener()`에서 `applyInsets()`를 즉시 호출하고, `Renderer.onSurfaceChanged()`에서 `FrameState.setViewportSize()`를 동기화하며, 키보드 표시 중 macOS 가상 디스플레이 재구성(`0x1d`)이 재발동하지 않도록 보호했습니다.
- **터치 클릭·좌표 안정화 (`AppleRfb.cpp`, `PointerModes.kt`, `Messenger.kt`, `InputView.kt`, `VncActivity.kt`)**:
  - `handleDisplayLayout`에서 `displays[0]` 대신 현재 선택된 디스플레이(`currentDisplayId`)의 `scale`을 `pointerScale`로 사용하도록 수정하고, `1100`/`-232` 커서 좌표의 중복 스케일링을 제거했습니다.
  - `appleRfbSendPointerEvent`가 버튼 누름 전에 새 좌표로 `0x05` 커서 이동을 먼저 보내고, 버튼 상태 변경은 클릭 횟수(`clickCount`)를 포함한 Apple `0x10` 암호화 포인터 메시지로 전송하도록 개선했습니다.
  - macOS 세션 클릭 시 `BasePointerMode.doClick`에 25ms 눌림 간격(`insertAppleClickHoldDelay`)을 두어 0ms 합성 클릭 누락을 방지하고, `InputView.onTouchEvent(ACTION_DOWN)` 및 `VncActivity.showKeyboard()`에서 포커스를 안정적으로 관리하도록 수정했습니다.

#### 5. 검증 결과
- **네이티브 C++ 단위·종단간(E2E) 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror`, OpenSSL 3 + zlib):
  - 기존 8개 테스트 + 신규 `testKeyboardPointerAndWatchdogE2E` 포함 총 **9개 테스트 전부 통과**: `1109` 파싱, `0x1a` 선택/해제, `0x10` 암호화 키(`Control+Space`, `Cmd+V`), `0x10` 암호화 포인터(사전 이동 + 단일/더블클릭 `clickCount`), `0x1f` Transient `PasteText` + `0x14 cmd=2` 에코 억제 + 클립보드 복원, `0x04` 유니코드 텍스트(`0x0100D55C`), 입력 후 프레임 정지 워치독 `0x03` 복구 검증 완료.
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest`):
  - 총 **12개 테스트 전부 통과** (`MacKeyboardSyncTest` 9/9 통과, `RemoteDisplayTest` 3/3 통과).
- **Android 계측 테스트 컴파일** (`./gradlew :app:compileDebugAndroidTestKotlin`): 통과.
- **Gradle & CodeMemo 신뢰 경로 APK 빌드** (`./gradlew :app:assembleDebug --no-daemon`, `gradle :app:assembleDebug --no-daemon`):
  - `AVNC-v3.3.3.apk` (프로젝트 루트) 및 `app/build/outputs/apk/debug/AVNC-v3.3.3.apk` (`14,411,968 bytes`, `versionCode 55`, `versionName "3.3.3 (debug)"`) 생성 완료, 두 파일의 SHA-256 해시(`ba80eb4d53359cb8379c665cacf5b680a40eeb8178008ec024a9431f5e18d7b0`) 일치 확인, 이전 루트 `AVNC-v3.3.2.apk` 자동 정리 확인.

#### 6. 실제 배포 상태 및 제한 사항
- **배포 상태**: 로컬 워크스페이스에서 C++ 네이티브 테스트, Android JVM 단위 테스트, 계측 테스트 컴파일 및 `AVNC-v3.3.3.apk` 빌드까지 완료되었으며, 실제 Android 기기 설치 및 실기 macOS 연동 확인은 사용자가 생성된 `AVNC-v3.3.3.apk`를 설치해 확인할 단계입니다.
- **제한 사항**:
  - 삼성 키보드 등 단일 Android IME 서브타입을 사용하는 키보드는 내부 한/영 버튼 전환 시 시스템 서브타입 이벤트를 발행하지 않으므로, 첫 글자 입력 순간 스크립트를 감지해 macOS 입력 소스를 전환하며 전환 과도기의 첫 글자도 `PasteText`로 안전하게 전달합니다(Gboard 등 멀티 서브타입 키보드는 한/영 키를 누르는 즉시 입력 전에도 반영).
  - 두 클라이언트가 같은 Mac에 동시 접속하면 macOS `screensharingd` 정책상 입력·커서·포커스는 하나의 세션으로 공유되지만, 표시할 디스플레이(`View -> Display 1 / Display 2 / All Displays`)는 클라이언트마다 독립 선택할 수 있습니다. 두 클라이언트가 같은 화면을 보려면 양쪽 모두 동일한 디스플레이(예: `Display 1`)를 선택하면 됩니다.

---

<a id="hist-20261121-005"></a>
### 2026-11-21 18:14:54 +09:00 — macOS Apple RFB 프레임 워치독 2단계 전체 갱신 에스컬레이션·스레드 안전화 및 스크롤 휠 포인터 수정 (`v3.3.4`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-005` |
| 변경 유형 | 버그 수정 및 안정성 강화 (`AppleRfb.cpp`, `AVNC-v3.3.4.apk`) |
| 상태 | 완료 (로컬 C++ 네이티브·JVM 단위 테스트 및 APK 빌드 검증 완료, 실기기 설치 대기) |

#### 1. 기능 목적
- Android ARM64 멀티스레드 환경에서 Sender 스레드와 Receiver 스레드 사이의 프레임 워치독/입력 상태 공유를 원자적(atomic) 연산으로 안전화하고, 증분 `0x03` 복구 요청에도 macOS에서 픽셀이 오지 않을 때 1회 전체 갱신(`incremental=0`)으로 에스컬레이션하여 화면 정지를 원천 차단합니다.
- macOS `screensharingd`에서 스크롤 휠 이벤트(`0x08`/`0x10`/`0x20`/`0x40`)가 암호화 포인터(`0x10`)로 잘못 라우팅되지 않고 표준 `0x05` 포인터 메시지로 전달되도록 수정합니다.

#### 2. 여는 위치 및 사용 방법
- CodeMemo **실행 탭**에서 최신 **`AVNC-v3.3.4.apk`** 링크로 설치한 뒤 기존과 동일하게 macOS Screen Sharing 프로필로 접속해 사용합니다.

#### 3. 주요 동작 및 수정 내용
- **원자적 워치독 상태 및 2단계 복구 에스컬레이션 (`app/src/main/cpp/AppleRfb.cpp`)**:
  - `hasInputSinceLastPixel`, `lastInputSentMs`, `fbWidth`, `fbHeight`, `paused`에 `__atomic_load_n` / `__atomic_store_n`을 적용하고 `pointerScale` 접근을 `writeMutex`로 보호해 ARM64 데이터 레이스를 제거했습니다.
  - `appleRfbSendMacKeyCode`, `appleRfbSelectKeyboardSource`도 입력 활동으로 기록해 한/영 전환 직후에도 180ms 프레임 정지 감지가 작동하도록 했습니다.
  - 입력 후 180ms 증분 `0x03` 복구 요청에 650ms 동안 픽셀 응답이 없으면 1회 비증분 전체 갱신(`sendUpdateArm(c, true)`)으로 에스컬레이션해 macOS가 현재 화면 전체를 즉시 재전송하도록 보강했습니다.
- **스크롤 휠 `0x05` 라우팅 분리 (`app/src/main/cpp/AppleRfb.cpp`)**:
  - `appleRfbSendPointerEvent`에서 휠 비트(`~0x07`)와 실제 마우스 버튼(`0x07`)을 분리해, 스크롤 휠은 항상 `0x05` `PointerEvent`로 전송하고 `0x10` `EncryptedPointerMessage`와 `clickCount` 추적을 오염시키지 않도록 수정했습니다.
- **버전 갱신 (`app/build.gradle.kts`)**: `versionCode = 56`, `versionName = "3.3.4"`.

#### 4. 검증 결과
- **네이티브 C++ 단위·E2E 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror`, OpenSSL 3 + zlib):
  - 총 **9개 테스트 전부 통과** (스크롤 휠 `0x05` 라우팅 + `clickCount` 비오염 검증 및 입력 후 증분 `0x03` → 650ms 무응답 시 비증분 전체 `0x03` 1회 에스컬레이션 검증 포함).
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest`): **12/12 통과**.
- **Android 계측 테스트 컴파일** (`./gradlew :app:compileDebugAndroidTestKotlin`): 통과.
- **Gradle & CodeMemo 신뢰 경로 APK 빌드** (`./gradlew :app:assembleDebug --no-daemon`, `gradle :app:assembleDebug --no-daemon`):
  - `AVNC-v3.3.4.apk` (프로젝트 루트) 및 `app/build/outputs/apk/debug/AVNC-v3.3.4.apk` (`13,143,224 bytes`, `versionCode 56`, `versionName "3.3.4 (debug)"`) 생성 및 SHA-256 일치(`a37fb8c0e49ec362642305d17eb046432648f32521c7381edb56e5bcce58b173`), 이전 루트 `AVNC-v3.3.3.apk` 자동 정리 확인.

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 워크스페이스 검증 및 `AVNC-v3.3.4.apk` 빌드까지 완료되었으며, 실제 Android 기기 설치 및 실기 macOS 연동 테스트는 실행 탭에서 `AVNC-v3.3.4.apk`를 설치해 확인할 단계입니다.

---

<a id="hist-20261121-006"></a>
### 2026-11-21 19:16:58 +09:00 — Android→macOS 한글 입력 근본 수정 (`InputView` IME 선택/조합 동기화·이중 IME 충돌 제거·직접 `0x04` 유니코드 및 원자적 `0x1f+Cmd+V` 폴백, `v3.3.5`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-006` |
| 변경 유형 | 버그 수정 (`AppleRfb.h`, `AppleRfb.cpp`, `native-vnc.cpp`, `VncClient.kt`, `Messenger.kt`, `VncViewModel.kt`, `MacKeyboardSync.kt`, `Dispatcher.kt`, `InputHandler.kt`, `InputView.kt`, `VirtualKeys.kt`, `VncActivity.kt`, `AVNC-v3.3.5.apk`) |
| 상태 | 완료 (로컬 C++ 네이티브·JVM 단위 테스트 및 APK 빌드 검증 완료, 실기기 설치 대기) |

#### 1. 기능 목적
- Android 스크린 키보드(삼성 키보드·Gboard 등)에서 macOS Screen Sharing(`RFB 3.889`)으로 한글/영문을 입력할 때 발생하던 5가지 근본 원인(IME 선택 상태 미보고, 이중 IME 충돌, `0x1f` 능력 비트 오판, 비원자적 `Cmd+V` 경쟁, 조합 중 과도한 클립보드 붙여넣기)을 일괄 해결해 한글과 영문 입력이 빠르고 정확하게 작동하도록 합니다.

#### 2. 여는 위치 및 사용 방법
- CodeMemo **실행 탭**에서 최신 **`AVNC-v3.3.5.apk`**를 설치한 뒤 macOS 프로필에 접속해 평소처럼 Android 스크린 키보드(또는 가상 키보드 텍스트 입력창)에서 한/영을 전환하며 한글과 영문을 입력합니다.

#### 3. 주요 수정 내용 및 동작
- **Android IME 조합/커서 동기화 전면 수정 (`InputView.kt`)**:
  - `init`에서 `Selection.setSelection(document, 0)`을 초기화하고 `onCreateInputConnection`에서 `initialSelStart = 0`, `initialSelEnd = 0`, `initialCapsMode = 0`, `EditorInfoCompat.setInitialSurroundingText`, `IME_ACTION_NONE | IME_FLAG_NO_EXTRACT_UI | IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_PERSONALIZED_LEARNING` 및 API 33+ `isAutoHandwritingEnabled = false`를 설정했습니다.
  - 모든 편집 콜백(`commitText`, `setComposingText`, `setComposingRegion`, `finishComposingText`, `setSelection`, `deleteSurroundingText*`)을 배치(`batch { ... }`)로 감싸고, 변경 시 매번 `imm.updateSelection(this, selStart, selEnd, compStart, compEnd)`를 보고해 삼성 키보드·Gboard의 한글 음절 조합이 깨지지 않도록 했습니다.
  - `KEYCODE_DEL`은 `BreakIterator` 기반 `deleteBackward()`로 로컬 조합 버퍼와 원격 백스페이스를 일관 처리하고, 화면 터치/엔터 시 조합 중인 마지막 글자를 먼저 flush한 뒤 `imm.restartInput(this)`로 IME 내부 버퍼를 깨끗하게 초기화합니다.
- **이중 IME 충돌 제거 & 직접 `0x04` 유니코드 한글 입력 (`VncViewModel.kt`, `MacKeyboardSync.kt`, `Dispatcher.kt`, `InputHandler.kt`)**:
  - macOS `ScreensharingAgent`는 비-IME 레이아웃(`com.apple.keylayout.ABC` / `US`)일 때 `0x04` 유니코드 키심(`0x01000000 | codepoint`)으로 한글(BMP 전체)과 영문을 클립보드·`Cmd+V` 없이 0ms 직접 입력하지만, macOS가 `2SetKorean` IME에 들어가면 `0x04` 유니코드와 영문을 가로채 망가뜨립니다.
  - 접속 시 및 소프트 키보드 사용 시 macOS를 `ABC`/`US`(비-IME)로 유지하여 Android 스크린 키보드가 한/영 전환과 한글 조합의 단일 진실 공급원(single source of truth)이 되도록 했습니다. (외장 물리 키보드의 하드웨어 한/영 키 토글은 그대로 유지됩니다.)
  - macOS가 `ABC`/`US`일 때는 백스페이스+대체 음절을 단일 패킷으로 묶은 **`appleRfbSendTextDiff`**(`0x04` 원자적 전송)로 실시간 한글 조합을 지연 없이 표시하고, macOS가 아직 IME(`2SetKorean`)에 있거나 `1109` 수신 전일 때는 조합 완료 텍스트만 원자적 `PasteText`로 안전하게 전달(`prefersCommittedTextOnly`)합니다.
- **Apple `0x1f` 페이스트보드 능력 판정 및 원자적 `PasteText` 수정 (`AppleRfb.h`, `AppleRfb.cpp`, `native-vnc.cpp`, `VncClient.kt`, `Messenger.kt`, `VirtualKeys.kt`)**:
  - `supportsApplePasteboard(c)`가 `0x1f` 비트만 보지 않고 `0x15`(`AutoPasteboard`) 광고 시에도 활성화되도록 수정해 실제 macOS 서버에서 `0x1f` 폴백이 무력화되던 치명적 버그를 제거했습니다.
  - `appleRfbPasteText`가 `0x1f` Transient 페이스트보드와 4개의 `0x04` `Cmd+V`(`0xffeb`, `'v'`) 키 이벤트를 단일 `streamWrite` 버퍼로 원자 전송하도록 수정했습니다.
  - `VirtualKeys.handleTextBoxAction`도 Apple 세션에서 `inputHandler.onTextInput` 경로를 직접 타도록 개선했습니다.
- **버전 갱신 (`app/build.gradle.kts`)**: `versionCode = 57`, `versionName = "3.3.5"`.

#### 4. 검증 결과
- **네이티브 C++ 단위·E2E 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror`, OpenSSL 3 + zlib):
  - 총 **9/9 테스트 전부 통과** (`0x1f` 비트 없이 `0x15`만 광고하는 서버에서 `PasteText` + 클립보드 복원 동작 검증, 단일 버퍼 `0x1f + 0x04 Cmd+V` 검증, `appleRfbSendTextDiff(1, "한a\r\n")` 원자 전송 검증 포함).
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest`): **12/12 통과** (`MacKeyboardSyncTest` 9/9, `RemoteDisplayTest` 3/3).
- **Android 계측 테스트 컴파일** (`./gradlew :app:compileDebugAndroidTestKotlin`): 통과.
- **Gradle & CodeMemo 신뢰 경로 APK 빌드** (`./gradlew :app:assembleDebug --no-daemon`, `gradle :app:assembleDebug --no-daemon`):
  - `AVNC-v3.3.5.apk` (프로젝트 루트) 및 `app/build/outputs/apk/debug/AVNC-v3.3.5.apk` (`13,145,355 bytes`, `versionCode 57`, `versionName "3.3.5 (debug)"`) 생성 및 SHA-256 일치(`7000e20e2f35ec0bee9086be70d121a43e7fe8d79f656a1654946a97ca28fa34`), 이전 루트 `AVNC-v3.3.4.apk` 자동 정리 확인.

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 워크스페이스 검증 및 `AVNC-v3.3.5.apk` 빌드까지 완료되었으며, 실제 Android 기기(삼성 키보드·Gboard)와 macOS 간 실기기 한글 입력 동작은 실행 탭에서 `AVNC-v3.3.5.apk`를 설치해 확인할 단계입니다.

---

<a id="hist-20261121-007"></a>
### 2026-11-21 20:01:21 +09:00 — 실행 탭 중복 APK 링크 제거 (Gradle 기본 산출물 단일 유지)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-007` |
| 변경 유형 | 빌드 설정 정리 (`app/build.gradle.kts`) |
| 상태 | 완료 (로컬 Gradle·CodeMemo 신뢰 경로 빌드 검증 완료) |

#### 1. 기능 목적
- CodeMemo 실행 탭에 프로젝트 루트 복사본과 Gradle 기본 디버그 산출물 2개가 중복 링크로 표시되던 문제를 해결해, 표준 Gradle 산출물(`app/build/outputs/apk/debug/AVNC-v3.3.5.apk`) 1개만 표시되도록 정리합니다.

#### 2. 여는 위치 및 사용 방법
- CodeMemo **실행 탭**에서 단일 링크 `avnc/app/build/outputs/apk/debug/AVNC-v3.3.5.apk`를 내려받아 설치합니다.

#### 3. 수정 내용
- `app/build.gradle.kts`에서 `assembleDebug`의 루트 복사 `doLast` 블록을 제거하고, `androidComponents`의 버전 포함 출력 파일명(`AVNC-v${versionName}.apk`) 설정만 유지했습니다.
- 프로젝트 루트에 남아 있던 중복 복사본 `AVNC-v3.3.5.apk`를 삭제했습니다.

#### 4. 검증 결과
- `./gradlew :app:assembleDebug --no-daemon` 및 `gradle :app:assembleDebug --no-daemon` 빌드 성공.
- `find . -maxdepth 6 -name "*.apk"` 기준 프로젝트 전체에 `app/build/outputs/apk/debug/AVNC-v3.3.5.apk` (`13,145,355 bytes`, SHA-256 `7000e20e2f35ec0bee9086be70d121a43e7fe8d79f656a1654946a97ca28fa34`) 1개만 존재함을 확인했습니다.

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 빌드 산출물 정리 완료 상태이며, CodeMemo 실행 탭 새로고침 시 단일 APK 링크만 표시됩니다.


---

<a id="hist-20261121-008"></a>
### 2026-11-21 20:11:48 +09:00 — 한·영 문서 작성 및 GitHub 첫 화면 안내 추가 (원본 `Readme.md` 유지)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-008` |
| 변경 유형 | 문서 추가 및 저장소 정리 (`.github/README.md`, `docs/MACOS_GUIDE.md`, `docs/MACOS_GUIDE.ko.md`, `.gitignore`) |
| 상태 | 작성·링크 검증 완료 (원본 `Readme.md` 무수정 확인, GitHub 커밋·푸시 대상) |

#### 1. 기능 목적
- 다중 모니터 macOS 서버에서 단일 디스플레이를 선택하는 기능(`RFB 3.889`), Android 스크린 키보드 한글·영문 입력 근본 해결, 화면 갱신 정지 방지 및 터치 정확도 개선 내역을 **한국어와 영어 문서**로 체계적으로 정리했습니다.
- 원본 업스트림 `Readme.md`는 전혀 수정하지 않고 그대로 유지하면서도, GitHub 저장소 방문자가 별도로 파일을 찾아보지 않아도 첫 화면에서 변경 사항을 즉시 볼 수 있도록 GitHub 최우선 표시 경로인 `.github/README.md`를 추가했습니다.

#### 2. 추가·수정 파일
- `.github/README.md`: GitHub 저장소 메인 화면에 자동 렌더링되는 한·영 요약 안내서(다중 모니터 단일 선택, 한글·영문 입력 해결, 화면 갱신/터치 수정 요약, 빠른 사용법, 상세 문서 및 원본 `Readme.md` 바로가기 링크 포함).
- `docs/MACOS_GUIDE.md`: 영문 전체 사용자 가이드, 프로토콜·아키텍처 해설, 다중 클라이언트 동작, 한글·영문 입력 5대 원인 및 해결 구조도, 변경 파일 표, 빌드·검증 명령.
- `docs/MACOS_GUIDE.ko.md`: 한국어 전체 사용자 가이드 및 기술 문서(`MACOS_GUIDE.md` 및 `Readme.md` 상호 링크).
- `.gitignore`: 로컬 CodeMemo 디자인 메타데이터(`.codememo-design/`)가 커밋되지 않도록 제외 규칙 보강.

#### 3. 검증 결과
- `git diff --stat Readme.md` 비어 있음 확인(원본 `Readme.md` 100% 무수정 유지).
- `.github/README.md`, `docs/MACOS_GUIDE.md`, `docs/MACOS_GUIDE.ko.md` 내 모든 상대 경로 링크 및 이미지(`../metadata/en-US/branding/wordmark.svg`)가 실제 파일에 정상 대응함을 확인했습니다.
- 로컬 전용 디렉터리(`.codememo-design/`, `.codememo-build/`, `.cc-uploads/`, `build/`) 및 `.apk` 산출물이 Git 추적 대상에서 제외됨을 확인했습니다.


---

<a id="hist-20261121-009"></a>
### 2026-11-21 22:09:38 +09:00 — 스마트폰 키보드 포커스·IME 바인딩·조합 입력 차단 버그 수정 (`v3.3.6-seionwon`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261121-009` |
| 변경 유형 | 버그 수정 (`Keyboard.kt`, `VirtualKeys.kt`, `Toolbar.kt`, `VncActivity.kt`, `InputView.kt`, `Dispatcher.kt`, `MacKeyboardSync.kt`, `KeyHandler.kt`, `AppleRfb.cpp`, `app/build.gradle.kts`) |
| 상태 | 완료 (로컬 C++ 네이티브·JVM 단위 테스트 및 `AVNC-v3.3.6-seionwon.apk` 빌드 검증 완료, 실기기 설치 테스트 대기) |

#### 1. 기능 목적
- 갤럭시 탭에서는 정상 동작하던 키보드 입력이 스마트폰에서 터치는 되는데 원격 입력창에 키보드 타이핑이 들어가지 않던 문제를 근본 수정했습니다.

#### 2. 여는 위치 및 사용 방법
- CodeMemo **실행 탭**에서 최신 **`AVNC-v3.3.6-seionwon.apk`**를 내려받아 스마트폰에 설치한 뒤 macOS(또는 일반 VNC) 서버에 접속해 툴바 키보드 버튼, 가상 키 바 키보드 아이콘, 두 손가락 탭 중 어떤 방식으로 키보드를 열더라도 바로 한글·영문을 입력할 수 있습니다.

#### 3. 주요 원인 및 수정 내용
- **오프스크린 `VirtualKeys.textBox` 포커스 탈취 및 `ToggleKeyboard` 포커스 누락 수정 (`VirtualKeys.kt`, `VncActivity.kt`)**:
  - 신규 설치 기본값(`showVirtualKeys = true`)으로 표시되는 `VirtualKeys`의 `ViewPager`(`offscreenPageLimit = 2`) 2페이지에 숨어 있던 `VkEditText`(`text_box`)가 비활성 상태에서도 `focusableInTouchMode = true`로 남아 있어, `VncActivity.showKeyboard()`의 `currentFocus as? EditText` 조건이 화면 밖 `text_box`로 키보드 입력을 가로채던 문제를 차단했습니다(`isTextBoxActive`가 참일 때만 `textBox` 포커스 허용, 숨김·페이지 전환 시 즉시 포커스 해제 후 `inputView`로 복원).
  - `VirtualKeys` 하단 키보드 토글 버튼이 `toggleKeyboard(inputView)`를 직접 호출해 `inputView.requestFocus()`를 생략하던 문제를 `activity.toggleKeyboard()` 경유로 통일하고, `onSessionConnected()`, `ViewerHelp` 종료, `onWindowFocusChanged(true)`에서도 `inputView` 포커스를 보장했습니다.
- **툴바 드로어 포커스 차단 및 Android 11–16 / 삼성 One UI IME 바인딩 타이밍 레이스 수정 (`Toolbar.kt`, `Keyboard.kt`)**:
  - `Toolbar.keyboardBtn`이 드로어가 열린 상태에서 `showKeyboard()`를 먼저 호출하고 `close()`를 나중에 호출하던 순서를 뒤집어 `close()` 후 `showKeyboard()`를 실행하고, `onDrawerClosed()` 시점에도 키보드 포커스·IME 바인딩을 재확인하도록 수정했습니다.
  - `Keyboard.showKeyboard(view)`가 `requestFocus()` 직후 동일 프레임에서 `SoftwareKeyboardControllerCompat.show()`만 호출해 `InputView.onCreateInputConnection()`이 바인딩되지 않던 레이스를 없애기 위해 `view.post` + 150ms 안전 재확인(`!imm.isActive(view)` 시 `imm.restartInput(view)` + `imm.showSoftInput` + `SoftwareKeyboardControllerCompat.show()`)을 적용하고, `hideKeyboard()` 시 예약된 재시도를 즉시 취소하도록 보강했습니다.
- **`1109` 미수신(`remoteKeyboardSource == null`) 시 폰 키보드 조합 입력 침묵 제거 (`Dispatcher.kt`, `MacKeyboardSync.kt`, `VncActivity.kt`, `InputView.kt`)**:
  - `remoteKeyboardSource == null`일 때 `prefersCommittedTextOnly`와 `shouldUsePasteText`가 `true`를 반환하면서, 영문·한글 모두 `setComposingText`로 조합하는 스마트폰 키보드(삼성 키보드·Gboard)의 입력을 전부 숨겨 버리던 문제를 수정했습니다. 이제 macOS가 실제로 IME 상태(`src != null && src.isInputMethod`)로 확인된 경우에만 조합 버퍼링/`PasteText`를 사용하고, `src == null`이거나 비-IME(`ABC`/`US`)일 때는 직접 `0x04` 유니코드 diff(`appleRfbSendTextDiff`)로 즉시 입력됩니다.
  - `remoteKeyboardSource`가 IME → `ABC`로 전환되면 `VncActivity`가 `inputView.onRemoteKeyboardSourceChanged()`를 호출해 대기 중이던 조합 문자열을 즉시 플러시합니다.
- **Android 13–16 `InputConnection` 확장 및 포커스 전환 복구 (`InputView.kt`, `KeyHandler.kt`)**:
  - Android 13/14+ 폰 키보드가 사용하는 `replaceText` (API 34+), 3인자 `commitText`/`setComposingText`/`setComposingRegion` (API 33+), `commitCompletion`을 `batch {}` 경로로 오버라이드했습니다.
  - `onFocusChanged` 및 `onTouchEvent(ACTION_DOWN)`에서 포커스 획득 직후 `imm.isActive(this)`가 거짓이면 `restartInput`으로 IME 연결을 자동 복구하고, 아직 포커스가 정착되기 전에 조기 `restartInput`을 호출하지 않도록 수정했습니다.
  - 일반(비-Apple) VNC 서버용 `KeyHandler.onTextInput`에서도 `\r\n`, `\n`, `\t`, `\b`를 `XK_Return`/`XK_Tab`/`XK_BackSpace`로 정확히 변환하도록 개선했습니다.
- **`appleRfbPasteText` 레이아웃 독립 물리 `Cmd+V` 개선 (`AppleRfb.cpp`)**:
  - macOS가 IME 상태이거나 이모지를 보낼 때 사용하는 `appleRfbPasteText`가 `0x1f` Transient 페이스트보드 전송 후 15ms 대기(`pboard` 반영 확보) 뒤 `0x10` 암호화 물리 키코드(`Command(55) + V(9)`)를 단일 패킷으로 전송하도록 강화했습니다(`0x10` 미지원 서버는 `0x04` 폴백 유지).
- **버전 갱신 (`app/build.gradle.kts`)**: 포크 버전 규칙에 따라 `versionCode = 58`, `versionName = "3.3.6-seionwon"`으로 갱신.

#### 4. 검증 결과
- **네이티브 C++ 단위·E2E 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations`, OpenSSL 3 + zlib): **9/9 통과** (`0x1f` Transient 페이스트보드 + `0x10` 물리 `Command(55)+V(9)` 검증 포함).
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest --no-daemon`): **12/12 통과** (`MacKeyboardSyncTest` 9/9, `RemoteDisplayTest` 3/3).
- **Android 계측 테스트 컴파일** (`./gradlew :app:compileDebugAndroidTestKotlin --no-daemon`): 통과.
- **디버그 APK 빌드** (`./gradlew :app:assembleDebug --no-daemon`):
  - 단일 산출물 `app/build/outputs/apk/debug/AVNC-v3.3.6-seionwon.apk` (`13,149,695 bytes`, `versionCode 58`, `versionName "3.3.6-seionwon (debug)"`, SHA-256 `035bc0a50f96b0254f31b44d8c8c4eddd5cc0990cdd5b8be85137009cde83b7d`) 생성 확인.

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 검증 및 `AVNC-v3.3.6-seionwon.apk` 빌드까지 완료되었으며, 실제 스마트폰 설치 테스트 및 원격 GitHub 푸시는 사용자 확인 후 진행할 단계입니다.


---

<a id="hist-20261122-001"></a>
### 2026-11-22 01:59:31 +09:00 — macOS 모니터 최대 6개 단일 선택 지원 및 2개 동시·전체 디스플레이 제거 (`v3.3.7-seionwon`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261122-001` |
| 변경 유형 | 기능 변경 (`arrays.xml`, `strings.xml`, `values-ko/strings.xml`, `ServerProfile.kt`, `RemoteDisplay.kt`, `EditorViewModel.kt`, `VncUri.kt`, `RemoteSession.kt`, `Toolbar.kt`, `VncClient.kt`, `AppleRfb.h`, `AppleRfb.cpp`, `native-vnc.cpp`, `app/build.gradle.kts`) |
| 상태 | 완료 (로컬 C++ 네이티브·JVM 단위 테스트 및 `AVNC-v3.3.7-seionwon.apk` 빌드 검증 완료, 실기기 설치 대기) |

#### 1. 기능 목적
- 다중 모니터 macOS 서버에서 **최대 6개의 모니터 중 반드시 1개만 선택해 표시**하도록 지원 범위를 확장(기존 4개 → 6개)하고, **2개 동시 가상 디스플레이(`가상 디스플레이 2개`) 및 다중 모니터 합쳐 보기(`전체 디스플레이`)** 기능을 제거했습니다.

#### 2. 여는 위치 및 사용 방법
- **프로필 편집 → 고급**:
  - **macOS 디스플레이**: `표준 (물리 디스플레이)` 또는 `고성능: 가상 디스플레이 1개` 중 선택(기존 `가상 디스플레이 2개` 제거).
  - **초기 디스플레이**: `디스플레이 1` ~ `디스플레이 6` 중 1개 선택(기본값 `디스플레이 1`, 기존 `전체 디스플레이` 제거).
- **뷰어 도구 모음 → 디스플레이 전환**: 연결된 모니터(최대 6개) 중 **단일 디스플레이 1개**만 선택해 실시간 전환할 수 있습니다.
- **빌드 산출물**: CodeMemo 실행 탭의 `app/build/outputs/apk/debug/AVNC-v3.3.7-seionwon.apk` (`versionCode 59`, `versionName "3.3.7-seionwon (debug)"`).

#### 3. 주요 변경 내용 및 하위 호환 처리
- **2개 동시 가상 디스플레이 제거 (`arrays.xml`, `strings.xml`, `values-ko/strings.xml`, `AppleRfb.h`, `AppleRfb.cpp`)**:
  - `APPLE_DISPLAY_VIRTUAL_2` 옵션과 UI 항목(`고성능: 가상 디스플레이 2개`)을 제거하고, `appleRfbBuildVirtualDisplayMessage`도 `displayCount == 1`만 허용하도록 제한했습니다.
  - 기존 저장 프로필이나 `vnc://` URI에 `appleDisplayMode = 2`가 남아 있던 경우 Room DB 마이그레이션 없이 자동으로 `APPLE_DISPLAY_VIRTUAL_1`(`1`)로 정규화됩니다.
- **최대 6개 단일 디스플레이 전용 선택 (`RemoteDisplay.kt`, `ServerProfile.kt`, `EditorViewModel.kt`, `VncUri.kt`, `RemoteSession.kt`, `Toolbar.kt`, `VncClient.kt`, `native-vnc.cpp`, `AppleRfb.cpp`)**:
  - `APPLE_MAX_SELECTABLE_DISPLAYS = 6` / `RemoteDisplayState.MAX_SELECTABLE_DISPLAYS = 6` 상수를 적용해 UI(초기 디스플레이 `1~6`, 뷰어 툴바 라디오 버튼)와 네이티브 콜백 모두 최대 6개까지 단일 선택만 노출합니다.
  - 뷰어 툴바에서 `전체 디스플레이` 항목을 제거하고, `VncClient.selectDisplay`와 `appleRfbSelectDisplay`에서 `ALL_DISPLAYS(0 / 0xffffffff)` 선택을 차단했습니다.
  - 다중 모니터 서버 접속 시 기존 프로필의 `appleInitialDisplay = 0`(구 전체 디스플레이)이거나 연결된 모니터 수보다 큰 번호가 설정되어 있어도 자동으로 `디스플레이 1`을 선택해 합쳐진 다중 화면에 머물지 않도록 처리했습니다.
- **한·영 문서 동기화**: `.github/README.md`, `docs/MACOS_GUIDE.md`, `docs/MACOS_GUIDE.ko.md`를 6모니터 단일 선택 기준에 맞게 갱신했습니다(원본 `Readme.md` 무수정 유지).
- **버전 갱신 (`app/build.gradle.kts`)**: 포크 버전 정책에 따라 `versionCode = 59`, `versionName = "3.3.7-seionwon"`으로 갱신했습니다.

#### 4. 검증 결과
- **네이티브 C++ 단위·E2E 테스트** (`clang++ -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations`, OpenSSL 3 + zlib): **9/9 통과** (`displayCount = 2` 거부, 6모니터 호스트에서 `initialDisplay = 6` 자동 선택·`디스플레이 5`/`디스플레이 1` 전환·`All Displays` 거부, 레거시 `initialDisplay = 0` → `디스플레이 1` 자동 선택 검증 포함).
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest --no-daemon`): **12/12 통과** (`RemoteDisplayTest` 3/3 — 6모니터 지원·8모니터 6개 상한·레거시 `2`/`0` 정규화 포함, `MacKeyboardSyncTest` 9/9).
- **Android 계측 테스트 컴파일 및 디버그 APK 빌드** (`:app:compileDebugAndroidTestKotlin`, `:app:assembleDebug`): 통과.
  - 단일 산출물: `app/build/outputs/apk/debug/AVNC-v3.3.7-seionwon.apk` (`13,149,443 bytes`, SHA-256 `ad2452cfe8d5bbda6b72e461065090e5c1c2b9250e64bdc04982e21bb58a0a3b`).

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 테스트 및 `AVNC-v3.3.7-seionwon.apk` 빌드까지 완료되었으며, 실기기 설치 확인 및 원격 GitHub 푸시는 사용자 확인 후 진행할 단계입니다.


---

<a id="hist-20261122-002"></a>
### 2026-11-22 02:22:18 +09:00 — 안드로이드 키보드 음성 입력(Voice Input) 지원 수정 (`v3.3.8-seionwon`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261122-002` |
| 변경 유형 | 버그 수정 (`InputView.kt`, `InputHandler.kt`, `Dispatcher.kt`, `app/build.gradle.kts`) |
| 상태 | 완료 (로컬 JVM 단위 테스트, 계측 테스트 컴파일 및 `AVNC-v3.3.8-seionwon.apk` 빌드 검증 완료, 실기기 설치 대기) |

#### 1. 기능 목적
- macOS 서버에 연결해 Claude Code 프롬프트 창 등 원격 입력란에서 안드로이드 키보드(삼성 키보드 음성 입력 / Google 음성 입력·Gboard)의 마이크 음성 입력을 시도할 때 **"음성 입력을 지원하지 않습니다"** 오류가 뜨던 문제를 해결했습니다.

#### 2. 여는 위치 및 사용 방법
- CodeMemo **실행 탭**에서 **`AVNC-v3.3.8-seionwon.apk`**를 내려받아 설치한 뒤 macOS 서버에 접속하고, Claude Code 프롬프트 등 원격 입력란을 터치해 안드로이드 키보드를 연 다음 키보드의 **마이크(음성 입력)** 버튼으로 한글·영문을 말하면 실시간으로 텍스트가 입력됩니다.

#### 3. 원인 및 수정 내용
- **음성 입력을 차단하던 `EditorInfo` 제한 플래그 제거 (`InputView.kt`, `InputHandler.kt`, `Dispatcher.kt`)**:
  - 기존 `InputView.onCreateInputConnection()`이 일반 입력 상태에서도 `EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING`(시크릿/보안 입력 모드)과 `InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS`를 항상 설정하고 있어, 삼성 키보드와 Gboard가 해당 입력창을 음성 입력 불가 필드로 판정하고 "지원하지 않습니다" 메시지를 표시했습니다.
  - 일반 입력 시 표준 멀티라인 텍스트 입력(`InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE`, `EditorInfo.IME_ACTION_UNSPECIFIED | IME_FLAG_NO_EXTRACT_UI | IME_FLAG_NO_FULLSCREEN`, `fieldId`, `packageName`)을 선언하도록 수정하고, `IME_FLAG_NO_PERSONALIZED_LEARNING` / `TYPE_TEXT_VARIATION_VISIBLE_PASSWORD`는 macOS `1109 KeyboardSource`가 실제 암호창(`secureInput == true`)을 알릴 때만 동적으로 적용(`onRemoteKeyboardSourceChanged`에서 자동 `restartInput`)하도록 변경했습니다.
- **`getExtractedText()` 및 `GET_EXTRACTED_TEXT_MONITOR` 구현 (`InputView.kt`)**:
  - `BaseInputConnection.getExtractedText()`가 기본 `null`을 반환해 일부 음성 받아쓰기 엔진이 편집 불가 뷰로 오인하던 문제를 해결하기 위해, `document` 버퍼와 선택 범위를 채운 `ExtractedText`를 반환하고 모니터링 토큰이 요청되면 편집 시 `imm.updateExtractedText(...)`로 실시간 동기화하도록 구현했습니다.
- **버전 갱신 (`app/build.gradle.kts`)**: 포크 버전 정책에 따라 `versionCode = 60`, `versionName = "3.3.8-seionwon"`으로 갱신했습니다.

#### 4. 검증 결과
- **Android JVM 단위 테스트** (`./gradlew :app:testDebugUnitTest --no-daemon`): **12/12 통과** (`MacKeyboardSyncTest` 9/9, `RemoteDisplayTest` 3/3).
- **Android 계측 테스트 컴파일 및 디버그 APK 빌드** (`:app:compileDebugAndroidTestKotlin`, `:app:assembleDebug`): 통과.
  - 단일 산출물: `app/build/outputs/apk/debug/AVNC-v3.3.8-seionwon.apk` (`13,150,341 bytes`, `versionCode 60`, `versionName "3.3.8-seionwon (debug)"`, SHA-256 `7aca4f2a894e4deca21afe83a192fc4830adf2ba7a4a63808d62ebaa87cd899a`).

#### 5. 실제 배포 상태 및 제한 사항
- 로컬 검증 및 `AVNC-v3.3.8-seionwon.apk` 빌드까지 완료되었으며, 실기기 설치 테스트 단계입니다.
- **참고 (제한 사항)**: 이 수정은 안드로이드 기기 키보드의 **음성 입력(삼성 음성 입력 / Google 음성 입력)**이 원격 macOS 입력창(Claude Code 프롬프트 포함)으로 텍스트를 받아쓰기하는 기능을 활성화합니다. VNC(RFB) 프로토콜 자체는 안드로이드 마이크 오디오를 원격 맥으로 전달하지 않으므로, 맥 터미널 안의 Claude Code 자체 `/voice` 명령 대신 **안드로이드 키보드의 마이크(음성 입력) 버튼**을 사용해 입력하면 됩니다.


---

<a id="hist-20261122-003"></a>
### 2026-11-22 04:34:30 +09:00 — 원격 화면의 부분 갱신과 커서 이미지 재사용

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261122-003` |
| 변경 유형 | 성능 개선 |
| 상태 | Android ARM 네이티브 빌드 및 로컬 렌더링 검증 완료 · 미배포 |

#### 1. 기능 목적 및 사용 방법

홈에서 일반 VNC 서버나 macOS 화면 공유에 접속하면 자동으로 적용됩니다. 정지한 원격 화면에서 포인터를 움직이거나 화면을 확대·이동할 때 Android 기기가 같은 화면 픽셀을 GPU로 반복 전송하던 비용을 줄였습니다.

#### 2. 갱신 동작

- 서버가 보낸 여러 변경 영역을 모아 첫 변경 줄부터 마지막 변경 줄까지 한 번에 전송합니다. 커서는 모양이 바뀔 때 이미지를 갱신하고, 위치만 바뀌면 기존 이미지를 새 위치에 그립니다.
- 재접속, 원격 해상도 변경, OpenGL 컨텍스트 재생성 시에는 전체 화면을 다시 올립니다. 텍스처 전송에 실패하면 다음 그리기에서 다시 초기화합니다.

#### 3. 검증 결과

- 1920×1080 기준으로 첫 전체 전송은 8,294,400바이트였습니다. 이후 동일 화면을 60회 다시 그릴 때 추가 화면 전송은 0바이트였고, 16줄 범위의 변경은 122,880바이트였습니다. OpenGL ES 소프트웨어 렌더러에서 부분 갱신, 누적 갱신, 두 번째 텍스처 복원, 해상도 변경 후 픽셀을 읽어 원본 버퍼와 일치하는지 확인했습니다.
- 일반 VNC의 Raw·CopyRect 및 macOS의 Raw·CopyRect·Zlib·AppleThousands·ZRLE 갱신 경로를 검증했습니다. 렌더러 호출 테스트에서 같은 크기의 새 커서 모양, 숨김 커서, 전송 실패 후 재시도, 연결 및 컨텍스트 재생성을 확인했습니다.
- Android NDK의 `arm64-v8a`와 `armeabi-v7a` 네이티브 라이브러리 빌드, Android API 36 대상 변경 Kotlin 소스 컴파일, 기존 JVM 단위 테스트 12개가 통과했습니다.

#### 4. 실제 배포 상태 및 제한 사항

이번 변경은 로컬 소스에 반영했습니다. 실행 환경에서 Gradle의 잠금용 소켓 생성이 제한되어 새 APK를 생성하지 못했습니다. 실기기 FPS와 배터리 사용량은 측정하지 않았습니다.


---

<a id="hist-20261125-001"></a>
### 2026-11-25 01:15:12 +09:00 — 설치용 APK의 실행 속도 최적화 (`v3.3.9-seionwon`)

| 항목 | 내용 |
| --- | --- |
| ID | `hist-20261125-001` |
| 변경 유형 | 성능 개선 (`app/build.gradle.kts`) |
| 상태 | 로컬 빌드 및 산출물 검증 완료 · 실기기 미측정 · GitHub 릴리스 `v3.3.10-seionwon`으로 배포 |

#### 1. 기능 목적

CodeMemo 실행 탭에서 내려받는 설치용 APK는 디버그 빌드입니다. 지금까지 이 APK는 화면 디코더와 암호화 코드가 컴파일러 최적화 없이(`-O0`) 만들어졌고, 최적화되지 않은 디버그용 암호화·JPEG 라이브러리에 연결되어 있었습니다. 앱 자체도 Android가 느린 디버그 모드로 실행하는 상태였습니다. 이 비용을 없애 원격 화면 디코딩과 암호화 연결 처리, 앱 전반의 반응을 빠르게 합니다.

#### 2. 여는 위치 및 사용 방법

- CodeMemo **실행 탭**에서 `app/build/outputs/apk/debug/AVNC-v3.3.9-seionwon.apk`를 내려받아 기존 앱 위에 그대로 설치합니다. 패키지 이름(`com.gaurav.avnc.debug`)과 서명 인증서가 `3.3.8-seionwon`과 같아서 서버 프로필과 설정이 유지됩니다.
- 별도 설정은 없습니다. 접속하면 자동으로 적용됩니다.
- 개발 중 디버거, LeakCanary, 계측 테스트의 인라인 모킹이 필요하면 `./gradlew :app:assembleDebug -PavncDebuggable`로 이전과 같은 디버그 빌드를 만듭니다.

#### 3. 변경 내용

- 네이티브 코드(`native-vnc`, `AppleRfb`, `libvncclient`)를 `RelWithDebInfo`(`-O2 -DNDEBUG`)로 빌드하고, 암호화(`libcrypto`, `libssl`)와 JPEG(`libjpeg`) 라이브러리도 최적화된 릴리스판에 연결합니다.
- 디버그 빌드 유형의 `debuggable`을 기본으로 끕니다. Android가 앱을 일반 앱처럼 최적화해 실행하고, LeakCanary의 누수 감시와 힙 덤프, 디버그 전용 검사(`debugCheck`, 셰이더 프로그램 검증)가 실행되지 않습니다.
- 기준 프로필(`assets/dexopt/baseline.prof`)이 APK에 포함됩니다.
- 버전을 `versionCode = 61`, `versionName = "3.3.9-seionwon"`으로 올렸습니다.

#### 4. 검증 결과

- `./gradlew :app:assembleDebug --no-daemon`: 통과. 산출물 `AVNC-v3.3.9-seionwon.apk` (`10,810,628 bytes`, SHA-256 `84362953d25084a46f3b638abc4dd6e657bf77b96858855c9500eb537513d223`).
- 빌드 설정 확인: `arm64-v8a`의 `CMAKE_BUILD_TYPE=RelWithDebInfo`, 컴파일 플래그 `-O2 -g -DNDEBUG`, 연결 라이브러리가 `vcpkg_installed/arm64-android/lib/`(릴리스판)임을 확인했습니다. 이전 빌드는 `-O` 플래그가 없었고 `debug/lib/`에 연결되어 있었습니다.
- APK 확인: `application-debuggable` 표시가 없어졌고, 패키지 이름과 서명 인증서 SHA-256이 `3.3.8-seionwon`과 일치합니다. `libnative-vnc.so`는 `arm64-v8a` 4,273,200 → 3,019,296바이트, `armeabi-v7a` 2,623,836 → 2,032,828바이트입니다.
- `./gradlew :app:testDebugUnitTest :app:compileDebugAndroidTestKotlin --no-daemon`: JVM 단위 테스트 15/15 통과, 계측 테스트 컴파일 통과.

#### 5. 실제 배포 상태 및 제한 사항

- 로컬 소스와 `app/build/outputs/apk/debug/`의 APK에만 반영했습니다. 커밋, `downloads/` 갱신, GitHub 릴리스는 하지 않았습니다.
- 연결된 기기나 에뮬레이터가 없어 실기기 설치, 실제 FPS·지연·배터리 변화는 측정하지 않았습니다. 속도 향상 폭은 수치로 확인하지 못했습니다.
- 계측 테스트 가운데 `mockk` 인라인 모킹을 쓰는 테스트는 `-PavncDebuggable`을 붙여 실행해야 합니다.
- Kotlin 코드 축소(R8)는 기기에서 검증할 수 없어 적용하지 않았습니다.
- 이 변경은 2026-11-25 01:42 +09:00에 `master`에 커밋·푸시해 GitHub 릴리스 [`v3.3.10-seionwon`](https://github.com/seionwon/avnc/releases/tag/v3.3.10-seionwon)으로 배포했습니다(`AVNC-v3.3.10-seionwon.apk`, `10,155,988 bytes`).


