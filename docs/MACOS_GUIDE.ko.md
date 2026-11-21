# AVNC macOS 기능 확장 가이드 (다중 모니터 단일 선택 & 한글·영문 입력)

> 🌐 **언어**: [English (`MACOS_GUIDE.md`)](MACOS_GUIDE.md) | **한국어**
> 📄 **원본 프로젝트**: [원본 AVNC `Readme.md`](../Readme.md) (수정 없이 원형 유지)

이 문서는 Android용 **AVNC** 포크에 추가한 **macOS 내장 화면 공유(`screensharingd`, RFB `003.889`) 전용 기능과 버그 수정**을 정리한 사용자·기술 가이드입니다.

1. **macOS 다중 모니터 서버에서 단일 디스플레이 선택 (`RFB 3.889`)**
2. **안드로이드 스크린 키보드 한글·영문 입력 문제 근본 해결**
3. **화면 갱신 멈춤 방지 및 터치·포인터 정확도 개선**

---

## 1. macOS 다중 모니터 서버에서 단일 디스플레이 선택 (`RFB 3.889`)

### 배경
외장 모니터가 여러 대 연결된 맥의 내장 VNC(화면 공유) 서버에 일반 VNC 클라이언트로 접속하면 모든 모니터가 가로로 이어 붙은 하나의 거대한 화면으로 표시됩니다. 반면 맥 내장 **화면 공유(Screen Sharing)** 앱은 **고성능(High Performance) → 가상 디스플레이 2개(2 Virtual Displays) → 보기(View) → 디스플레이 1(Display 1)** 선택으로 원하는 모니터 1개만 깔끔하게 표시합니다.

기존 LibVNCClient는 표준 RFB `3.8`만 지원하므로 Apple 전용 확장인 가상 디스플레이 구성(`0x1d`), 디스플레이 레이아웃 수신(`1105`), 단일 디스플레이 전환(`0x0d`)을 처리할 수 없었습니다.

### 사용 방법
1. 맥에서 **시스템 설정 → 일반 → 공유 → 화면 공유**를 켭니다.
2. AVNC에서 **서버 프로필 추가 / 편집 → 고급(Advanced)**을 엽니다.
   - **호스트 / 포트**: 맥의 IP 또는 호스트명, 포트 `5900`
   - **보안(Security)**: `자동(Auto)`(권장) 또는 `macOS / ARD` (타입 `30`)
   - **사용자 이름 / 암호**: 맥 로그인 계정 정보 (RFB `3.889` 디스플레이 확장 기능은 Apple 계정 인증 시 활성화되며, VNC 전용 암호만 쓰는 서버는 표준 LibVNCClient로 자동 폴백합니다.)
   - **macOS 디스플레이 모드**:
     - `표준 (물리 디스플레이)` — 맥에 연결된 물리 모니터 사용
     - `고성능: 가상 디스플레이 1개` — 가상 디스플레이 1개 생성 요청(`0x1d`)
     - `고성능: 가상 디스플레이 2개` — 가상 디스플레이 2개 생성 요청(`0x1d`, 맥 내장 화면 공유의 고성능 2 가상 디스플레이와 동일)
   - **초기 표시 디스플레이**:
     - `모든 디스플레이`, `디스플레이 1`, `디스플레이 2` 등 선택 가능 — 접속 직후 선택한 디스플레이로 자동 전환(`0x0d`)
3. **접속 중 실시간 디스플레이 전환**:
   - 뷰어 화면에서 **툴바**를 열고 **디스플레이(Display)** 아이콘(`ic_displays`)을 탭합니다.
   - 해상도와 `주(Main)` / `가상(Virtual)` 표시를 보며 **모든 디스플레이 / 디스플레이 1 / 디스플레이 2**를 즉시 전환할 수 있습니다.
4. **`vnc://` URI 지원**:
   - 프로필 내보내기/가져오기 및 `vnc://` URI에서 `AppleDisplayMode`(`0`/`1`/`2`)와 `AppleInitialDisplay`(`0`/`1`/`2`…) 파라미터를 그대로 보존합니다.

### 두 클라이언트 동시 접속 시 동작
같은 맥에 두 대의 클라이언트가 동시에 접속하면:
- macOS `screensharingd`는 **하나의 데스크톱 세션(창 포커스, 입력 프롬프트, 커서)**을 공유합니다.
- 어느 모니터를 표시할지(`디스플레이 1`, `디스플레이 2`, `모든 디스플레이`)는 **클라이언트마다 독립적으로 선택**할 수 있습니다.
- 양쪽 클라이언트 모두 같은 디스플레이(예: 둘 다 `디스플레이 1`)를 선택하면 실시간으로 동일한 화면을 보게 됩니다.

