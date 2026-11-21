/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.vnc

/**
 * Simplified input language state synchronized between Android's on-screen
 * keyboard and macOS Screen Sharing's active TIS input source.
 */
enum class InputLanguage {
    ENGLISH,
    KOREAN
}

/**
 * Snapshot of the active macOS keyboard input source received via
 * Apple RFB 3.889 encoding 1109 (0x455 KeyboardSource).
 */
data class RemoteKeyboardSource(
        val sourceId: String = "",
        val secureInput: Boolean = false,
        val supportsSelection: Boolean = false
) {
    /**
     * True when macOS is currently using an Input Method (e.g. Korean 2-Set,
     * Chinese Pinyin, Japanese Kotoeri) rather than a direct keyboard layout
     * (`com.apple.keylayout.*`).
     */
    val isInputMethod: Boolean
        get() = sourceId.isNotEmpty() && !sourceId.startsWith("com.apple.keylayout.")

    val isKorean: Boolean
        get() = sourceId.contains("Korean", ignoreCase = true) ||
                sourceId.contains("Hangul", ignoreCase = true) ||
                sourceId.contains("Gureum.han", ignoreCase = true)

    val isEnglish: Boolean
        get() = sourceId.isNotEmpty() && !isKorean &&
                (sourceId.startsWith("com.apple.keylayout.") ||
                 sourceId.contains("Gureum.qwerty", ignoreCase = true) ||
                 sourceId.contains("Gureum.roman", ignoreCase = true))

    fun matches(language: InputLanguage): Boolean = when (language) {
        InputLanguage.KOREAN -> isKorean
        InputLanguage.ENGLISH -> isEnglish
    }

    fun detectedLanguage(): InputLanguage? = when {
        sourceId.isEmpty() -> null
        isKorean -> InputLanguage.KOREAN
        isEnglish -> InputLanguage.ENGLISH
        else -> null
    }
}
