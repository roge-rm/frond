package com.rm.frond

import android.view.KeyEvent

// Characters to key presses on a US keyboard, the layout the core maps to the Palm infrared
// keyboard (native/IrKeyboardMapping.h). Soft keyboards send text, so each character is typed
// as its key with Shift where needed.
object PalmKeys {
    data class Key(val code: Int, val shift: Boolean = false)

    private val symbols: Map<Char, Key> = buildMap {
        fun pair(plain: Char, shifted: Char, code: Int) {
            put(plain, Key(code))
            put(shifted, Key(code, true))
        }
        pair('`', '~', KeyEvent.KEYCODE_GRAVE)
        pair('-', '_', KeyEvent.KEYCODE_MINUS)
        pair('=', '+', KeyEvent.KEYCODE_EQUALS)
        pair('[', '{', KeyEvent.KEYCODE_LEFT_BRACKET)
        pair(']', '}', KeyEvent.KEYCODE_RIGHT_BRACKET)
        pair('\\', '|', KeyEvent.KEYCODE_BACKSLASH)
        pair(';', ':', KeyEvent.KEYCODE_SEMICOLON)
        pair('\'', '"', KeyEvent.KEYCODE_APOSTROPHE)
        pair(',', '<', KeyEvent.KEYCODE_COMMA)
        pair('.', '>', KeyEvent.KEYCODE_PERIOD)
        pair('/', '?', KeyEvent.KEYCODE_SLASH)
        ")!@#$%^&*(".forEachIndexed { digit, c -> put(c, Key(KeyEvent.KEYCODE_0 + digit, true)) }
        put(' ', Key(KeyEvent.KEYCODE_SPACE))
        put(' ', Key(KeyEvent.KEYCODE_SPACE))
        put('\n', Key(KeyEvent.KEYCODE_ENTER))
        put('\r', Key(KeyEvent.KEYCODE_ENTER))
        put('\t', Key(KeyEvent.KEYCODE_TAB))
        put('\b', Key(KeyEvent.KEYCODE_DEL))
        // Soft keyboards' curly quotes.
        put('‘', Key(KeyEvent.KEYCODE_APOSTROPHE))
        put('’', Key(KeyEvent.KEYCODE_APOSTROPHE))
        put('“', Key(KeyEvent.KEYCODE_APOSTROPHE, true))
        put('”', Key(KeyEvent.KEYCODE_APOSTROPHE, true))
    }

    // The key for a character, or null when the keyboard has none.
    fun forChar(c: Char): Key? = when (c) {
        in 'a'..'z' -> Key(KeyEvent.KEYCODE_A + (c - 'a'))
        in 'A'..'Z' -> Key(KeyEvent.KEYCODE_A + (c - 'A'), true)
        in '0'..'9' -> Key(KeyEvent.KEYCODE_0 + (c - '0'))
        else -> symbols[c]
    }

    // The keys for a piece of text, skipping characters without one.
    fun forText(text: CharSequence): List<Key> = text.mapNotNull { forChar(it) }

    // What to type to turn the text typed so far into the new text: backspaces over the part that
    // changed, then the rest of the new text.
    data class Edit(val backspaces: Int, val typed: String)

    fun edit(from: String, to: String): Edit {
        val same = from.commonPrefixWith(to).length
        return Edit(from.length - same, to.substring(same))
    }
}
