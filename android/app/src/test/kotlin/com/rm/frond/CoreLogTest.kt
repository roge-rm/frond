package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Test

class CoreLogTest {
    @Test
    fun shortTextStaysWhole() {
        assertEquals("a\nb\n", CoreLog.tail("a\nb\n", 10))
    }

    @Test
    fun cutStartsAtAWholeLine() {
        assertEquals("three\n", CoreLog.tail("one\ntwo\nthree\n", 8))
    }

    @Test
    fun oneLongLineIsCut() {
        assertEquals("6789", CoreLog.tail("0123456789", 4))
    }

    @Test
    fun cutOnTheLastNewlineKeepsTheEnd() {
        assertEquals("ef\n", CoreLog.tail("abcdef\n", 3))
    }
}
