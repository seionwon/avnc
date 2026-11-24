# AVNC Android 릴리스 · seionwon/avnc

<!-- release-notes:start -->
**[Android APK 다운로드 / Download APK](https://github.com/seionwon/avnc/raw/refs/heads/master/downloads/AVNC-v3.3.10-seionwon.apk)** · 9.7 MiB

**3.3.10-seionwon** · Android 5.0 이상 · 디버그 빌드 (Debug)

한 APK에 ARM64와 ARMv7 버전이 함께 들어 있습니다.
<!-- release-notes:end -->

[이 포크의 전체 릴리스 / All releases](https://github.com/seionwon/avnc/releases)

---

<p align="center"><img src="../metadata/en-US/branding/wordmark.svg" alt="AVNC Banner" height="160"></p>

<p align="center">
  <b>AVNC (macOS Enhanced Fork)</b> — Native macOS Screen Sharing (RFB 3.889) Multi-Display Selection &amp; Korean/English Input Support
</p>

<p align="center">
  <a href="../docs/MACOS_GUIDE.md"><b>English Guide</b></a> ·
  <a href="../docs/MACOS_GUIDE.ko.md"><b>한국어 가이드</b></a> ·
  <a href="../Readme.md"><b>Original Upstream README</b></a>
</p>

---

> [!NOTE]
> This fork preserves the original upstream [`Readme.md`](../Readme.md) untouched and uses `.github/README.md` so visitors immediately see what changed in this version on GitHub.
>
> 이 포크는 원본 [`Readme.md`](../Readme.md)를 수정하지 않고 그대로 유지하면서, GitHub 저장소 첫 화면에서 변경 사항을 바로 볼 수 있도록 `.github/README.md`로 안내를 제공합니다.

---

## What Changed in This Fork / 이번에 추가·개선된 핵심 기능

### 1. macOS Multi-Monitor & Single-Display Selection (`RFB 3.889`) / 다중 모니터 서버에서 단일 모니터 선택
Connect to a macOS host with up to **6 monitors** and view **only the single display you want** (`Display 1`–`Display 6`, single-display selection only):
- **Native Apple RFB 3.889 Engine** (`AppleRfb.h` / `AppleRfb.cpp`): Automatically detects macOS Screen Sharing (`RFB 003.889`), authenticates with Apple SRP (36) / RSA (33) / DH (30), activates the AES-128-CBC + SHA-1 record layer (`1103`), and falls back cleanly to standard LibVNCClient for non-Apple or VncAuth-only servers.
- **Single-Display Selection (Up to 6 Monitors)**: Supports `Standard (Physical displays)` and `High Performance: 1 Virtual Display` (`0x1d`), parses `1105` (`DisplayLayout`), and lets you select **one single display out of up to 6** (**Display 1** through **Display 6**) via `0x0d` (`SetDisplay`) both automatically on connect (Profile Editor → Advanced) and live during a session (Toolbar → Display button).

최대 **6대의 모니터**가 연결된 맥(macOS Screen Sharing)에서 **그중 원하는 1개의 디스플레이(`디스플레이 1`~`디스플레이 6`)만 골라 접속**할 수 있습니다:
- **네이티브 Apple RFB 3.889 구현**: macOS 서버(`RFB 003.889`) 자동 감지, SRP(36)/RSA(33)/DH(30) 계정 인증, AES-128-CBC 암호화 레코드 계층 지원(일반 VNC 서버는 기존 LibVNCClient로 자동 폴백).
- **단일 디스플레이 전용 선택 (최대 6개)**: 프로필 고급 설정에서 **macOS 디스플레이 모드**(표준 / 가상 디스플레이 1개)와 **초기 표시 디스플레이**(`디스플레이 1`~`디스플레이 6`)를 지정하고, 접속 중 툴바의 디스플레이 버튼에서 최대 6개 중 1개 모니터를 실시간으로 전환할 수 있습니다.

👉 Full details: **[English Guide — Multi-Display Selection](../docs/MACOS_GUIDE.md#1-macos-multi-monitor--single-display-selection-rfb-3889)** | **[한국어 가이드 — 다중 모니터 단일 선택](../docs/MACOS_GUIDE.ko.md#1-macos-다중-모니터-서버에서-단일-디스플레이-선택-rfb-3889)**

---

### 2. Reliable Korean (Hangul) & English Input on macOS / macOS 한글·영문 입력 문제 근본 해결
Fixes broken Korean composition, dropped Hangul, `ㅁㅠㅊ` / `gksrmf` language mismatches, and clipboard paste races when typing from Android on-screen keyboards (Samsung Keyboard, Gboard, etc.) to macOS:
- **Full Android IME Editor Sync (`InputView.kt`)**: Proper `Selection(0, 0)` initialization, `EditorInfoCompat.setInitialSurroundingText`, batched edits, `InputMethodManager.updateSelection(...)` reporting on every edit, `BreakIterator`-aware Backspace, and clean flush + `restartInput` on focus/tap transitions so Samsung Keyboard & Gboard compose Hangul syllables reliably.
- **No Double-IME Conflict + Direct `0x04` Unicode Hangul**: Monitors macOS input source (`1109 KeyboardSource`) and keeps macOS on a non-IME keylayout (`ABC`/`US`) via `MacKeyboardSync` when using Android's soft keyboard, allowing `ScreensharingAgent` to type **both Hangul and English directly via atomic `0x04` Unicode diffs (`0x01000000 | codepoint`)** with zero clipboard hacks.
- **Atomic `PasteText` Fallback (`0x1f` + `0x04 Cmd+V`)**: Fixes `0x15` (`AutoPasteboard`) capability detection, packs transient `0x1f` pasteboard (`org.nspasteboard.TransientType`) + `0x04 Cmd+V` into a single atomic write with echo suppression and automatic clipboard restoration when macOS is in an IME or sending non-BMP emoji, while keeping physical hardware keyboard language-switch keys independent.

안드로이드 스크린 키보드(삼성 키보드·Gboard 등)로 맥에 한글/영문을 입력할 때 발생하던 자모 깨짐, 누락, `ㅁㅠㅊ`/`gksrmf` 오입력, 클립보드 충돌을 근본적으로 해결했습니다:
- **Android IME 조합·선택 상태 동기화 (`InputView.kt`)**: `Selection(0, 0)` 초기화, `EditorInfoCompat.setInitialSurroundingText`, 배치 편집 및 매 편집마다 `imm.updateSelection(...)` 보고, `BreakIterator` 기반 백스페이스 처리로 삼성 키보드와 Gboard의 한글 조합을 정상화했습니다.
- **이중 IME 충돌 제거 & 직접 `0x04` 유니코드 한글 입력**: `1109 KeyboardSource`와 `MacKeyboardSync`로 Android 스크린 키보드 사용 시 맥을 비-IME(`ABC`/`US`) 상태로 유지해, 백스페이스와 완성 음절을 단일 패킷(`appleRfbSendTextDiff`, `0x01000000 | cp`)으로 지연 없이 직접 입력합니다.
- **원자적 `PasteText` 폴백**: `0x15`(`AutoPasteboard`) 능력 판정을 보완하고, 맥이 아직 IME 상태이거나 이모지를 보낼 때만 조합 완료 텍스트를 `0x1f` Transient 페이스트보드 + `0x04 Cmd+V` 단일 패킷으로 안전하게 전달한 뒤 기존 맥 클립보드를 복원합니다.

👉 Full details: **[English Guide — Korean & English Input](../docs/MACOS_GUIDE.md#2-korean-hangul--english-input-on-macos)** | **[한국어 가이드 — 한글·영문 입력 해결](../docs/MACOS_GUIDE.ko.md#2-안드로이드-스크린-키보드-한글영문-입력-문제-근본-해결)**

---

### 3. Framebuffer Sync & Touch Delivery Fixes / 화면 갱신 정지 방지 & 터치 정확도 개선
- **2-Stage Framebuffer Stall Watchdog**: Detects stalled macOS `0x09` auto-push after input (`180ms` incremental `0x03` recovery → `650ms` full-frame refresh escalation → automatic continuous incremental pull promotion + `1.5s` idle keep-fresh), ensuring live screen updates even with multiple simultaneous clients.
- **Accurate macOS Touch & Pointer Events**: Uses the active display's `pointerScale`, sends pre-move `0x05(mask=0)` + `0x10` encrypted pointer events with `clickCount` (`1..7`) and `25ms` click hold, keeps scroll wheel pulses (`0x08`/`0x10`) on `0x05`, and synchronizes Android API 30+ keyboard insets with the OpenGL viewport.

👉 Full details: **[English Guide — Framebuffer & Touch Fixes](../docs/MACOS_GUIDE.md#3-framebuffer-reliability--touchpointer-accuracy)** | **[한국어 가이드 — 화면 갱신 & 터치 개선](../docs/MACOS_GUIDE.ko.md#3-화면-갱신-멈춤-방지-및-터치포인터-정확도-개선)**

---

## Documentation Index / 문서 목록

| Document / 문서 | Description / 설명 |
| --- | --- |
| [`docs/MACOS_GUIDE.md`](../docs/MACOS_GUIDE.md) | Complete English user guide, protocol architecture, troubleshooting, and verification details |
| [`docs/MACOS_GUIDE.ko.md`](../docs/MACOS_GUIDE.ko.md) | 한국어 상세 사용자 가이드, 프로토콜·아키텍처 해설, 문제 해결 및 검증 내역 |
| [`docs/기능 추가 history.md`](../docs/기능%20추가%20history.md) | Chronological change & build history (Korean) / 시간순 기능 추가·수정 이력 |
| [`Readme.md`](../Readme.md) | Original upstream AVNC README (untouched) / 원본 AVNC README (원형 유지) |

---

## Quick Start (macOS Single Display) / 빠른 사용법

1. **Profile Editor → Advanced** (`서버 프로필 편집 → 고급`):
   - **Host / Port**: Your Mac's IP/hostname and port `5900`
   - **Username / Password**: Your macOS account username and password (**Security**: `Auto` or `macOS/ARD`)
   - **macOS Display Mode**: `Standard (Physical displays)` or `High Performance: 1 Virtual Display`
   - **Initial Display**: `Display 1` – `Display 6` (single display only)
2. Connect and switch between any single display (1–6) anytime from **Toolbar → Display** (`툴바 → 디스플레이`).
3. Type Korean and English directly with your Android keyboard's built-in **한/영** toggle.

---

## Credits
Based on upstream **[AVNC](https://github.com/gujjwal00/avnc)** by Gaurav Ujjwal and contributors (GPL-3.0-or-later). See [`Readme.md`](../Readme.md) and [`COPYING.txt`](../COPYING.txt) for upstream details and license.
