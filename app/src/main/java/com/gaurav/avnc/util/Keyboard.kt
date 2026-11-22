/*
 * Copyright (c) 2026  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.util

import android.content.Context
import android.view.View
import android.view.inputmethod.InputMethodManager
import androidx.core.view.SoftwareKeyboardControllerCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.util.WeakHashMap

private val pendingShowRetries = WeakHashMap<View, Runnable>()

fun isKeyboardVisible(view: View): Boolean {
    return ViewCompat.getRootWindowInsets(view)?.isVisible(WindowInsetsCompat.Type.ime()) == true
}

fun showKeyboard(view: View) {
    pendingShowRetries.remove(view)?.let { view.removeCallbacks(it) }

    if (!view.isFocusable) view.isFocusable = true
    if (!view.isFocusableInTouchMode) view.isFocusableInTouchMode = true
    if (!view.hasFocus()) view.requestFocus()

    SoftwareKeyboardControllerCompat(view).show()

    fun ensureImeBoundAndShown() {
        if (!view.isAttachedToWindow) return
        if (!view.hasFocus()) view.requestFocus()
        val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        if (imm != null) {
            if (!imm.isActive(view)) {
                imm.restartInput(view)
            }
            imm.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
        }
        SoftwareKeyboardControllerCompat(view).show()
    }

    // Wait for the current focus/layout pass (e.g. after DrawerLayout.close() or requestFocus())
    // so ViewRootImpl binds InputView.onCreateInputConnection() on Android 11-16 / Samsung One UI.
    view.post { ensureImeBoundAndShown() }

    val retry = Runnable {
        pendingShowRetries.remove(view)
        if (!view.isAttachedToWindow || !view.hasWindowFocus() || !view.hasFocus()) return@Runnable
        val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        if (imm != null && (!imm.isActive(view) || !isKeyboardVisible(view))) {
            ensureImeBoundAndShown()
        }
    }
    pendingShowRetries[view] = retry
    view.postDelayed(retry, 150L)
}

fun hideKeyboard(view: View) {
    pendingShowRetries.remove(view)?.let { view.removeCallbacks(it) }
    SoftwareKeyboardControllerCompat(view).hide()
    val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
    imm?.hideSoftInputFromWindow(view.windowToken, 0)
}

fun toggleKeyboard(view: View) {
    if (isKeyboardVisible(view))
        hideKeyboard(view)
    else
        showKeyboard(view)
}
