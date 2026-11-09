package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class SerialChoiceTest {
    @Test fun settingRoundTripsAndDefaultsOff() {
        assertEquals("off", MachineSettings().serial)
        val s = MachineSettings(serial = "link:aa11", infrared = "link:bb22")
        assertEquals("link:aa11", s.json().getString("serial"))
        assertEquals(s, MachineSettings.from(s.json()))
        // Settings from before, and the old LAN modes, are off.
        assertEquals("off", MachineSettings.from(JSONObject()).serial)
        for (v in listOf("tcp:5023", "listen:5033", "connect:10.0.2.2:5033", "link:", "link:local", "pty", ""))
            assertEquals(v, "off", MachineSettings.from(JSONObject().put("serial", v)).serial)
    }

    @Test fun modemChoice() {
        val s = MachineSettings(serial = "modem")
        assertEquals("modem", s.json().getString("serial"))
        assertEquals(s, MachineSettings.from(s.json()))
        // Only the serial port takes it.
        assertEquals("off", MachineSettings.from(JSONObject().put("infrared", "modem")).infrared)
        val paired = listOf("aa11" to "Desk")
        assertEquals(listOf("off", "modem", "link:aa11"), InfraredChoice.serialChoices("off", paired, true).map { it.first })
        assertEquals(listOf("off", "link:aa11"), InfraredChoice.serialChoices("off", paired, false).map { it.first })
        assertEquals("Modem", CoreLink.Status("modem", title = "Serial").text)
    }

    @Test fun statusLine() {
        assertEquals("Off", CoreLink.Status(title = "Serial").text)
        assertEquals("Serial: Unknown connection", CoreLink.Status("link:cc33", title = "Serial").text)
        assertEquals("Serial: connecting", CoreLink.Status("link:aa11", active = true, title = "Serial").text)
        fun joined(far: String) = CoreLink.Status("link:aa11", active = true, title = "Serial",
            ir = LinkWire.IrStatus("joined", "Desk", machine = "Palm TX", far = far)).text
        assertEquals("Serial: Desk, joined to Palm TX", joined("running"))
        assertEquals("Serial: Desk, port closed", joined("asleep"))
        assertEquals("Serial: Desk, Palm paused", joined("paused"))
        assertEquals("Serial: waiting for a machine on Desk", LinkWire.channelText("Serial", LinkWire.IrStatus("waiting", "Desk")))
        assertEquals("Serial on Desk is in use by Palm E2", LinkWire.channelText("Serial", LinkWire.IrStatus("busy", "Desk", busyBy = "Palm E2")))
        // Infrared keeps its own words.
        assertEquals("Infrared: Desk, Palm asleep", LinkWire.irText(LinkWire.IrStatus("joined", "Desk", far = "asleep")))
        assertEquals("Serial", SerialLink.status.value.title)
        assertEquals("Infrared", IrLink.status.value.title)
    }
}
