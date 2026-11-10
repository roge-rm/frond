package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class UsbPortTest {
    @Test fun onlyTheDanaOffersWifi() {
        for (device in Device.entries) {
            val choices = UsbPort.choices(device)
            assertEquals(device.id, UsbPort.OFF, choices.first())
            assertEquals(device.id, device.usbPrinter, UsbPort.PRINTER in choices)
            assertEquals(device.id, device.usbWifi, UsbPort.WIFI in choices)
        }
        val dana = Device.entries.first { it.usbWifi }
        assertEquals(listOf("off", "printer", "wifi"), UsbPort.choices(dana))
        assertEquals(listOf("Off", "Printer", "Wi-Fi adapter"), UsbPort.choices(dana).map { UsbPort.label(it) })
        assertEquals(1, Device.entries.count { it.usbWifi })
    }

    @Test fun wirelessOffersPrinterAndItsOwnModule() {
        val wireless = Device.byId("alphasmart-dana-wireless")!!
        assertTrue(wireless.builtInWifi && wireless.usbPrinter && !wireless.usbWifi && wireless.isDana())
        assertEquals(listOf("off", "printer"), UsbPort.choices(wireless))
        // The external Wi-Fi adapter isn't offered, and an old saved choice of it falls back to Off.
        assertEquals("off", UsbPort.forStart(wireless, "wifi"))
        assertEquals("printer", UsbPort.forStart(wireless, "printer"))
        assertEquals("printer" to "attach on", UsbPort.startRequest(wireless, "printer"))
        assertEquals("usb-port" to "off", UsbPort.startRequest(wireless, "off"))
        assertEquals("network-mode" to "wireless", UsbPort.moduleRequest(wireless, true))
        assertEquals("network-mode" to "disabled", UsbPort.moduleRequest(wireless, false))
        for (device in Device.entries.filter { it != wireless }) {
            assertFalse(device.id, device.builtInWifi)
            assertNull(device.id, UsbPort.moduleRequest(device, true))
        }
        assertEquals(1, Device.entries.count { it.builtInWifi })
        assertTrue(Device.byId("alphasmart-dana")!!.isDana())
        assertFalse(wireless.infraredKeyboard)
    }

    @Test fun moduleChoiceIsKeptPerMachine() {
        assertTrue(MachineSettings().wifiModule)
        // Written either way, so a missing field can fall back to the caller's choice.
        assertTrue(MachineSettings().json().getBoolean("wifiModule"))
        val off = MachineSettings(wifiModule = false).json()
        assertFalse(MachineSettings.from(off, MachineSettings()).wifiModule)
        assertTrue(MachineSettings.from(MachineSettings(wifiModule = true).json(), MachineSettings(wifiModule = false)).wifiModule)
    }

    @Test fun moduleReadouts() {
        assertEquals("Off", PrintJob.moduleReadout("off", false))
        assertEquals("Waiting for the Dana", PrintJob.moduleReadout("off", true))
        assertEquals("Connected, 10.0.2.15", PrintJob.moduleReadout("powered link=up ip=10.0.2.15 mac=02:00:00:00:00:02 tx=3 rx=4 dropped=0", true))
        assertEquals("Not connected", PrintJob.moduleReadout("powered link=down ip= mac=02:00:00:00:00:02 tx=0 rx=0 dropped=0", true))
        assertEquals("Waiting for the Dana", PrintJob.moduleReadout("unpowered link=down ip= mac=02:00:00:00:00:02 tx=0 rx=0 dropped=0", true))
    }

    @Test fun startUsesWhatTheDeviceHas() {
        val dana = Device.entries.first { it.usbWifi }
        val other = Device.entries.first { !it.usbWifi }
        assertEquals("wifi", UsbPort.forStart(dana, "wifi"))
        assertEquals("off", UsbPort.forStart(other, "wifi"))
        assertEquals("off", UsbPort.forStart(other, "printer"))
        assertEquals("usb-port" to "wifi", UsbPort.startRequest(dana, "wifi"))
        assertEquals("printer" to "attach on", UsbPort.startRequest(dana, "printer"))
        // Off goes to the core too, since a session brings the device it was saved with.
        assertEquals("usb-port" to "off", UsbPort.startRequest(dana, "off"))
        assertNull(UsbPort.startRequest(Device.entries.first { !it.usbWifi && !it.usbPrinter }, "off"))
    }

    @Test fun settingsKeepTheChoice() {
        for (port in listOf("off", "printer", "wifi")) {
            val saved = MachineSettings(usbPort = port).json()
            assertEquals(port, saved.getString("usbPort"))
            assertEquals(port, MachineSettings.from(saved, MachineSettings()).usbPort)
        }
        // Older settings only have the printer switch.
        assertEquals("printer", MachineSettings.from(JSONObject().put("usbPrinter", true), MachineSettings()).usbPort)
        assertEquals("off", MachineSettings.from(JSONObject().put("usbPrinter", false), MachineSettings()).usbPort)
        assertEquals("off", MachineSettings.from(JSONObject().put("usbPort", "serial"), MachineSettings()).usbPort)
        assertTrue(MachineSettings(usbPort = "printer").json().getBoolean("usbPrinter"))
        assertFalse(MachineSettings(usbPort = "wifi").json().getBoolean("usbPrinter"))
    }

    @Test fun linkStateReads() {
        val up = PrintJob.parsePort("wifi powered link=up ip=10.0.2.15 mac=02:00:00:00:00:01 tx=3 rx=4 dropped=0")
        assertTrue(up.valid && up.powered && up.linkUp)
        assertEquals("Connected, 10.0.2.15", PrintJob.wifiReadout(up))
        assertEquals("Not connected", PrintJob.wifiReadout(PrintJob.parsePort("wifi powered link=down ip= mac=02:00:00:00:00:01 tx=0 rx=0 dropped=0")))
        assertEquals("Waiting for the Dana", PrintJob.wifiReadout(PrintJob.parsePort("wifi unpowered link=down ip= mac=02:00:00:00:00:01 tx=0 rx=0 dropped=0")))
        assertNull(PrintJob.wifiReadout(PrintJob.parsePort("printer attached collecting bytes=0 last=none idle=-1")))
        assertTrue(PrintJob.parsePort("printer attached collecting bytes=0 last=none idle=-1").valid)
        assertTrue(PrintJob.parsePort("off").valid)
        assertFalse(PrintJob.parsePort("").valid)
        assertFalse(PrintJob.parsePort("Use usb-port off|printer|wifi.").valid)
    }
}
