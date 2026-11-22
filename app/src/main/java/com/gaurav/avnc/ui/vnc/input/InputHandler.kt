/*
 * Copyright (c) 2025  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.ui.vnc.input

import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import com.gaurav.avnc.ui.vnc.VncActivity
import com.gaurav.avnc.vnc.InputLanguage

/**
 * Input handler for remote session.
 * All input events are passed through here.
 *
 * This is a simple hub, used for logging, filtering, notifications etc.
 * Main input processing happens in [TouchHandler] & [KeyHandler].
 */
class InputHandler {
    private var dispatcher: Dispatcher? = null
    private var touchHandler: TouchHandler? = null
    private var keyHandler: KeyHandler? = null
    private var interceptMouseBack = false
    private var kmLanguageSwitchToSuper = false

    /**
     * List of listeners to be notified after a [KeyEvent] is handled
     */
    val onAfterKeyEventListeners = mutableListOf<(KeyEvent) -> Unit>()

    fun onSessionConnected(activity: VncActivity) {
        val viewModel = activity.viewModel
        dispatcher = Dispatcher(activity)
        touchHandler = TouchHandler(activity.binding.inputView, dispatcher!!, viewModel.pref)
        keyHandler = KeyHandler(dispatcher!!, viewModel.pref)
        interceptMouseBack = viewModel.pref.input.interceptMouseBack
        kmLanguageSwitchToSuper = viewModel.pref.input.kmLanguageSwitchToSuper
    }

    fun onSessionDisconnected() {
        dispatcher = null
        touchHandler = null
        keyHandler = null
        interceptMouseBack = false
        kmLanguageSwitchToSuper = false
    }

    val isAppleSession: Boolean
        get() = dispatcher?.isAppleKeyboardSyncSupported == true

    val isRemoteSecureInput: Boolean
        get() = dispatcher?.isRemoteSecureInput == true

    val prefersCommittedTextOnly: Boolean
        get() = dispatcher?.prefersCommittedTextOnly == true

    fun ensureMacNonIme() {
        dispatcher?.ensureMacNonImeForSoftKeyboard()
    }

    fun onTextInput(deleting: Int, text: String): Boolean {
        val d = dispatcher ?: return false
        if (d.isAppleKeyboardSyncSupported) {
            return d.onAppleTextInput(deleting, text)
        }
        return keyHandler?.onTextInput(deleting, text) == true
    }

    fun onKeyEvent(keyEvent: KeyEvent): Boolean {
        val handled = handleAppleSpecialKeyEvent(keyEvent) ||
                      keyHandler?.onKeyEvent(keyEvent) == true ||
                      interceptBackPressFromMouse(keyEvent)
        onAfterKeyEventListeners.forEach { it(keyEvent) }
        return handled
    }

    private fun handleAppleSpecialKeyEvent(event: KeyEvent): Boolean {
        val d = dispatcher ?: return false
        if (!d.isAppleKeyboardSyncSupported) return false

        // Multi-character events (e.g. VirtualKeys text box or IME commit via ACTION_MULTIPLE)
        @Suppress("DEPRECATION")
        if (event.action == KeyEvent.ACTION_MULTIPLE &&
            event.keyCode == KeyEvent.KEYCODE_UNKNOWN &&
            !event.characters.isNullOrEmpty()) {
            return d.onAppleTextInput(0, event.characters)
        }

        // Hardware / soft language-switch keys
        when (event.keyCode) {
            KeyEvent.KEYCODE_EISU -> {
                if (event.action == KeyEvent.ACTION_DOWN) d.onMacLanguageRequest(InputLanguage.ENGLISH)
                return true
            }

            KeyEvent.KEYCODE_KANA -> {
                if (event.action == KeyEvent.ACTION_DOWN) d.onMacLanguageRequest(InputLanguage.KOREAN)
                return true
            }

            KeyEvent.KEYCODE_LANGUAGE_SWITCH -> {
                if (!kmLanguageSwitchToSuper) {
                    if (event.action == KeyEvent.ACTION_DOWN) d.onMacLanguageToggle()
                    return true
                }
            }
        }

        // Detect English letters typed directly via soft-keyboard KeyEvents.
        // If macOS has not yet finished leaving Korean IME, paste the ASCII letter safely so it never
        // gets turned into Korean jamo (e.g. 'a' -> 'ㅁ') during the transition window.
        // Physical hardware keyboards (deviceId != VIRTUAL_KEYBOARD) are left free to use macOS IME
        // when the user explicitly toggles language with a hardware language-switch key.
        if (event.deviceId == KeyCharacterMap.VIRTUAL_KEYBOARD &&
            !event.isCtrlPressed && !event.isAltPressed && !event.isMetaPressed &&
            (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP)) {
            val uChar = event.getUnicodeChar(event.metaState)
            if ((uChar in 'A'.code..'Z'.code) || (uChar in 'a'.code..'z'.code)) {
                if (event.action == KeyEvent.ACTION_DOWN) {
                    d.onMacLanguageRequest(InputLanguage.ENGLISH)
                }
                if (d.shouldPasteAsciiDuringMacIme) {
                    if (event.action == KeyEvent.ACTION_DOWN) {
                        d.onApplePasteText(uChar.toChar().toString())
                    }
                    return true
                }
            }
        }

        return false
    }

    fun onVkKeyEvent(keyEvent: KeyEvent): Boolean {
        val handled = keyHandler?.onVkKeyEvent(keyEvent) == true
        onAfterKeyEventListeners.forEach { it(keyEvent) }
        return handled
    }

    fun onTouchEvent(event: MotionEvent): Boolean {
        return touchHandler?.onTouchEvent(event) == true
    }

    fun onGenericMotionEvent(event: MotionEvent): Boolean {
        return touchHandler?.onGenericMotionEvent(event) == true
    }

    fun onCapturedPointerEvent(event: MotionEvent): Boolean {
        return touchHandler?.onCapturedPointerEvent(event) == true
    }

    fun onHoverEvent(event: MotionEvent): Boolean {
        return touchHandler?.onHoverEvent(event) == true
    }

    /*********************************************************************************************/

    private fun interceptBackPressFromMouse(keyEvent: KeyEvent): Boolean {
        //It seems that some device manufacturers are hell-bent on making developers'
        //life miserable. In their infinite wisdom, they decided that Android apps don't
        //need Mouse right-click events. It is hardcoded to act as back-press, without
        //giving apps a chance to handle it. For better or worse, they set the 'source'
        //for such key events to Mouse, enabling the following workarounds.
        if (touchHandler != null && interceptMouseBack &&
            keyEvent.keyCode == KeyEvent.KEYCODE_BACK &&
            keyEvent.scanCode == 0 &&
            keyEvent.flags and KeyEvent.FLAG_VIRTUAL_HARD_KEY == 0 &&
            InputDevice.getDevice(keyEvent.deviceId)?.supportsSource(InputDevice.SOURCE_MOUSE) == true) {
            if (keyEvent.action == KeyEvent.ACTION_DOWN)
                touchHandler!!.onMouseBack()
            return true
        }
        return false
    }
}