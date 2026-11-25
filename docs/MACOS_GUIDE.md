# AVNC macOS Enhancements Guide (Multi-Display Selection & Korean/English Input)

> 🌐 **Language**: **English** | [한국어 (`MACOS_GUIDE.ko.md`)](MACOS_GUIDE.ko.md)
> 📄 **Upstream**: [Original AVNC `Readme.md`](../Readme.md) (preserved untouched)

This document explains the features, protocol design, and bug fixes added in this fork of **AVNC** for connecting from Android to macOS's built-in **Screen Sharing** server (`screensharingd`, RFB `003.889`):

1. **macOS Multi-Monitor & Single-Display Selection (`RFB 3.889`)**
2. **Reliable Korean (Hangul) & English Input from Android Soft Keyboards**
3. **Framebuffer Stall Recovery & Touch/Pointer Accuracy**

---

## 1. macOS Multi-Monitor & Single-Display Selection (`RFB 3.889`)

### Problem
When connecting a standard VNC client to a Mac with multiple external monitors, macOS streams a single wide composite framebuffer combining all physical displays together. By contrast, Apple's built-in macOS Screen Sharing client lets you choose **High Performance → Display Type: 2 Virtual Displays** and then pick **View → Display 1** to work cleanly on a single display.

Standard LibVNCClient speaks plain RFB `3.8` and cannot negotiate Apple's proprietary `RFB 003.889` display configuration (`0x1d`), display layout (`1105`), or single-display switching (`0x0d`) messages.

### How to Use It
1. On macOS, enable **System Settings → General → Sharing → Screen Sharing** (or Remote Management).
2. In AVNC, open **Add / Edit Server Profile** and expand **Advanced**:
   - **Host / Port**: Mac IP or hostname, port `5900`.
   - **Security**: `Auto` (recommended) or `macOS / ARD` (type `30`).
   - **Username / Password**: Your macOS login account credentials (Apple account auth is required to activate RFB `3.889` display features; VNC-password-only connections automatically fall back to standard LibVNCClient).
   - **macOS Display Mode**:
     - `Standard (Physical Displays)` — Uses physical monitors attached to the Mac (supports up to 6 monitors; single monitor selected at a time).
     - `High Performance: 1 Virtual Display` — Requests a single virtual display (`0x1d`).
   - **Initial Display**:
     - `Display 1` through `Display 6` — Automatically selects that single display right after connecting (`0x0d`), never showing a combined multi-monitor screen.