### 아키텍처 및 프로토콜 구현 (`AppleRfb.h` / `AppleRfb.cpp`)
- **비파괴 프로토콜 감지**: 소켓 연결 직후 `appleRfbPeekIsAppleBanner()`가 `MSG_PEEK`로 배너 12바이트를 확인해 `RFB 003.889\n`(또는 `003.888\n`)이면 네이티브 `AppleRfbClient`로 진입하고, 일반 VNC 서버나 VncAuth 전용 서버는 기존 LibVNCClient 경로로 매끄럽게 폴백합니다.
- **Apple 인증 & 암호화 레코드 계층**:
  - OpenSSL 3 / LibreSSL 기반으로 **Apple SRP(36)**, **RSA-SRP / RSA1-Plain(33)**, **Apple DH(30)** 인증을 C++ STL 없이(`-DANDROID_STL=none`) 구현했습니다.
  - `ViewerInfo(0x21)` → `SetMode(0x0a)` → `AutoPasteboard(0x15)` → `SetEncryption(0x12)` → `1103(0x44f) Rekey` 교환으로 AES-128-CBC + SHA-1 시퀀스 검증 레코드 계층을 활성화합니다.
- **가상 디스플레이 & 레이아웃 파싱**:
  - `SetEncodings` 이전에 `0x1d`(`SetDisplayConfiguration`)를 전송하며, 서버가 광고한 최대 가상 디스플레이 수(`serverFlags >> 5`) 범위로 안전하게 보정합니다.
  - 서버의 `1105(0x451) DisplayLayout`을 파싱해 프레임버퍼 크기, `currentDisplayId`, 논리/백킹 해상도, 밀도, `scale`, 플래그를 갱신하고, 프로필의 `initialDisplayIndex`를 `0x0d`(`SetDisplay`)로 자동 적용합니다.
- **DB 마이그레이션**: Room `MainDb`를 스키마 `v7 → v8`([`8.json`](../app/roomSchema/com.gaurav.avnc.model.db.MainDb/8.json))로 마이그레이션해 `appleDisplayMode`, `appleInitialDisplay` 컬럼(기본값 `0`)을 추가했습니다.

---

## 2. 안드로이드 스크린 키보드 한글·영문 입력 문제 근본 해결

### 기존에 한글 입력이 깨지던 5가지 근본 원인
안드로이드 스마트폰/태블릿에서 macOS VNC로 한글과 영문을 입력할 때 다음 5가지 문제가 복합적으로 작용했습니다.

1. **구형 X11 한글 키심 무시**: 기존 AVNC는 더미 `BaseInputConnection(this, false)` 위에서 한글을 구형 X11 한글 키심(`0x0ea1..`)으로 변환해 보냈는데, macOS `ScreensharingAgent`는 이를 인식하지 못하고 상위 워드가 `0x0100`인 BMP 유니코드 키심(`0x01000000 | utf16_code_unit`)만 처리합니다.
2. **Android IME 선택·조합 상태 미보고(삼성 키보드·Gboard 한글 조합 붕괴)**: 풀 에디터 `BaseInputConnection(this, true)`는 `Selection(0, 0)` 초기화, `EditorInfo.initialSelStart/End` + `EditorInfoCompat.setInitialSurroundingText`, 배치 편집, 그리고 매 편집마다 `InputMethodManager.updateSelection(view, selStart, selEnd, compStart, compEnd)` 보고가 필수입니다. 이를 빠뜨리면 삼성 키보드·Gboard가 조합 중인 한글 영역(`ㅎ → 하 → 한`)을 추적하지 못해 글자가 누락·중복되거나 자모가 분리됩니다.
3. **이중 IME 충돌 (`Android 한글 IME` vs `macOS 두벌식 IME`)**: 안드로이드 스크린 키보드로 한글을 입력할 때 맥까지 `2SetKorean` IME로 전환하면 두 입력기가 충돌합니다. 맥이 IME 상태(`isInputMethod == true`)일 때는 `ScreensharingAgent`가 `0x04` 키 이벤트를 macOS TSM으로 넘겨 `0x0100XXXX` 한글 키심이 드롭되고 영문도 한글 자모(`abc` → `ㅁㅠㅊ`)로 변질됩니다.
4. **`0x1f` 능력 비트 오판**: `ServerInit.serverCommands`에서 `0x1f` 비트만 검사하면, `0x15`(`AutoPasteboard`)로 클립보드 공유를 광고하는 실제 macOS 서버에서 `0x1f` 폴백이 비활성화되는 문제가 있었습니다.
5. **자모 단위 비원자적 `Cmd+V` 경쟁**: 조합 중인 자모(`ㅎ`, `하`, `한`)마다 `BackSpace` + `0x1f` + 4개의 분리된 `0x10 Cmd+V`를 연속 전송하면 macOS `NSPasteboard`(`pboard`) IPC와 경쟁해 이전 클립보드 내용이 붙거나 글자가 깨졌습니다.

