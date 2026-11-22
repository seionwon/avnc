/*
 * Copyright (c) 2026  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.ui.vnc.input

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.os.Build
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.text.SpannableStringBuilder
import android.util.AttributeSet
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.CompletionInfo
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.ExtractedText
import android.view.inputmethod.ExtractedTextRequest
import android.view.inputmethod.InputMethodManager
import android.view.inputmethod.TextAttribute
import androidx.annotation.RequiresApi
import androidx.core.view.inputmethod.EditorInfoCompat
import com.gaurav.avnc.viewmodel.VncViewModel
import java.text.BreakIterator
import kotlin.math.max
import kotlin.math.min

/**
 * Transparent view that receives touch, pointer, hardware key, and soft-keyboard IME events.
 *
 * Uses a full [SpannableStringBuilder] editor buffer (`BaseInputConnection(this, true)`) with
 * proper initial `Selection(0, 0)`, `EditorInfoCompat.setInitialSurroundingText`, batching,
 * and `InputMethodManager.updateSelection(...)` reporting so Samsung Keyboard, Gboard, and
 * other Android Korean/CJK IMEs compose Hangul syllables reliably without drops or corruption.
 */
class InputView(context: Context?, attrs: AttributeSet? = null) : View(context, attrs) {

    private var viewModel: VncViewModel? = null
    private var inputHandler: InputHandler? = null

    private val document = SpannableStringBuilder()
    private var sentText = ""
    private var batchDepth = 0
    private var imeVisible = false
    private var extractedTextMonitorToken: Int? = null
    private var lastSecureInput = false

    private val imm: InputMethodManager?
        get() = context?.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager

    private val markedRange: IntRange?
        get() {
            val start = BaseInputConnection.getComposingSpanStart(document)
            val end = BaseInputConnection.getComposingSpanEnd(document)
            if (start < 0 || end <= start || end > document.length) return null
            return start until end
        }

    /**
     * Full-editor InputConnection supporting Hangul/CJK composition, live diffing when
     * macOS accepts direct 0x04 Unicode keysyms (ABC/US layout), and committed-only
     * flushing when macOS is temporarily in an Input Method (PasteText fallback).
     */
    inner class InputConnection : BaseInputConnection(this, true) {

        override fun getEditable(): Editable = document

        override fun beginBatchEdit(): Boolean {
            batchDepth++
            return true
        }

        override fun endBatchEdit(): Boolean {
            if (batchDepth > 0) batchDepth--
            if (batchDepth == 0) documentDidChange()
            return batchDepth > 0
        }

        private inline fun <T> batch(body: () -> T): T {
            beginBatchEdit()
            try {
                return body()
            } finally {
                endBatchEdit()
            }
        }

        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean =
            batch { super.commitText(text, newCursorPosition) }

        @RequiresApi(33)
        override fun commitText(text: CharSequence, newCursorPosition: Int, textAttribute: TextAttribute?): Boolean =
            batch { super.commitText(text, newCursorPosition, textAttribute) }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean =
            batch { super.setComposingText(text, newCursorPosition) }

        @RequiresApi(33)
        override fun setComposingText(text: CharSequence, newCursorPosition: Int, textAttribute: TextAttribute?): Boolean =
            batch { super.setComposingText(text, newCursorPosition, textAttribute) }

        override fun setComposingRegion(start: Int, end: Int): Boolean =
            batch { super.setComposingRegion(start, end) }

        @RequiresApi(33)
        override fun setComposingRegion(start: Int, end: Int, textAttribute: TextAttribute?): Boolean =
            batch { super.setComposingRegion(start, end, textAttribute) }

        override fun finishComposingText(): Boolean =
            batch { super.finishComposingText() }

        override fun setSelection(start: Int, end: Int): Boolean =
            batch { super.setSelection(start, end) }

        @RequiresApi(34)
        override fun replaceText(start: Int, end: Int, text: CharSequence, newCursorPosition: Int,
                                 textAttribute: TextAttribute?): Boolean =
            batch { super.replaceText(start, end, text, newCursorPosition, textAttribute) }

        override fun commitCompletion(text: CompletionInfo?): Boolean {
            val completion = text?.text ?: return false
            return commitText(completion, 1)
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            val excess = excessBefore(beforeLength, codePoints = false)
            val deleted = batch { super.deleteSurroundingText(beforeLength, afterLength) }
            if (excess > 0) {
                sendRemoteBackspaces(excess)
            }
            return deleted
        }

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
            val excess = excessBefore(beforeLength, codePoints = true)
            val deleted = batch { super.deleteSurroundingTextInCodePoints(beforeLength, afterLength) }
            if (excess > 0) {
                sendRemoteBackspaces(excess)
            }
            return deleted
        }