3. **Switch Displays Live**:
   - While connected, open the viewer **Toolbar** and tap the **Display** button (`ic_displays`).
   - Choose any single display from **Display 1** through **Display 6** (labeled with each display's resolution).
4. **`vnc://` URI Support**:
   - Profiles exported or opened via URI also preserve `AppleDisplayMode` (`0` = Standard, `1` = 1 Virtual Display) and `AppleInitialDisplay` (`1`–`6`).

### Multiple Simultaneous Clients on the Same Mac
When two VNC clients connect to the same macOS host at once:
- macOS `screensharingd` shares **one desktop session** (active window focus, text input prompt, and cursor) across connected viewers.
- Each client independently chooses **which single display to view** (`Display 1`–`Display 6`).
- If both clients select the same display (e.g., both on `Display 1`), both screens stay synchronized in real time.

### Architecture & Wire Protocol (`AppleRfb.h` / `AppleRfb.cpp`)
- **Non-Destructive Protocol Detection**: Before handing the socket to LibVNCClient, `appleRfbPeekIsAppleBanner()` peeks 12 bytes with `MSG_PEEK`. If the server advertises `RFB 003.889\n` (or `003.888\n`) and offers Apple authentication, AVNC activates the native `AppleRfbClient` engine; otherwise, non-Apple or VncAuth-only servers proceed transparently through LibVNCClient.
- **Authentication & Encrypted Record Layer**:
  - Supports Apple **Direct SRP (36)**, **RSA-SRP / RSA1-Plain (33)**, and **Apple DH (30)** (plus NoAuth `1` for local mock testing), built with OpenSSL 3 / LibreSSL and zero C++ STL (`-DANDROID_STL=none`).
  - Negotiates `ViewerInfo (0x21)`, `SetMode (0x0a)`, `AutoPasteboard (0x15)`, `SetEncryption (0x12)`, and `1103 (0x44f) Rekey` to establish Apple's AES-128-CBC + SHA-1 sequence-tagged record layer.
- **Single Virtual Display & Up-to-6 Display Selection**:
  - Sends `0x1d` (`SetDisplayConfiguration`, `display_count = 1`) when `1 Virtual Display` is requested, but only after the first `1105 DisplayLayout` shows no virtual display in use. Virtual displays are one configuration shared by every viewer, so if another viewer already has them AVNC joins without creating or resizing anything, and it stops resizing if another viewer later replaces the configuration. A viewer in standard mode cannot be detected; use the `Standard` profile when sharing the Mac with one.
  - Parses server `1105 (0x451) DisplayLayout` rectangles (framebuffer size, active `currentDisplayId`, logical/backing bounds, density, `scale`, and display flags), exposes up to **6 selectable displays**, and automatically selects the configured single display (`1..6`, falling back to `Display 1`) via `0x0d` (`SetDisplay`).
- **Database Migration**: Room `MainDb` migrated from schema `v7` → `v8` ([`8.json`](../app/roomSchema/com.gaurav.avnc.model.db.MainDb/8.json)) adding `appleDisplayMode` and `appleInitialDisplay` with default `0`.

---

## 2. Korean (Hangul) & English Input on macOS

### Why Korean Input Broke Previously (5 Root Causes Fixed)
Typing Korean from an Android phone/tablet to macOS over VNC failed for five separate, compounding reasons:

1. **Legacy X11 KeySyms Ignored by macOS**: Upstream AVNC mapped Hangul characters to legacy X11 Hangul keysyms (`0x0ea1..`) using a dummy `BaseInputConnection(this, false)`. macOS `ScreensharingAgent` does not accept legacy Hangul keysyms — it only recognizes BMP Unicode keysyms with high word `0x0100` (`0x01000000 | utf16_code_unit`).
2. **Broken Android IME State Tracking (Samsung Keyboard & Gboard)**: A real `Editable` buffer (`BaseInputConnection(this, true)`) must initialize `Selection(0, 0)`, populate `EditorInfo.initialSelStart/End` + `EditorInfoCompat.setInitialSurroundingText`, batch updates, and call `InputMethodManager.updateSelection(view, selStart, selEnd, compStart, compEnd)` on every change. Without `updateSelection()`, Samsung Keyboard and Gboard lose track of the composing Hangul span (`ㅎ → 하 → 한`), causing dropped, duplicated, or decomposed jamo.
3. **Double-IME Conflict (`Android Korean IME` vs `macOS 2SetKorean`)**: Switching macOS *into* `com.apple.inputmethod.Korean.2SetKorean` while typing from an Android soft keyboard creates a double-IME war. When macOS is in an Input Method (`isInputMethod == true`), `ScreensharingAgent` routes `0x04` key events through macOS's TSM/IME, which drops `0x0100XXXX` Hangul keysyms and turns English letters into Korean jamo (`abc` → `ㅁㅠㅊ`).
4. **False `0x1f` Capability Gate**: Checking only `serverAccepts(c, 0x1f)` caused Apple pasteboard fallback (`0x1f`) to be disabled on macOS hosts that advertise clipboard sharing via `0x15` (`AutoPasteboard`) without setting bit `0x1f` in `ServerInit.serverCommands`.
5. **Non-Atomic Per-Jamo `Cmd+V` Races**: Sending `BackSpace` + `0x1f` + 4 separate `0x10` `Cmd+V` writes on every intermediate jamo stroke (`ㅎ`, `하`, `한`) raced with macOS `NSPasteboard` (`pboard`) IPC, pasting stale clipboard text or losing characters.

### Solution Architecture

```text
Android Soft Keyboard (Samsung Keyboard / Gboard / Naver SmartBoard)
  │  composes Hangul & toggles 한/영 locally (0ms latency)
  ▼
InputView (BaseInputConnection(this, true))
  │  • Selection(0, 0) + EditorInfoCompat.setInitialSurroundingText
  │  • batch { ... } + imm.updateSelection(...) on every edit
  │  • BreakIterator deleteBackward() + flush & restartInput() on tap/Enter
  ▼
Dispatcher / MacKeyboardSync (tracks macOS 1109 KeyboardSource)
  ├─ macOS on non-IME layout (ABC / US) [Normal Path — auto-selected on connect]
  │     └─► appleRfbSendTextDiff(backspaces, utf8)
  │           ONE atomic 0x04 streamWrite:
  │             [N × XK_BackSpace] + [0x01000000 | cp for Hangul BMP, ASCII for English]
  │           → Zero clipboard touch, zero Cmd+V, works in Terminal/Chrome/Notes/Spotlight!
  │
  └─ macOS in Input Method (2SetKorean) or non-BMP Emoji [Safe Fallback Path]
        ├─► InputView holds uncommitted composing text locally (prefersCommittedTextOnly)
        ├─► appleRfbPasteText(utf8):
        │     ONE atomic streamWrite: [0x1f Transient Pasteboard] + [4 × 0x04 Cmd+V]
        │     (enabled whenever server accepts 0x15 OR 0x1f; suppresses 0x14 cmd=2 echo;
        │      restores original Mac clipboard after 1.2s idle)
        └─► MacKeyboardSync gently returns macOS to ABC/US for subsequent typing
```

#### Key Components
- **`RemoteKeyboardSource.kt` & `AppleRfb.cpp` (`1109` / `0x1a` / `0x10`)**: Parses macOS `1109 (0x455) KeyboardSource` updates (active TIS source ID and `secureInput` password-field flag) and classifies keylayouts (`com.apple.keylayout.ABC`, `US`) vs Input Methods (`2SetKorean`, `3SetKorean`, Gureum, etc.).
- **`MacKeyboardSync.kt`**: 5-phase state machine (`TOGGLING → SELECTING → LEAVING → RETURNING → PINNING`) supporting `Control+Space` (`0x10`), `0x1a SelectKeyboardSource + VoidSymbol`, automatic pinning fallback when `Control+Space` is disabled in macOS System Settings, and `secureInput` deferral while typing passwords.
  - On connect and when using Android's soft keyboard, AVNC places macOS on a non-IME keylayout (`ABC`/`US`) so Android's soft keyboard is the single source of truth for both Korean and English.
  - Physical Bluetooth/USB hardware keyboards (`event.deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD`) remain free to toggle macOS's native IME via `KEYCODE_LANGUAGE_SWITCH`, `KEYCODE_KANA`, or `KEYCODE_EISU`.
- **`appleRfbSendTextDiff()` & `appleRfbPasteText()`**:
  - `appleRfbSendTextDiff()` packs deletions (`XK_BackSpace`) and BMP Unicode codepoints (`0x01000000 | cp`, collapsing `\r\n` → single `XK_Return`) into one encrypted RFB write so live Hangul syllable updates (`ㅎ → 하 → 한`) render atomically without flickering or races.
  - `supportsApplePasteboard()` enables `0x1f` whenever `serverAccepts(0x1f) || serverAccepts(0x15)`.
  - `appleRfbPasteText()` packs the `0x1f` zlib-compressed archive (`public.utf8-plain-text` + `org.nspasteboard.TransientType`) and four `0x04` `Cmd+V` events (`0xffeb` Super_L + `'v'`) into a single `streamWrite`, suppresses our own `0x14 cmd=2` pasteboard echo for 3 seconds, and restores the Mac's previous non-transient clipboard after 1.2 seconds of typing idle.
- **`VirtualKeys.kt`**: Routes AVNC's built-in Virtual Keys text box directly through `inputHandler.onTextInput(0, text)` on Apple sessions so Korean and mixed Korean/English strings work identically there too.

### Keyboard Behavior Notes
- **Samsung Keyboard & Gboard**: Switch 한/영 directly on the Android keyboard as usual. Korean syllables compose smoothly and appear live on macOS once macOS is on `ABC`/`US`.
- **Password Fields (`secureInput == true`)**: macOS reports `secureInput = true`; AVNC defers input-source switching and never uses clipboard paste inside password prompts.
- **Emoji (Non-BMP)**: Automatically delivered via atomic `PasteText` since macOS `0x04` key events only encode 16-bit BMP `UniChar` values.

---

## 3. Framebuffer Reliability & Touch/Pointer Accuracy

### Framebuffer Stall Recovery
- **Rekey `DisplayLayout` Re-Arming**: If `1105 DisplayLayout` arrives during `pendingRekey` before `running = true`, AVNC now applies initial display selection and re-arms `sendUpdateArm(true)` immediately once the encrypted record layer activates.
- **2-Stage Stall Watchdog (`maybeKickStalledFramebuffer`)**:
  - Polls every `250ms` and tracks input activity with lock-free atomic flags (`__atomic_load_n` / `__atomic_store_n`) across Sender and Receiver threads.
  - If no pixel update arrives within **`180ms`** after key/pointer/text input, immediately sends an incremental `0x03` + `0x09` recovery request.
  - If incremental recovery still receives zero pixels after **`650ms`**, escalates once to a **non-incremental full refresh** (`0x03`, `incremental = 0`).
  - If macOS `0x09` auto-push stalls twice after input, automatically promotes the session to **continuous incremental `0x03` pull mode**.
  - Sends a lightweight 10-byte incremental `0x03` every **`1.5s`** while idle so remote changes from another client or background animations never freeze on Android.
- **Zlib & Android Layout Sync**: Fixed `zlibDecoderInflate` draining when `avail_in == 0 && avail_out == 0`, triggered `applyInsets()` directly in API 30+ `LayoutManager.hookInsetsListener`, synced `FrameState.setViewportSize()` in `Renderer.onSurfaceChanged()`, and skipped `0x1d` virtual-display resize while the soft keyboard is visible.

### Touch & Pointer Accuracy
- **Active Display `pointerScale`**: Reads `pointerScale` from the currently selected display (`currentDisplayId`) instead of blindly `displays[0]`, and removes duplicate scaling on incoming `1100` / `-232` cursor coordinates.
- **macOS `0x10` Encrypted Pointer Events**: Sends a pre-move `0x05(mask = 0)` before pressing a button at a new tap location, sends mouse button transitions via `0x10` `EncryptedPointerMessage` with accurate `clickCount` (`1..7`) so `screensharingd` registers click/drag ownership cleanly, and inserts a `25ms` click hold delay (`Messenger.insertAppleClickHoldDelay`).
- **Scroll Wheel Separation**: Keeps scroll wheel pulses (`0x08`, `0x10`, `0x20`, `0x40`) strictly on `0x05` `PointerEvent` as required by `screensharingd`, without polluting `0x10` `clickCount`.

---

## 4. Key Files Added / Modified

| Area | Files |
| --- | --- |
| Native Apple RFB 3.889 | [`AppleRfb.h`](../app/src/main/cpp/AppleRfb.h), [`AppleRfb.cpp`](../app/src/main/cpp/AppleRfb.cpp), [`native-vnc.cpp`](../app/src/main/cpp/native-vnc.cpp), [`ClientEx.h`](../app/src/main/cpp/ClientEx.h), [`Cursor.h`](../app/src/main/cpp/Cursor.h), [`app/CMakeLists.txt`](../app/CMakeLists.txt) |
| Display Selection & Persistence | [`RemoteDisplay.kt`](../app/src/main/java/com/gaurav/avnc/vnc/RemoteDisplay.kt), [`ServerProfile.kt`](../app/src/main/java/com/gaurav/avnc/model/ServerProfile.kt), [`MainDb.kt`](../app/src/main/java/com/gaurav/avnc/model/db/MainDb.kt), [`8.json`](../app/roomSchema/com.gaurav.avnc.model.db.MainDb/8.json), [`VncUri.kt`](../app/src/main/java/com/gaurav/avnc/vnc/VncUri.kt), [`ProfileEditor.kt`](../app/src/main/java/com/gaurav/avnc/ui/home/ProfileEditor.kt), [`Toolbar.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/Toolbar.kt) |
| Korean / English Input & Keyboard Sync | [`RemoteKeyboardSource.kt`](../app/src/main/java/com/gaurav/avnc/vnc/RemoteKeyboardSource.kt), [`MacKeyboardSync.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/MacKeyboardSync.kt), [`InputView.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/InputView.kt), [`InputHandler.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/InputHandler.kt), [`Dispatcher.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/Dispatcher.kt), [`KeyHandler.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/KeyHandler.kt), [`VirtualKeys.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/VirtualKeys.kt) |
| Session, Rendering & Pointer | [`VncClient.kt`](../app/src/main/java/com/gaurav/avnc/vnc/VncClient.kt), [`RemoteSession.kt`](../app/src/main/java/com/gaurav/avnc/session/RemoteSession.kt), [`Messenger.kt`](../app/src/main/java/com/gaurav/avnc/session/Messenger.kt), [`VncViewModel.kt`](../app/src/main/java/com/gaurav/avnc/viewmodel/VncViewModel.kt), [`VncActivity.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/VncActivity.kt), [`LayoutManager.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/LayoutManager.kt), [`Renderer.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/gl/Renderer.kt), [`PointerModes.kt`](../app/src/main/java/com/gaurav/avnc/ui/vnc/input/PointerModes.kt) |
| Unit Tests & Build | [`RemoteDisplayTest.kt`](../app/src/test/java/com/gaurav/avnc/vnc/RemoteDisplayTest.kt), [`MacKeyboardSyncTest.kt`](../app/src/test/java/com/gaurav/avnc/vnc/MacKeyboardSyncTest.kt), [`app/build.gradle.kts`](../app/build.gradle.kts), [`build.gradle.kts`](../build.gradle.kts) |

---

## 5. Build & Verification

```bash
# Run Android JVM unit tests (RemoteDisplayTest + MacKeyboardSyncTest)
./gradlew :app:testDebugUnitTest --no-daemon

# Compile Android instrumented tests
./gradlew :app:compileDebugAndroidTestKotlin --no-daemon

# Build debug APK (outputs app/build/outputs/apk/debug/AVNC-v<versionName>.apk)
./gradlew :app:assembleDebug --no-daemon
```

Verified locally:
- **Native C++ unit & E2E tests** (`AppleRfb.cpp` against OpenSSL 3 + zlib with `-Wall -Wextra -Werror`): **9/9 passed** (wire builders/parsers, `MSG_PEEK`, DH/SRP/RSA/NoAuth/VncAuth-fallback, virtual displays + Display 1 auto-select, `1109`/`0x1a`/`0x10` keyboard & pointer, scroll wheel `0x05`, `0x15`-only pasteboard + atomic `0x1f + 0x04 Cmd+V` + clipboard restore, atomic `appleRfbSendTextDiff`, 2-stage framebuffer stall watchdog).
- **Android JVM unit tests**: **12/12 passed** (`MacKeyboardSyncTest`: 9/9, `RemoteDisplayTest`: 3/3).
- **AndroidTest Kotlin compilation & debug APK build**: passed (`app/build/outputs/apk/debug/AVNC-v3.3.5.apk`).