### 해결 아키텍처

```text
Android 스크린 키보드 (삼성 키보드 / Gboard / 네이버 스마트보드 등)
  │  안드로이드 기기 내에서 한글 조합 및 한/영 전환 즉시 처리 (지연 0ms)
  ▼
InputView (BaseInputConnection(this, true))
  │  • Selection(0, 0) + EditorInfoCompat.setInitialSurroundingText 초기화
  │  • batch { ... } + 매 편집마다 imm.updateSelection(...) 보고
  │  • BreakIterator 기반 deleteBackward() + 화면 터치/엔터 시 조합 flush 후 restartInput()
  ▼
Dispatcher / MacKeyboardSync (macOS 1109 KeyboardSource 실시간 추적)
  ├─ macOS가 비-IME 레이아웃(ABC / US)일 때 [기본 고속 경로 — 접속 시 자동 전환]
  │     └─► appleRfbSendTextDiff(backspaces, utf8)
  │           단일 원자 0x04 streamWrite:
  │             [N × XK_BackSpace] + [한글 BMP 0x01000000 | cp, 영문 ASCII]
  │           → 클립보드/Cmd+V 미사용, 터미널·Chrome·Notes·Spotlight 모든 앱에서 즉시 입력!
  │
  └─ macOS가 IME(2SetKorean) 상태이거나 이모지(non-BMP)일 때 [안전 폴백 경로]
        ├─► InputView가 조합 완료 텍스트만 전송 (prefersCommittedTextOnly)
        ├─► appleRfbPasteText(utf8):
        │     단일 원자 streamWrite: [0x1f Transient 페이스트보드] + [0x04 Cmd+V × 4]
        │     (서버가 0x15 또는 0x1f 광고 시 활성화, 0x14 cmd=2 에코 억제,
        │      입력 종료 1.2초 후 기존 맥 클립보드 자동 복원)
        └─► MacKeyboardSync가 백그라운드에서 macOS를 ABC/US로 전환해 이후 고속 경로 복귀
```

#### 핵심 구성 요소
- **`RemoteKeyboardSource.kt` & `AppleRfb.cpp` (`1109` / `0x1a` / `0x10`)**: macOS `1109(0x455) KeyboardSource`를 파싱해 현재 입력 소스 ID와 보안 입력(`secureInput`, 암호창) 여부를 추적하고, 비-IME 레이아웃(`ABC`/`US`)과 IME(`2SetKorean`/`3SetKorean`/구름 입력기)를 구분합니다.
- **`MacKeyboardSync.kt`**: `TOGGLING → SELECTING → LEAVING → RETURNING → PINNING` 5단계 상태 머신으로 `Control+Space`(`0x10`), `0x1a SelectKeyboardSource + VoidSymbol`, `Control+Space`가 꺼진 맥을 위한 `0x1a` 고정 폴백(`PINNING`), 암호 입력창(`secureInput`) 전환 보류를 처리합니다.
  - 접속 시 및 안드로이드 스크린 키보드 사용 시 맥을 `ABC`/`US`로 유지해 안드로이드 스크린 키보드의 한/영 전환이 지연 없이 그대로 반영되도록 했습니다.
  - 외장 블루투스/USB 물리 키보드(`event.deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD`)는 하드웨어 한/영 키(`KEYCODE_LANGUAGE_SWITCH`, `KEYCODE_KANA`, `KEYCODE_EISU`)로 macOS 자체 IME를 토글해 사용할 수 있습니다.
- **`appleRfbSendTextDiff()` & `appleRfbPasteText()`**:
  - `appleRfbSendTextDiff()`는 백스페이스(`XK_BackSpace`)와 BMP 유니코드 키심(`0x01000000 | cp`, `\r\n`은 단일 `XK_Return`으로 정규화)을 **단일 암호화 레코드**로 묶어 보내므로 `ㅎ → 하 → 한` 실시간 한글 조합이 끊김 없이 표시됩니다.
  - `supportsApplePasteboard()`가 `serverAccepts(0x1f) || serverAccepts(0x15)`를 모두 인정해 실제 macOS 서버에서도 `0x1f` 폴백이 확실히 작동합니다.
  - `appleRfbPasteText()`는 `0x1f` Transient 페이스트보드(`public.utf8-plain-text` + `org.nspasteboard.TransientType`)와 `0x04 Cmd+V`(`0xffeb`, `'v'`)를 **단일 `streamWrite`**로 원자 전송하고, 3초간 자체 에코(`0x14 cmd=2`)를 억제한 뒤 1.2초 후 기존 맥 클립보드를 복원합니다.
