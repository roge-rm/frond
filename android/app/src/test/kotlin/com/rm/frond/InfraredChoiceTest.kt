package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class InfraredChoiceTest {
    private val paired = listOf("aa11" to "Desk", "bb22" to "Laptop")

    @Test fun settingRoundTripsAndDefaultsOff() {
        assertEquals("off", MachineSettings().infrared)
        val s = MachineSettings(infrared = "link:aa11")
        assertEquals("link:aa11", s.json().getString("infrared"))
        assertEquals(s, MachineSettings.from(s.json()))
        // Settings saved before the key, and values Android can't use, are off.
        assertEquals("off", MachineSettings.from(JSONObject()).infrared)
        for (v in listOf("tcp:5030", "link:", "link:local", "pty", ""))
            assertEquals(v, "off", MachineSettings.from(JSONObject().put("infrared", v)).infrared)
    }

    @Test fun choicesListConnectionsAndKeepAGoneOne() {
        assertEquals(listOf("off" to "Off", "link:aa11" to "Desk", "link:bb22" to "Laptop"), InfraredChoice.choices("off", paired))
        assertEquals(listOf("off" to "Off", "link:aa11" to "Desk", "link:bb22" to "Laptop", "link:cc33" to "Unknown connection"),
            InfraredChoice.choices("link:cc33", paired))
        assertEquals(listOf("off" to "Off"), InfraredChoice.choices("off", emptyList()))
    }

    @Test fun aGoneConnectionActsAsOff() {
        assertEquals("aa11", InfraredChoice.active("link:aa11", paired))
        assertNull(InfraredChoice.active("link:cc33", paired))
        assertNull(InfraredChoice.active("off", paired))
        assertEquals("cc33", InfraredChoice.linkId("link:cc33"))
        assertNull(InfraredChoice.linkId("link:local"))
    }

    @Test fun statusLine() {
        assertEquals("Off", CoreLink.Status().text)
        assertEquals("Off. Restored keyboard owns the infrared UART.",
            CoreLink.Status("link:aa11", error = "Restored keyboard owns the infrared UART.").text)
        assertEquals("Infrared: Unknown connection", CoreLink.Status("link:cc33").text)
        assertEquals("Infrared: connecting", CoreLink.Status("link:aa11", active = true).text)
        val joined = CoreLink.Status("link:aa11", active = true, ir = LinkWire.IrStatus("joined", "Desk", machine = "Palm TX"))
        assertEquals("Infrared: Desk, joined to Palm TX", joined.text)
        // Beaming over a connection keeps the keyboard off the port.
        assertTrue(joined.active)
        assertFalse(CoreLink.Status().active)
    }
}
