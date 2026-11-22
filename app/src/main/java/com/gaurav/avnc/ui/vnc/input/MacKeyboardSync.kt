/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.ui.vnc.input

import com.gaurav.avnc.vnc.InputLanguage
import com.gaurav.avnc.vnc.RemoteKeyboardSource

/**
 * Synchronizes Android's on-screen keyboard language (Korean / English) with
 * macOS Screen Sharing's active TIS input source (encoding 1109 / command 0x1a /
 * Control+Space via 0x10).
 *
 * Why a multi-step state machine?
 * macOS `screensharingd` applies `0x1a` (`SelectKeyboardSource`) temporarily until
 * `0x1a("")` clears the override; on modern macOS, clearing may revert to the
 * previous source while placing our target source at the top of the MRU list,
 * after which `Control+Space` permanently switches to it. If a user disabled
 * `Control+Space` in macOS System Settings, this state machine falls back to
 * pinning via `0x1a(id)` without clearing so switching still succeeds.
 *
 * All public methods must be called from the same thread (Main thread in production).
 */
class MacKeyboardSync(
        private val selectSource: (sourceId: String, clearAfter: Boolean) -> Unit,
        private val toggleSource: () -> Unit,
        private val scheduleTimer: (delayMs: Long, action: () -> Unit) -> () -> Unit,
        private val stepTimeoutMs: Long = DEFAULT_STEP_TIMEOUT_MS,
        private val initialFallbackDelayMs: Long = DEFAULT_INITIAL_FALLBACK_MS
) {

    enum class Phase {
        IDLE,
        TOGGLING,
        SELECTING,
        LEAVING,
        RETURNING,
        PINNING
    }

    var currentSource: RemoteKeyboardSource? = null
        private set

    var desiredLanguage: InputLanguage? = null
        private set

    var phase: Phase = Phase.IDLE
        private set

    private var sessionActive = false
    private var supportsSelectionFallback = false
    private var hasPerformedInitialSync = false
    private var previousSourceId: String? = null
    private var rememberedEnglishId: String? = null
    private var rememberedKoreanId: String? = null
    private var controlSpaceUnavailable = false

    private var targetLanguage: InputLanguage? = null
    private var candidates: List<String> = emptyList()
    private var candidateIndex = 0
    private var lastSelectedId: String? = null
    private var cancelStepTimer: (() -> Unit)? = null
    private var cancelInitialTimer: (() -> Unit)? = null

    /**
     * Called once the Apple RFB 3.889 session becomes connected.
     * Immediately synchronizes macOS's input source to match [initialAndroidLanguage]
     * if encoding 1109 has already arrived, or as soon as it arrives (with a bounded
     * fallback if the server does not send an unsolicited 1109 on connect).
     */
    fun onConnected(initialAndroidLanguage: InputLanguage?, supportsSelection: Boolean) {
        sessionActive = true
        supportsSelectionFallback = supportsSelection
        hasPerformedInitialSync = false
        if (initialAndroidLanguage != null) {
            desiredLanguage = initialAndroidLanguage
        }

        val src = currentSource
        val desired = desiredLanguage
        if (src != null) {
            if (desired != null && !src.secureInput) {
                hasPerformedInitialSync = true
                if (!src.matches(desired)) {
                    startSwitch(desired)
                }
            }
        } else if (desired != null && supportsSelection) {
            cancelInitialTimer?.invoke()
            cancelInitialTimer = scheduleTimer(initialFallbackDelayMs) {
                cancelInitialTimer = null
                if (sessionActive && currentSource == null && !hasPerformedInitialSync) {
                    val lang = desiredLanguage ?: return@scheduleTimer
                    hasPerformedInitialSync = true
                    startSelecting(lang)
                }
            }
        }
    }

    /**
     * Called whenever macOS reports its active input source via encoding 1109.
     */
    fun onRemoteSourceChanged(source: RemoteKeyboardSource) {
        cancelInitialTimer?.invoke()
        cancelInitialTimer = null

        val prev = currentSource
        val wasSecure = prev?.secureInput == true

        if (source.sourceId.isNotEmpty()) {
            if (source.isKorean) {
                rememberedKoreanId = source.sourceId
            } else if (source.isEnglish) {
                rememberedEnglishId = source.sourceId
            }
            if (prev != null && prev.sourceId.isNotEmpty() && prev.sourceId != source.sourceId) {
                previousSourceId = prev.sourceId
            }
        }
        currentSource = source

        if (source.secureInput) {
            resetPhase()
            return
        }

        if (!sessionActive) {
            return
        }

        val target = targetLanguage
        when (phase) {
            Phase.IDLE -> {
                val desired = desiredLanguage
                if (desired != null && (!hasPerformedInitialSync || wasSecure)) {
                    hasPerformedInitialSync = true
                    if (!source.matches(desired)) {
                        startSwitch(desired)
                    }
                }
            }

            Phase.TOGGLING -> {
                controlSpaceUnavailable = false
                cancelStepTimer?.invoke()
                cancelStepTimer = null
                if (target != null && source.matches(target)) {
                    finishSwitch()
                } else if (target != null) {
                    startSelecting(target)
                } else {
                    resetPhase()
                }
            }

            Phase.SELECTING -> {
                if (target != null && source.matches(target)) {
                    cancelStepTimer?.invoke()
                    cancelStepTimer = null
                    lastSelectedId = source.sourceId
                    enterLeaving()
                }
            }

            Phase.LEAVING -> {
                if (target != null && !source.matches(target)) {
                    cancelStepTimer?.invoke()
                    cancelStepTimer = null
                    if (!controlSpaceUnavailable) {
                        enterReturning()
                    } else {
                        enterPinning()
                    }
                }
            }

            Phase.RETURNING -> {
                controlSpaceUnavailable = false
                cancelStepTimer?.invoke()
                cancelStepTimer = null
                if (target != null && source.matches(target)) {
                    finishSwitch()
                } else {
                    enterPinning()
                }
            }

            Phase.PINNING -> {
                cancelStepTimer?.invoke()
                cancelStepTimer = null
                if (target != null && source.matches(target)) {
                    finishSwitch()
                } else {
                    advanceCandidate(pinning = true)
                }
            }
        }
    }

    /**
     * Requests macOS to switch to [language] (called when Android's screen keyboard
     * switches 한/영 or emits Korean/English characters).
     */
    fun requestLanguage(language: InputLanguage) {
        desiredLanguage = language
        if (!sessionActive) return
        hasPerformedInitialSync = true

        val src = currentSource
        if (src?.secureInput == true) {
            return
        }
        if (src != null && src.matches(language)) {
            if (phase != Phase.IDLE && targetLanguage != language) {
                resetPhase()
            }
            return
        }
        if (phase != Phase.IDLE && targetLanguage == language) {
            return
        }

        startSwitch(language)
    }

    /**
     * Toggles between Korean and English (e.g. hardware language-switch key).
     */
    fun toggleLanguage() {
        if (!sessionActive) return
        val current = currentSource?.detectedLanguage() ?: desiredLanguage
        when (current) {
            InputLanguage.KOREAN -> requestLanguage(InputLanguage.ENGLISH)
            InputLanguage.ENGLISH -> requestLanguage(InputLanguage.KOREAN)
            null -> toggleSource()
        }
    }

    fun reset() {
        sessionActive = false
        supportsSelectionFallback = false
        hasPerformedInitialSync = false
        currentSource = null
        desiredLanguage = null
        previousSourceId = null
        rememberedEnglishId = null
        rememberedKoreanId = null
        controlSpaceUnavailable = false
        cancelInitialTimer?.invoke()
        cancelInitialTimer = null
        resetPhase()
    }

    /**************************************************************************
     * State machine internals
     **************************************************************************/

    private fun canSelectSource(): Boolean {
        return currentSource?.supportsSelection ?: supportsSelectionFallback
    }

    private fun startSwitch(language: InputLanguage) {
        resetPhase()
        targetLanguage = language
        candidates = candidatesFor(language)
        candidateIndex = 0
        lastSelectedId = null

        val prevId = previousSourceId
        val prevMatchesTarget = prevId != null &&
                RemoteKeyboardSource(prevId).matches(language)

        if (!controlSpaceUnavailable && (prevId == null || prevMatchesTarget)) {
            enterToggling()
        } else if (canSelectSource()) {
            if (controlSpaceUnavailable) {
                enterPinningCurrentCandidate()
            } else {
                startSelecting(language)
            }
        } else {
            enterToggling()
        }
    }

    private fun enterToggling() {
        phase = Phase.TOGGLING
        toggleSource()
        armStepTimer {
            if (phase != Phase.TOGGLING) return@armStepTimer
            val target = targetLanguage ?: return@armStepTimer
            if (canSelectSource()) {
                startSelecting(target)
            } else {
                resetPhase()
            }
        }
    }

    private fun startSelecting(language: InputLanguage) {
        if (!canSelectSource()) {
            resetPhase()
            return
        }
        targetLanguage = language
        candidates = candidatesFor(language)
        candidateIndex = 0
        sendSelectCandidate(pinning = false)
    }

    private fun sendSelectCandidate(pinning: Boolean) {
        if (candidateIndex >= candidates.size) {
            resetPhase()
            return
        }
        val id = candidates[candidateIndex]
        lastSelectedId = id
        phase = if (pinning) Phase.PINNING else Phase.SELECTING
        selectSource(id, !pinning)
        armStepTimer {
            if (phase != Phase.SELECTING && phase != Phase.PINNING) return@armStepTimer
            advanceCandidate(pinning = (phase == Phase.PINNING))
        }
    }

    private fun advanceCandidate(pinning: Boolean) {
        candidateIndex++
        if (candidateIndex < candidates.size) {
            sendSelectCandidate(pinning)
        } else {
            resetPhase()
        }
    }

    private fun enterLeaving() {
        phase = Phase.LEAVING
        armStepTimer {
            // Did not revert back within timeout -> macOS kept the selected source!
            if (phase == Phase.LEAVING) {
                finishSwitch()
            }
        }
    }

    private fun enterReturning() {
        phase = Phase.RETURNING
        toggleSource()
        armStepTimer {
            if (phase != Phase.RETURNING) return@armStepTimer
            // Control+Space did not switch back -> disabled on this Mac.
            controlSpaceUnavailable = true
            enterPinning()
        }
    }

    private fun enterPinning() {
        val id = lastSelectedId
        if (!canSelectSource() || id.isNullOrEmpty()) {
            resetPhase()
            return
        }
        phase = Phase.PINNING
        selectSource(id, false)
        armStepTimer {
            if (phase == Phase.PINNING) {
                resetPhase()
            }
        }
    }

    private fun enterPinningCurrentCandidate() {
        if (!canSelectSource()) {
            resetPhase()
            return
        }
        sendSelectCandidate(pinning = true)
    }

    private fun finishSwitch() {
        resetPhase()
    }

    private fun resetPhase() {
        cancelStepTimer?.invoke()
        cancelStepTimer = null
        phase = Phase.IDLE
        targetLanguage = null
        candidates = emptyList()
        candidateIndex = 0
        lastSelectedId = null
    }

    private fun armStepTimer(onTimeout: () -> Unit) {
        cancelStepTimer?.invoke()
        cancelStepTimer = scheduleTimer(stepTimeoutMs) {
            cancelStepTimer = null
            onTimeout()
        }
    }

    private fun candidatesFor(language: InputLanguage): List<String> {
        val list = ArrayList<String>(4)
        when (language) {
            InputLanguage.ENGLISH -> {
                rememberedEnglishId?.let { list.add(it) }
                list.add(SOURCE_ABC)
                list.add(SOURCE_US)
            }

            InputLanguage.KOREAN -> {
                rememberedKoreanId?.let { list.add(it) }
                list.add(SOURCE_KOREAN_2SET)
                list.add(SOURCE_KOREAN_3SET)
            }
        }
        return list.distinct()
    }

    companion object {
        const val SOURCE_ABC = "com.apple.keylayout.ABC"
        const val SOURCE_US = "com.apple.keylayout.US"
        const val SOURCE_KOREAN_2SET = "com.apple.inputmethod.Korean.2SetKorean"
        const val SOURCE_KOREAN_3SET = "com.apple.inputmethod.Korean.3SetKorean"

        const val DEFAULT_STEP_TIMEOUT_MS = 350L
        const val DEFAULT_INITIAL_FALLBACK_MS = 700L

        /**
         * Maps an Android IME subtype locale / BCP-47 language tag (e.g. "ko-KR",
         * "ko_KR", "en-US", "en_GB") to [InputLanguage].
         */
        fun detectLanguageFromLocaleTag(tag: String?): InputLanguage? {
            if (tag.isNullOrBlank()) return null
            val primary = tag.trim().lowercase().split('-', '_').firstOrNull() ?: return null
            return when (primary) {
                "ko", "kor" -> InputLanguage.KOREAN
                "en", "eng" -> InputLanguage.ENGLISH
                else -> null
            }
        }

        /**
         * Inspects text emitted by the soft keyboard (Samsung Keyboard, Gboard, etc.)
         * and returns the language of the most recent Hangul or Latin character.
         * Digits, spaces, punctuation, and emoji return null (neutral).
         */
        fun detectScriptLanguage(text: CharSequence?): InputLanguage? {
            if (text.isNullOrEmpty()) return null
            for (i in text.length - 1 downTo 0) {
                val c = text[i]
                if (isHangulChar(c)) return InputLanguage.KOREAN
                if ((c in 'A'..'Z') || (c in 'a'..'z')) return InputLanguage.ENGLISH
            }
            return null
        }

        fun isHangulChar(c: Char): Boolean {
            return (c in '\uAC00'..'\uD7A3') || // Hangul Syllables
                   (c in '\u3130'..'\u318F') || // Hangul Compatibility Jamo
                   (c in '\u1100'..'\u11FF')    // Hangul Jamo
        }

        /**
         * Decides whether [text] should be delivered via transient Apple 0x1f PasteText
         * (`true`) or directly via atomic 0x04 KeyEvents (`false`).
         *
         * Matches macOS `ScreensharingAgent`'s verified behavior:
         * - Password fields (`secureInput == true`): always KeyEvents.
         * - Whitespace / control-only (`" "`, `"\n"`, `"\t"`): always KeyEvents.
         * - Non-BMP (surrogate pairs, e.g. emoji): always PasteText (macOS 0x04 posts 16-bit UniChar).
         * - macOS confirmed on a non-IME keylayout (`com.apple.keylayout.ABC` / `US`) or
         *   unreported (`remoteSource == null`): direct 0x04 KeyEvents (`0x01000000 | codepoint`)
         *   deliver BOTH English and Hangul BMP cleanly with zero clipboard latency or Cmd+V races.
         * - macOS confirmed in an Input Method (`remoteSource != null && remoteSource.isInputMethod`,
         *   e.g. `2SetKorean`): atomic PasteText so macOS TSM never drops Hangul or turns English
         *   into Korean jamo (`ㅁㅠㅊ`) while MacKeyboardSync switches macOS back to ABC/US.
         */
        fun shouldUsePasteText(text: String, remoteSource: RemoteKeyboardSource?): Boolean {
            if (text.isEmpty()) return false
            if (remoteSource?.secureInput == true) return false

            var onlyWhitespaceControl = true
            for (c in text) {
                if (Character.isSurrogate(c)) return true
                if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                    onlyWhitespaceControl = false
                }
            }
            if (onlyWhitespaceControl) return false
            return remoteSource != null && remoteSource.isInputMethod
        }
    }
}
