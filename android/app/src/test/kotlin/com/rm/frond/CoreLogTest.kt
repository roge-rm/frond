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

    @Test
    fun romRefusalIsTheMainText() {
        assertEquals(
            "Frond couldn't start this machine: this ROM has no serial number token.",
            CoreLog.refusal("starting core: x\nalphasmart-dana-wireless: this ROM has no serial number token\n"),
        )
    }

    @Test
    fun sessionRefusalIsTheMainText() {
        assertEquals(
            "Frond couldn't open this session: device alphasmart-dana doesn't match.",
            CoreLog.refusal("session refused: device alphasmart-dana doesn't match\n"),
        )
    }

    @Test
    fun otherFailuresHaveNoRefusal() {
        assertEquals(null, CoreLog.refusal("guest log: this ROM is fine\nSegmentation fault\n"))
    }
}
