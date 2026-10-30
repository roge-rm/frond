package com.rm.frond

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PalmKeysTest {
    private fun key(code: Int, shift: Boolean = false) = PalmKeys.Key(code, shift)

    @Test
    fun lettersAndCapitals() {
        assertEquals(key(KeyEvent.KEYCODE_A), PalmKeys.forChar('a'))
        assertEquals(key(KeyEvent.KEYCODE_Z), PalmKeys.forChar('z'))
        assertEquals(key(KeyEvent.KEYCODE_Q, true), PalmKeys.forChar('Q'))
    }

    @Test
    fun digitsAndTheirShiftedSymbols() {
        assertEquals(key(KeyEvent.KEYCODE_0), PalmKeys.forChar('0'))
        assertEquals(key(KeyEvent.KEYCODE_7), PalmKeys.forChar('7'))
        assertEquals(key(KeyEvent.KEYCODE_1, true), PalmKeys.forChar('!'))
        assertEquals(key(KeyEvent.KEYCODE_2, true), PalmKeys.forChar('@'))
        assertEquals(key(KeyEvent.KEYCODE_9, true), PalmKeys.forChar('('))
        assertEquals(key(KeyEvent.KEYCODE_0, true), PalmKeys.forChar(')'))
    }

    @Test
    fun punctuationOnUsPositions() {
        assertEquals(key(KeyEvent.KEYCODE_SEMICOLON, true), PalmKeys.forChar(':'))
        assertEquals(key(KeyEvent.KEYCODE_APOSTROPHE, true), PalmKeys.forChar('"'))
        assertEquals(key(KeyEvent.KEYCODE_SLASH, true), PalmKeys.forChar('?'))
        assertEquals(key(KeyEvent.KEYCODE_EQUALS, true), PalmKeys.forChar('+'))
        assertEquals(key(KeyEvent.KEYCODE_GRAVE, true), PalmKeys.forChar('~'))
        assertEquals(key(KeyEvent.KEYCODE_BACKSLASH), PalmKeys.forChar('\\'))
        assertEquals(key(KeyEvent.KEYCODE_PERIOD), PalmKeys.forChar('.'))
    }

    @Test
    fun controlCharacters() {
        assertEquals(key(KeyEvent.KEYCODE_SPACE), PalmKeys.forChar(' '))
        assertEquals(key(KeyEvent.KEYCODE_ENTER), PalmKeys.forChar('\n'))
        assertEquals(key(KeyEvent.KEYCODE_TAB), PalmKeys.forChar('\t'))
        assertEquals(key(KeyEvent.KEYCODE_DEL), PalmKeys.forChar('\b'))
    }

    @Test
    fun curlyQuotesBecomeStraight() {
        assertEquals(PalmKeys.forChar('\''), PalmKeys.forChar('’'))
        assertEquals(PalmKeys.forChar('"'), PalmKeys.forChar('“'))
    }

    @Test
    fun charactersWithoutAKeyAreSkipped() {
        assertNull(PalmKeys.forChar('é'))
        assertNull(PalmKeys.forChar('€'))
        assertEquals(
            listOf(key(KeyEvent.KEYCODE_H, true), key(KeyEvent.KEYCODE_I), key(KeyEvent.KEYCODE_1, true)),
            PalmKeys.forText("Hé€i!"),
        )
    }

    @Test
    fun editTypesOnlyWhatChanged() {
        assertEquals(PalmKeys.Edit(0, "lo"), PalmKeys.edit("hel", "hello"))
        assertEquals(PalmKeys.Edit(2, "p"), PalmKeys.edit("hello", "help"))
        assertEquals(PalmKeys.Edit(3, ""), PalmKeys.edit("abc", ""))
        assertEquals(PalmKeys.Edit(0, "abc"), PalmKeys.edit("", "abc"))
    }
}