- **`VirtualKeys.kt`**: AVNC 가상 키보드의 텍스트 입력창(`handleTextBoxAction`)도 Apple 세션에서는 `inputHandler.onTextInput(0, text)` 경로를 직접 타도록 통합해 한글·영문 혼합 입력이 동일하게 작동합니다.

### 사용 참고 사항
- **삼성 키보드 & Gboard**: 평소처럼 안드로이드 키보드의 **한/영** 키로 전환하며 입력하면 됩니다. 맥이 `ABC`/`US` 상태일 때 한글 자모 조합이 실시간으로 화면에 반영됩니다.
- **암호 입력창(`secureInput == true`)**: macOS가 보안 입력 상태를 알리면 입력 소스 강제 전환을 보류하고 클립보드 붙여넣기를 사용하지 않습니다.
- **이모지(non-BMP)**: macOS `0x04` 키 이벤트는 16비트 BMP `UniChar`만 지원하므로 이모지는 자동으로 원자적 `PasteText` 경로로 전달됩니다.

---

## 3. 화면 갱신 멈춤 방지 및 터치·포인터 정확도 개선

### 화면 갱신 정지(프레임 멈춤) 복구
- **Rekey 도중 `DisplayLayout` 재무장**: `pendingRekey` 상태에서 `1105 DisplayLayout`이 먼저 도착한 경우에도 레코드 계층 활성화 직후 초기 디스플레이 선택 및 `sendUpdateArm(true)`가 누락되지 않도록 수정했습니다.
- **2단계 프레임 정지 워치독 (`maybeKickStalledFramebuffer`)**:
  - `250ms` 주기로 폴링하며, Sender/Receiver 스레드 간 입력 활동 플래그를 lock-free atomic(`__atomic_load_n`/`__atomic_store_n`)으로 안전하게 공유합니다.
  - 키·터치·텍스트 입력 후 **`180ms`** 안에 픽셀 갱신이 없으면 즉시 증분 `0x03` + `0x09` 복구를 요청합니다.
  - 증분 복구 후 **`650ms`** 동안 여전히 픽셀이 오지 않으면 **비증분 전체 갱신(`0x03`, `incremental = 0`)**을 1회 에스컬레이션해 macOS가 현재 화면 전체를 재전송하게 합니다.
  - `0x09` 자동 푸시 정지가 2회 감지되면 세션 전체를 **연속 증분 `0x03` Pull 모드**로 자동 승격하며, 유휴 상태에서도 **`1.5초`**마다 10바이트 증분 `0x03`을 요청해 다른 클라이언트나 원격 애니메이션 변화가 항상 반영됩니다.
- **Zlib & Android 레이아웃 동기화**: `zlibDecoderInflate`의 `avail_in == 0 && avail_out == 0` 경계 조건을 수정하고, API 30+ `LayoutManager.hookInsetsListener`의 `applyInsets()` 즉시 호출, `Renderer.onSurfaceChanged()`의 뷰포트 크기 동기화, 키보드 표시 중 가상 디스플레이 재구성(`0x1d`) 방지를 적용했습니다.

### 터치·포인터 전달 정확도
- **선택 디스플레이 `pointerScale` 적용**: `displays[0]` 대신 현재 선택된 디스플레이(`currentDisplayId`)의 `scale`을 사용하고, 수신 커서 좌표(`1100`/`-232`)의 중복 스케일링을 제거했습니다.
- **Apple `0x10` 암호화 포인터 이벤트**: 새 좌표 탭 시 `0x05(mask = 0)` 커서 사전 이동 후 정확한 `clickCount(1..7)`가 담긴 `0x10` 암호화 포인터 메시지와 `25ms` 클릭 홀드 간격(`insertAppleClickHoldDelay`)으로 클릭·더블클릭·포커스 전환을 안정화했습니다.
- **스크롤 휠 `0x05` 분리**: 스크롤 휠 비트(`0x08`/`0x10`/`0x20`/`0x40`)는 `screensharingd` 규격대로 `0x05` `PointerEvent`로만 보내 `0x10` `clickCount`를 오염시키지 않습니다.

---

## 4. 주요 추가·수정 파일

