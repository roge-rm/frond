package com.rm.frond

import android.media.midi.MidiDeviceInfo.PortInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ZodiacMidiTest {
    @Test fun firstInputPort() {
        assertEquals(2, ZodiacMidiChoice.inputPort(listOf(PortInfo.TYPE_OUTPUT to 0, PortInfo.TYPE_INPUT to 2, PortInfo.TYPE_INPUT to 3)))
        assertNull(ZodiacMidiChoice.inputPort(listOf(PortInfo.TYPE_OUTPUT to 0)))
        assertNull(ZodiacMidiChoice.inputPort(emptyList()))
    }

    @Test fun flagOnlyWithADevice() {
        assertEquals(listOf("--music-midi-out", "7"), ZodiacMidiChoice.arguments(7))
        assertEquals(emptyList<String>(), ZodiacMidiChoice.arguments(null))
    }
}