        override fun performEditorAction(editorAction: Int): Boolean {
            sendEnterAndReset()
            return true
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            if (!event.hasNoModifiers()) {
                if (event.action == KeyEvent.ACTION_DOWN) {
                    clearEditorState(flushComposing = true, restartIme = false)
                }
                return inputHandler?.onKeyEvent(event) == true || super.sendKeyEvent(event)
            }

            when (event.keyCode) {
                KeyEvent.KEYCODE_DEL -> {
                    if (event.action == KeyEvent.ACTION_DOWN) {
                        deleteBackward()
                    }
                    return true
                }

                KeyEvent.KEYCODE_ENTER,
                KeyEvent.KEYCODE_NUMPAD_ENTER -> {
                    if (event.action == KeyEvent.ACTION_DOWN) {
                        sendEnterAndReset()
                    }
                    return true
                }

                else -> {
                    if (event.action == KeyEvent.ACTION_DOWN && shouldClearEditorOnKey(event)) {
                        clearEditorState(flushComposing = true, restartIme = true)
                    }
                    return inputHandler?.onKeyEvent(event) == true || super.sendKeyEvent(event)
                }
            }
        }

        override fun getExtractedText(request: ExtractedTextRequest?, flags: Int): ExtractedText {
            extractedTextMonitorToken = if (request != null && (flags and GET_EXTRACTED_TEXT_MONITOR) != 0) {
                request.token
            } else {
                null
            }
            return buildExtractedText()
        }

