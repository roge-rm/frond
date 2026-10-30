package com.rm.frond

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import org.libsdl.app.SDLActivity

// An invisible view that takes the soft keyboard while the Palm infrared keyboard is attached.
// What the soft keyboard sends goes into SDL as key presses, the same way hardware keys do, and
// the core passes them to the Palm keyboard driver. Hardware keys go through SDL's own handling.
class PalmKeyboard(context: Context) : View(context) {
    init {
        isFocusable = true
        isFocusableInTouchMode = true
        setOnKeyListener { view, keyCode, event -> SDLActivity.handleKeyEvent(view, keyCode, event, null) }
    }

    override fun onCheckIsTextEditor() = true

    // Plain keys: no suggestions, no capitals by itself, and Enter types a new line.
    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
        outAttrs.initialSelStart = 0
        outAttrs.initialSelEnd = 0
        return Connection(this)
    }

    val showing get() = hasFocus()

    fun show() {
        requestFocus()
        imm().showSoftInput(this, 0)
    }

    // The caller gives focus back to SDL's surface.
    fun hide() {
        imm().hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
    }

    private fun imm() = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager

    // Text from the soft keyboard. Composing text is typed as it changes and backspaced over when
    // the keyboard replaces it. Runs on the main thread, like SDL's own key handling.
    private class Connection(view: View) : BaseInputConnection(view, false) {
        private var composing = ""

        override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
            replaceComposing(text.toString())
            composing = ""
            return true
        }

        override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
            replaceComposing(text.toString())
            composing = text.toString()
            return true
        }

        override fun finishComposingText(): Boolean {
            composing = ""
            return true
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            repeat(beforeLength) { press(PalmKeys.Key(KeyEvent.KEYCODE_DEL)) }
            repeat(afterLength) { press(PalmKeys.Key(KeyEvent.KEYCODE_FORWARD_DEL)) }
            return true
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> SDLActivity.onNativeKeyDown(event.keyCode)
                KeyEvent.ACTION_UP -> SDLActivity.onNativeKeyUp(event.keyCode)
            }
            return true
        }

        override fun performEditorAction(actionCode: Int): Boolean {
            press(PalmKeys.Key(KeyEvent.KEYCODE_ENTER))
            return true
        }

        // Nothing is kept as text, so the keyboard sends Backspace as a key when nothing is composing.
        override fun getTextBeforeCursor(length: Int, flags: Int): CharSequence = composing.takeLast(length)
        override fun getTextAfterCursor(length: Int, flags: Int): CharSequence = ""

        private fun replaceComposing(text: String) {
            val edit = PalmKeys.edit(composing, text)
            repeat(edit.backspaces) { press(PalmKeys.Key(KeyEvent.KEYCODE_DEL)) }
            PalmKeys.forText(edit.typed).forEach(::press)
        }

        private fun press(key: PalmKeys.Key) {
            if (key.shift) SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SHIFT_LEFT)
            SDLActivity.onNativeKeyDown(key.code)
            SDLActivity.onNativeKeyUp(key.code)
            if (key.shift) SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SHIFT_LEFT)
        }
    }
}
