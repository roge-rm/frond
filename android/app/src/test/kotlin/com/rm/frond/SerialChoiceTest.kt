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

    @Test fun localSyncIsOnlyOfferedOnSupportedSerialPorts() {
        val choice = InfraredChoice.SYNC_MANAGER
        val settings = MachineSettings(serial = choice)
        assertEquals(choice, MachineSettings.from(settings.json()).serial)
        assertEquals("Sync Manager", CoreLink.Status(choice, title = "Serial").text)
        assertEquals(listOf("off", choice), InfraredChoice.serialChoices("off", emptyList(), modem = false, sync = true).map { it.first })
        assertEquals(listOf("off"), InfraredChoice.serialChoices("off", emptyList(), modem = false).map { it.first })
    }

    @Test fun irHotSyncIsOnlyOfferedOnHotSyncDevicesWithInfrared() {
        val choice = InfraredChoice.SYNC_MANAGER
        assertEquals(choice, InfraredChoice.normaliseInfrared(choice))
        assertEquals(choice, MachineSettings.from(MachineSettings(infrared = choice).json()).infrared)
        val paired = listOf("aa11" to "Desk")
        assertEquals(listOf("off", "printer", choice, "link:aa11"),
            InfraredChoice.infraredChoices("off", paired, false, sync = true).map { it.first })
        assertEquals(listOf("off", "printer", "link:aa11"), InfraredChoice.infraredChoices("off", paired, false).map { it.first })
        assertEquals("Sync Manager", CoreLink.Status(choice, title = "Infrared").text)
        val iii = requireNotNull(Device.byId("palm-iii"))
        assertEquals(choice, MachineSettings(infrared = choice).forDevice(iii, Device.TX).infrared)
        val rx1950 = Device.entries.firstOrNull { SyncProtocol.forDevice(it) == SyncProtocol.ACTIVE_SYNC && it.infrared }
        if (rx1950 != null) assertEquals("off", MachineSettings(infrared = choice).forDevice(iii, rx1950).infrared)
    }

    @Test fun portableKeyboardIsOnlyOfferedWhereTheDeviceTakesIt() {
        val keyboard = InfraredChoice.KEYBOARD
        assertEquals(keyboard, MachineSettings.from(MachineSettings(serial = keyboard).json()).serial)
        assertEquals("off", InfraredChoice.normaliseInfrared(keyboard))
        val iii = requireNotNull(Device.byId("palm-iii"))
        val iiix = requireNotNull(Device.byId("palm-iiix"))
        assertEquals(true, iii.serialKeyboard && iiix.serialKeyboard)
        assertEquals(keyboard, MachineSettings(serial = keyboard).forDevice(iii, iiix).serial)
        assertEquals("off", MachineSettings(serial = keyboard).forDevice(iiix, Device.TX).serial)
        assertEquals(listOf("off", keyboard), InfraredChoice.serialChoices("off", emptyList(), modem = false, keyboard = true).map { it.first })
        assertEquals("Keyboard: ready", CoreLink.Status(keyboard, title = "Serial", native = "keyboard ready").text)
        assertEquals("Keyboard: low power", CoreLink.Status(keyboard, title = "Serial", native = "keyboard low-power").text)
    }

    @Test fun directPppIsOnlyOfferedOnSupportedSerialPorts() {
        val ppp = InfraredChoice.PPP
        assertEquals(ppp, MachineSettings.from(MachineSettings(serial = ppp).json()).serial)
        val iiix = requireNotNull(Device.byId("palm-iiix"))
        assertEquals("off", MachineSettings(serial = ppp).forDevice(iiix, Device.TX).serial)
        assertEquals("off", InfraredChoice.normaliseInfrared(ppp))
        assertEquals(listOf("off", ppp), InfraredChoice.serialChoices("off", emptyList(), modem = false, ppp = true).map { it.first })
        assertEquals("Direct PPP: online", CoreLink.Status(ppp, title = "Serial", native = "ppp online").text)
        assertEquals("Modem: online at 33600",
            CoreLink.Status("modem", title = "Serial", native = "modem online 33600; hotsync sync waiting modem").text)
    }

    @Test fun bluetoothPhoneUsesTheLocalRadioAndKeepsItsOwnSetting() {
        val phone = BluetoothChoice.PHONE
        assertEquals(phone, MachineSettings.from(MachineSettings(bluetooth = phone).json()).bluetooth)
        assertEquals(BluetoothChoice.RADIO, BluetoothChoice.active(phone, emptyList()))
        assertEquals(phone to "Mobile phone", BluetoothChoice.choices("off", emptyList()).last())
        assertEquals("Mobile phone: online at 33600",
            BluetoothLink.Status(choice = phone, active = true, phone = "online 33600").text)
    }

    @Test fun phoneChoice() {
        val s = MachineSettings(infrared = "phone")
        assertEquals("phone", s.json().getString("infrared"))
        assertEquals(s, MachineSettings.from(s.json()))
        // Only the infrared port takes it.
        assertEquals("off", MachineSettings.from(JSONObject().put("serial", "phone")).serial)
        val paired = listOf("aa11" to "Desk")
        assertEquals(listOf("off", "phone", "printer", "link:aa11"), InfraredChoice.infraredChoices("off", paired, true).map { it.first })
        assertEquals(listOf("off", "printer", "link:aa11"), InfraredChoice.infraredChoices("off", paired, false).map { it.first })
        assertEquals(listOf("off", "link:aa11"), InfraredChoice.infraredChoices("off", paired, false, false).map { it.first })
        assertEquals("Mobile phone", CoreLink.Status("phone", title = "Infrared").text)
    }

    @Test fun printerChoice() {
        val s = MachineSettings(infrared = "printer")
        assertEquals("printer", s.json().getString("infrared"))
        assertEquals(s, MachineSettings.from(s.json()))
        // Only the infrared port takes it.
        assertEquals("off", MachineSettings.from(JSONObject().put("serial", "printer")).serial)
        assertEquals("off", MachineSettings.from(JSONObject().put("bluetooth", "printer")).bluetooth)
        assertEquals("Printer", CoreLink.Status("printer", title = "Infrared").text)
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