| 영역 | 파일 |
| --- | --- |
| 네이티브 Apple RFB 3.889 | [`AppleRfb.h`](../app/src/main/cpp/AppleRfb.h), [`AppleRfb.cpp`](../app/src/main/cpp/AppleRfb.cpp), [`native-vnc.cpp`](../app/src/main/cpp/native-vnc.cpp), [`ClientEx.h`](../app/src/main/cpp/ClientEx.h), [`Cursor.h`](../app/src/main/cpp/Cursor.h), [`app/CMakeLists.txt`](../app/CMakeLists.txt) |
| 디스플레이 선택 & 저장 | [`RemoteDisplay.kt`](../app/src/main/java/com/gaurav/avnc/vnc/RemoteDisplay.kt), [`ServerProfile.kt`](../app/src/main/java/com/gaurav/avnc/model/ServerProfile.kt), [`MainDb.kt`](../app/src/main/java/com/gaurav/avnc/model/db/MainDb.kt), [`8.json`](../app/roomSchema/com.gaurav.avnc.model.db.MainDb/8.json), [`VncUri.kt`](../app/src/main/java/com/gaurav/avnc/vnc/VncUri.kt), [`ProfileEditor.kt`](../app/src/main/java/com/gaurav/avnc/ui/home/ProfileEditor.kt), [`Toolbar.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/Toolbar.kt) |
| 한글·영문 입력 & 키보드 동기화 | [`RemoteKeyboardSource.kt`](../app/src/main/java/com/gaurav/avnc/vnc/RemoteKeyboardSource.kt), [`MacKeyboardSync.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/MacKeyboardSync.kt), [`InputView.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/InputView.kt), [`InputHandler.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/InputHandler.kt), [`Dispatcher.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/Dispatcher.kt), [`KeyHandler.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/KeyHandler.kt), [`VirtualKeys.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/VirtualKeys.kt) |
| 세션·렌더링·포인터 | [`VncClient.kt`](../app/src/main/java/com/gaurav/avnc/vnc/VncClient.kt), [`RemoteSession.kt`](../app/src/main/java/com/gaurav/avnc/session/RemoteSession.kt), [`Messenger.kt`](../app/src/main/java/com/gaurav/avnc/session/Messenger.kt), [`VncViewModel.kt`](../app/src/main/java/com/gaurav/avnc/viewmodel/VncViewModel.kt), [`VncActivity.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/VncActivity.kt), [`LayoutManager.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/LayoutManager.kt), [`Renderer.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/gl/Renderer.kt), [`PointerModes.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/PointerModes.kt) |
| 테스트 & 빌드 | [`RemoteDisplayTest.kt`](../app/src/test/java/com/gaurav/avnc/vnc/RemoteDisplayTest.kt), [`MacKeyboardSyncTest.kt`](../app/src/test/java/com/gaurav/avnc/vnc/MacKeyboardSyncTest.kt), [`app/build.gradle.kts`](../app/build.gradle.kts), [`build.gradle.kts`](../build.gradle.kts) |

---

## 5. 빌드 및 검증

```bash
# Android JVM 단위 테스트 실행 (RemoteDisplayTest + MacKeyboardSyncTest)
./gradlew :app:testDebugUnitTest --no-daemon

# Android 계측 테스트 컴파일 확인
./gradlew :app:compileDebugAndroidTestKotlin --no-daemon

# 디버그 APK 빌드 (출력: app/build/outputs/apk/debug/AVNC-v<versionName>.apk)
./gradlew :app:assembleDebug --no-daemon
```

로컬 검증 결과:
- **네이티브 C++ 단위·E2E 테스트** (`AppleRfb.cpp`, OpenSSL 3 + zlib, `-Wall -Wextra -Werror`): **9/9 통과** (와이어 빌더/파서, `MSG_PEEK`, DH/SRP/RSA/NoAuth/VncAuth 폴백, 가상 디스플레이 + 디스플레이 1 자동 선택, `1109`/`0x1a`/`0x10` 키보드·포인터, 스크롤 휠 `0x05`, `0x15` 단독 서버의 원자적 `0x1f + 0x04 Cmd+V` + 클립보드 복원, 원자적 `appleRfbSendTextDiff`, 2단계 프레임 정지 워치독).
- **Android JVM 단위 테스트**: **12/12 통과** (`MacKeyboardSyncTest` 9/9, `RemoteDisplayTest` 3/3).
- **Android 계측 테스트 컴파일 및 디버그 APK 빌드**: 통과 (`app/build/outputs/apk/debug/AVNC-v3.3.5.apk`).