        override fun closeConnection() {
            super.closeConnection()
            batchDepth = 0
            extractedTextMonitorToken = null
        }
    }

    init {
        Selection.setSelection(document, 0)
        isFocusable = true
        isFocusableInTouchMode = true
        setWillNotDraw(true)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            defaultFocusHighlightEnabled = false
        }
        if (Build.VERSION.SDK_INT >= 33) {
            isAutoHandwritingEnabled = false
        }
    }

    /**
     * Should be called from [com.gaurav.avnc.ui.vnc.VncActivity.onCreate].
     */
    fun initialize(viewModel: VncViewModel, inputHandler: InputHandler) {
        this.viewModel = viewModel
        this.inputHandler = inputHandler

        // Hide local cursor if requested and supported
        if (Build.VERSION.SDK_INT >= 24 && viewModel.pref.input.hideLocalCursor)
            pointerIcon = PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL)
    }

    fun onSessionConnected() {
        clearEditorState(flushComposing = false, restartIme = false)
    }

    fun onSessionDisconnected() {
        clearEditorState(flushComposing = false, restartIme = false)
    }

    fun onImeVisibilityChanged(visible: Boolean) {
        if (imeVisible == visible) return
        imeVisible = visible
        Log.d(TAG, "onImeVisibilityChanged: visible=$visible focused=$isFocused active=${imm?.isActive(this)}")
        if (visible) {
            inputHandler?.ensureMacNonIme()
            if (isFocused && imm?.isActive(this) != true) {
                post {
                    if (isFocused && imeVisible && imm?.isActive(this) != true) {
                        imm?.restartInput(this)
                    }
                }
            }
        } else {
            clearEditorState(flushComposing = true, restartIme = false)
        }
    }

    fun onWindowFocusGained() {
        if (imeVisible) {
            inputHandler?.ensureMacNonIme()
            if (isFocused && imm?.isActive(this) != true) {
                post {
                    if (isFocused && imeVisible && imm?.isActive(this) != true) {
                        imm?.restartInput(this)
                    }
                }
            }
        }
    }

    fun onRemoteKeyboardSourceChanged() {
        val isSecure = inputHandler?.isRemoteSecureInput == true
        if (isSecure != lastSecureInput) {
            lastSecureInput = isSecure
            if (isFocused && imm?.isActive(this) == true) {
                imm?.restartInput(this)
            }
        }
        if (batchDepth == 0 && document.isNotEmpty() && inputHandler?.prefersCommittedTextOnly != true) {
            syncDocument()
        }
    }

    override fun onFocusChanged(gainFocus: Boolean, direction: Int, previouslyFocusedRect: Rect?) {
        super.onFocusChanged(gainFocus, direction, previouslyFocusedRect)
        Log.d(TAG, "onFocusChanged: gainFocus=$gainFocus imeVisible=$imeVisible")
        if (!gainFocus) {
            clearEditorState(flushComposing = true, restartIme = false)
        } else if (imeVisible) {
            post {
                if (isFocused && imeVisible && imm?.isActive(this) != true) {
                    imm?.restartInput(this)
                }
            }
        }
    }

    fun clearEditorState(flushComposing: Boolean = true, restartIme: Boolean = false) {
        val hadContent = document.isNotEmpty() || sentText.isNotEmpty()
        if (flushComposing && markedRange != null) {
            BaseInputConnection.removeComposingSpans(document)
            syncDocument()
        }
        BaseInputConnection.removeComposingSpans(document)
        document.clear()
        sentText = ""
        Selection.setSelection(document, 0)
        if (hadContent) {
            reportSelection()
            if (restartIme && isFocused && imm?.isActive(this) == true) {
                imm?.restartInput(this)
            }
        }
    }

    private fun sendEnterAndReset() {
        if (markedRange != null) {
            BaseInputConnection.removeComposingSpans(document)
            syncDocument()
        }
        clearEditorState(flushComposing = false, restartIme = true)
        inputHandler?.onKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))
        inputHandler?.onKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
    }

    private fun deleteBackward() {
        var selStart = Selection.getSelectionStart(document)
        var selEnd = Selection.getSelectionEnd(document)
        if (selStart < 0 || selEnd < 0) {
            selStart = document.length
            selEnd = document.length
        }
        var start = min(selStart, selEnd)
        var end = max(selStart, selEnd)
        val marked = markedRange

        if (start == end) {
            if (start <= 0) {
                if (marked == null) {
                    sendRemoteBackspaces(1)
                }
                return
            }
            val bi = BreakIterator.getCharacterInstance()
            bi.setText(document.toString())
            val prev = bi.preceding(start)
            start = if (prev != BreakIterator.DONE) prev else (start - 1)
        }

        if (marked != null) {
            start = max(start, marked.first)
            end = min(end, marked.last + 1)
            if (end <= start) return
        }

        document.delete(start, end)
        if (markedRange == null) {
            BaseInputConnection.removeComposingSpans(document)
        }
        Selection.setSelection(document, start)
        documentDidChange()
    }

    private fun sendRemoteBackspaces(count: Int) {
        if (count <= 0) return
        inputHandler?.onTextInput(count, "")
    }

    private fun excessBefore(length: Int, codePoints: Boolean): Int {
        if (length <= 0 || markedRange != null) return 0
        val selStart = Selection.getSelectionStart(document)
        val selEnd = Selection.getSelectionEnd(document)
        val start = if (selStart >= 0 && selEnd >= 0) min(selStart, selEnd) else document.length
        val available = if (codePoints) Character.codePointCount(document, 0, start) else start
        return max(0, length - available)
    }

    private fun documentDidChange() {
        if (batchDepth > 0) return
        syncDocument()
        reportSelection()
    }

    private fun buildExtractedText(): ExtractedText {
        val selStart = Selection.getSelectionStart(document).coerceIn(0, document.length)
        val selEnd = Selection.getSelectionEnd(document).coerceIn(0, document.length)
        return ExtractedText().apply {
            text = document.toString()
            startOffset = 0
            partialStartOffset = -1
            partialEndOffset = -1
            selectionStart = selStart
            selectionEnd = selEnd
            flags = 0
        }
    }

    private fun reportSelection() {
        val selStart = Selection.getSelectionStart(document).coerceIn(0, document.length)
        val selEnd = Selection.getSelectionEnd(document).coerceIn(0, document.length)
        val compStart = BaseInputConnection.getComposingSpanStart(document)
        val compEnd = BaseInputConnection.getComposingSpanEnd(document)
        imm?.updateSelection(this, selStart, selEnd, compStart, compEnd)
        extractedTextMonitorToken?.let { token ->
            imm?.updateExtractedText(this, token, buildExtractedText())
        }
    }

    private fun syncDocument() {
        val committedOnly = inputHandler?.prefersCommittedTextOnly == true
        val marked = markedRange
        val current = if (committedOnly && marked != null) {
            document.toString().removeRange(marked)
        } else {
            document.toString()
        }

        if (current != sentText) {
            var prefix = 0
            val minLen = min(sentText.length, current.length)
            while (prefix < minLen && sentText[prefix] == current[prefix]) {
                prefix++
            }
            // Never split a UTF-16 surrogate pair across the diff boundary
            if (prefix > 0 && prefix < sentText.length && Character.isLowSurrogate(sentText[prefix])) {
                prefix--
            }

            val deleting = if (prefix < sentText.length) {
                Character.codePointCount(sentText, prefix, sentText.length)
            } else {
                0
            }
            val inserting = current.substring(prefix)
            sentText = current

            if (deleting > 0 || inserting.isNotEmpty()) {
                inputHandler?.onTextInput(deleting, inserting)
            }
        }

        // Keep local editor buffer bounded once no composition is active
        if (markedRange == null && document.length > MAX_BUFFER_LEN) {
            val trim = document.length - KEEP_BUFFER_LEN
            val safeTrim = if (trim > 0 && Character.isLowSurrogate(document[trim])) trim - 1 else trim
            if (safeTrim > 0) {
                val selStart = Selection.getSelectionStart(document)
                val selEnd = Selection.getSelectionEnd(document)
                document.delete(0, safeTrim)
                sentText = document.toString()
                val newStart = if (selStart >= 0) (selStart - safeTrim).coerceIn(0, document.length) else document.length
                val newEnd = if (selEnd >= 0) (selEnd - safeTrim).coerceIn(0, document.length) else document.length
                Selection.setSelection(document, newStart, newEnd)
            }
        }
    }

    private fun shouldClearEditorOnKey(event: KeyEvent): Boolean {
        return when (event.keyCode) {
            KeyEvent.KEYCODE_TAB,
            KeyEvent.KEYCODE_ESCAPE,
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_MOVE_HOME,
            KeyEvent.KEYCODE_MOVE_END,
            KeyEvent.KEYCODE_PAGE_UP,
            KeyEvent.KEYCODE_PAGE_DOWN,
            KeyEvent.KEYCODE_FORWARD_DEL -> true
            else -> false
        }
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        batchDepth = 0
        extractedTextMonitorToken = null
        BaseInputConnection.removeComposingSpans(document)
        document.clear()
        sentText = ""
        Selection.setSelection(document, 0)

        val isSecure = inputHandler?.isRemoteSecureInput == true
        lastSecureInput = isSecure

        // Avoid IME_FLAG_NO_PERSONALIZED_LEARNING and TYPE_TEXT_FLAG_NO_SUGGESTIONS during normal
        // editing so Samsung Voice Input and Gboard Voice Typing remain enabled (they treat those
        // flags as a restricted/incognito field and display "voice input is not supported").
        outAttrs.inputType = if (isSecure) {
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        } else {
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        }
        outAttrs.imeOptions = EditorInfo.IME_ACTION_UNSPECIFIED or
                EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                EditorInfo.IME_FLAG_NO_FULLSCREEN or
                (if (isSecure) EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING else 0)
        outAttrs.fieldId = id
        outAttrs.packageName = context?.packageName
        outAttrs.initialSelStart = 0
        outAttrs.initialSelEnd = 0
        outAttrs.initialCapsMode = 0
        EditorInfoCompat.setInitialSurroundingText(outAttrs, document)

        inputHandler?.ensureMacNonIme()
        Log.d(TAG, "onCreateInputConnection: focused=$isFocused apple=${inputHandler?.isAppleSession} secure=$isSecure")
        return InputConnection()
    }

    override fun onCheckIsTextEditor(): Boolean = true

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            val wasFocused = isFocused
            if (!wasFocused) requestFocus()
            clearEditorState(flushComposing = true, restartIme = wasFocused)
            if (!wasFocused && imeVisible) {
                post {
                    if (isFocused && imeVisible && imm?.isActive(this) != true) {
                        imm?.restartInput(this)
                    }
                }
            }
        }
        return inputHandler?.onTouchEvent(event) == true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        return inputHandler?.onGenericMotionEvent(event) == true
    }

    override fun onHoverEvent(event: MotionEvent): Boolean {
        return inputHandler?.onHoverEvent(event) == true
    }

    override fun onCapturedPointerEvent(event: MotionEvent): Boolean {
        return inputHandler?.onCapturedPointerEvent(event) == true
    }

    companion object {
        private const val TAG = "InputView"
        private const val MAX_BUFFER_LEN = 256
        private const val KEEP_BUFFER_LEN = 64
    }
}
