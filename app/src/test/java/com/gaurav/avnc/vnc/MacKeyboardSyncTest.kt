/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.vnc

import com.gaurav.avnc.ui.vnc.input.MacKeyboardSync
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MacKeyboardSyncTest {

    private class Harness {
        val actions = mutableListOf<String>()
        private var nextTimerId = 1
        private val pendingTimers = linkedMapOf<Int, () -> Unit>()

        val sync = MacKeyboardSync(
                selectSource = { id, clear -> actions.add("select:$id:clear=$clear") },
                toggleSource = { actions.add("toggle") },
                scheduleTimer = { _, action ->
                    val id = nextTimerId++
                    pendingTimers[id] = action
                    ({ pendingTimers.remove(id) })
                }
        )

        fun fireNextTimer(): Boolean {
            val first = pendingTimers.entries.firstOrNull() ?: return false
            pendingTimers.remove(first.key)
            first.value.invoke()
            return true
        }

        fun hasPendingTimers(): Boolean = pendingTimers.isNotEmpty()
    }

    @Test
    fun remoteKeyboardSourceClassification() {
        val abc = RemoteKeyboardSource("com.apple.keylayout.ABC", secureInput = false, supportsSelection = true)
        assertFalse(abc.isInputMethod)
        assertTrue(abc.isEnglish)
        assertFalse(abc.isKorean)
        assertTrue(abc.matches(InputLanguage.ENGLISH))
        assertFalse(abc.matches(InputLanguage.KOREAN))

        val us = RemoteKeyboardSource("com.apple.keylayout.US", secureInput = false, supportsSelection = true)
        assertFalse(us.isInputMethod)
        assertTrue(us.isEnglish)

        val ko2 = RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", secureInput = false, supportsSelection = true)
        assertTrue(ko2.isInputMethod)
        assertTrue(ko2.isKorean)
        assertFalse(ko2.isEnglish)
        assertTrue(ko2.matches(InputLanguage.KOREAN))

        val gureumKo = RemoteKeyboardSource("org.youknowone.inputmethod.Gureum.han2")
        assertTrue(gureumKo.isKorean)
        assertTrue(gureumKo.isInputMethod)

        val gureumEn = RemoteKeyboardSource("org.youknowone.inputmethod.Gureum.qwerty")
        assertTrue(gureumEn.isEnglish)
    }

    @Test
    fun localeTagAndScriptDetection() {
        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectLanguageFromLocaleTag("ko"))
        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectLanguageFromLocaleTag("ko-KR"))
        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectLanguageFromLocaleTag("ko_KR"))
        assertEquals(InputLanguage.ENGLISH, MacKeyboardSync.detectLanguageFromLocaleTag("en"))
        assertEquals(InputLanguage.ENGLISH, MacKeyboardSync.detectLanguageFromLocaleTag("en-US"))
        assertEquals(InputLanguage.ENGLISH, MacKeyboardSync.detectLanguageFromLocaleTag("en_GB"))
        assertNull(MacKeyboardSync.detectLanguageFromLocaleTag(""))
        assertNull(MacKeyboardSync.detectLanguageFromLocaleTag("ja-JP"))

        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectScriptLanguage("ㅎ"))
        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectScriptLanguage("안녕"))
        assertEquals(InputLanguage.KOREAN, MacKeyboardSync.detectScriptLanguage("hello 안녕"))
        assertEquals(InputLanguage.ENGLISH, MacKeyboardSync.detectScriptLanguage("a"))
        assertEquals(InputLanguage.ENGLISH, MacKeyboardSync.detectScriptLanguage("안녕 hello"))
        assertNull(MacKeyboardSync.detectScriptLanguage("1234 !@# "))
    }

    @Test
    fun shouldUsePasteTextDecisions() {
        val abc = RemoteKeyboardSource("com.apple.keylayout.ABC")
        val ko = RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean")
        val secure = RemoteKeyboardSource("com.apple.keylayout.ABC", secureInput = true)

        // Confirmed non-IME keylayout (ABC) or unreported (null): both English and Hangul BMP
        // use fast direct 0x04 Unicode so phone keyboards that compose words never freeze silently.
        assertFalse(MacKeyboardSync.shouldUsePasteText("hello", abc))
        assertFalse(MacKeyboardSync.shouldUsePasteText("한글", abc))
        assertFalse(MacKeyboardSync.shouldUsePasteText("한글", null))
        assertFalse(MacKeyboardSync.shouldUsePasteText("hello", null))
        // Non-BMP emoji always uses PasteText (macOS 0x04 posts 16-bit UniChar)
        assertTrue(MacKeyboardSync.shouldUsePasteText("😀", abc))
        assertTrue(MacKeyboardSync.shouldUsePasteText("😀", null))
        // Confirmed active macOS Input Method (2SetKorean): safely use atomic PasteText!
        assertTrue(MacKeyboardSync.shouldUsePasteText("hello", ko))
        assertTrue(MacKeyboardSync.shouldUsePasteText("한글", ko))
        // Whitespace/control and secureInput password fields always use 0x04 KeyEvents
        assertFalse(MacKeyboardSync.shouldUsePasteText(" ", ko))
        assertFalse(MacKeyboardSync.shouldUsePasteText("\n", ko))
        assertFalse(MacKeyboardSync.shouldUsePasteText("pass123", secure))
    }

    @Test
    fun initialConnectSyncMatchesAndroidEnglishAgainstMacKorean() {
        val h = Harness()
        h.sync.onConnected(InputLanguage.ENGLISH, supportsSelection = true)
        assertTrue(h.actions.isEmpty()) // waiting briefly for initial 1109

        // Mac reports 2SetKorean right after connect -> immediately triggers switch to English
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true)
        )
        assertEquals(listOf("toggle"), h.actions)
        assertEquals(MacKeyboardSync.Phase.TOGGLING, h.sync.phase)

        // Mac confirms ABC -> switch complete!
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.keylayout.ABC", false, true)
        )
        assertEquals(MacKeyboardSync.Phase.IDLE, h.sync.phase)
        assertFalse(h.hasPendingTimers())
    }

    @Test
    fun initialConnectAlreadyMatchedDoesNothing() {
        val h = Harness()
        h.sync.onConnected(InputLanguage.KOREAN, supportsSelection = true)
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true)
        )
        assertTrue(h.actions.isEmpty())
        assertEquals(MacKeyboardSync.Phase.IDLE, h.sync.phase)
    }

    @Test
    fun initialConnectWhen1109ArrivedBeforeOnConnected() {
        val h = Harness()
        // 1109 arrived during rekey before onConnected()
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.keylayout.ABC", false, true)
        )
        assertTrue(h.actions.isEmpty())

        // onConnected sees Android is Korean -> immediately switches Mac!
        h.sync.onConnected(InputLanguage.KOREAN, supportsSelection = true)
        assertEquals(listOf("toggle"), h.actions)
    }

    @Test
    fun threeSourceFallbackSelectLeavingReturning() {
        val h = Harness()
        h.sync.onConnected(InputLanguage.ENGLISH, supportsSelection = true)
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.keylayout.ABC", false, true))
        assertTrue(h.actions.isEmpty())

        // User switches Android screen keyboard to Korean
        h.sync.requestLanguage(InputLanguage.KOREAN)
        assertEquals(listOf("toggle"), h.actions)

        // Control+Space landed on Japanese (3rd input source) -> falls back to 0x1a Selecting!
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Kotoeri.RomajiTyping.Japanese", false, true)
        )
        assertEquals(
                listOf("toggle", "select:com.apple.inputmethod.Korean.2SetKorean:clear=true"),
                h.actions
        )
        assertEquals(MacKeyboardSync.Phase.SELECTING, h.sync.phase)

        // Mac reports 2SetKorean -> enters Leaving
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true)
        )
        assertEquals(MacKeyboardSync.Phase.LEAVING, h.sync.phase)

        // 0x1a("") causes Mac to revert to Japanese -> enters Returning (Control+Space)
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Kotoeri.RomajiTyping.Japanese", false, true)
        )
        assertEquals(MacKeyboardSync.Phase.RETURNING, h.sync.phase)
        assertEquals(
                listOf("toggle", "select:com.apple.inputmethod.Korean.2SetKorean:clear=true", "toggle"),
                h.actions
        )

        // Mac permanently switches to 2SetKorean -> IDLE!
        h.sync.onRemoteSourceChanged(
                RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true)
        )
        assertEquals(MacKeyboardSync.Phase.IDLE, h.sync.phase)
    }

    @Test
    fun controlSpaceDisabledFallsBackToPinningAndSkipsToggleNextTime() {
        val h = Harness()
        h.sync.onConnected(InputLanguage.ENGLISH, supportsSelection = true)
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true))

        // Toggling times out (Control+Space disabled on Mac) -> Selecting ABC
        assertTrue(h.fireNextTimer())
        assertEquals(listOf("toggle", "select:com.apple.keylayout.ABC:clear=true"), h.actions)

        // Mac temporarily switches to ABC, then 0x1a("") reverts to 2SetKorean -> Returning (Control+Space)
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.keylayout.ABC", false, true))
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.inputmethod.Korean.2SetKorean", false, true))
        assertEquals(MacKeyboardSync.Phase.RETURNING, h.sync.phase)

        // Returning ALSO times out -> marks Control+Space unavailable and pins ABC without clear!
        assertTrue(h.fireNextTimer())
        assertEquals(MacKeyboardSync.Phase.PINNING, h.sync.phase)
        assertEquals("select:com.apple.keylayout.ABC:clear=false", h.actions.last())

        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.keylayout.ABC", false, true))
        assertEquals(MacKeyboardSync.Phase.IDLE, h.sync.phase)

        // Next switch back to Korean skips Control+Space and goes straight to pinning remembered Korean ID!
        h.actions.clear()
        h.sync.requestLanguage(InputLanguage.KOREAN)
        assertEquals(listOf("select:com.apple.inputmethod.Korean.2SetKorean:clear=false"), h.actions)
    }

    @Test
    fun secureInputDefersSyncUntilExit() {
        val h = Harness()
        h.sync.onConnected(InputLanguage.KOREAN, supportsSelection = true)
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.keylayout.ABC", secureInput = true, supportsSelection = true))
        assertTrue(h.actions.isEmpty())

        // Leaving password field resumes sync to desired Korean!
        h.sync.onRemoteSourceChanged(RemoteKeyboardSource("com.apple.keylayout.ABC", secureInput = false, supportsSelection = true))
        assertEquals(listOf("toggle"), h.actions)
    }
}
