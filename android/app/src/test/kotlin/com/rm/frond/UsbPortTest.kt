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

    @Test fun startUsesWhatTheDeviceHas() {
        val dana = Device.entries.first { it.usbWifi }
        val other = Device.entries.first { !it.usbWifi }
        assertEquals("wifi", UsbPort.forStart(dana, "wifi"))
        assertEquals("off", UsbPort.forStart(other, "wifi"))
        assertEquals("off", UsbPort.forStart(other, "printer"))
        assertEquals("usb-port" to "wifi", UsbPort.startRequest("wifi"))
        assertEquals("printer" to "attach on", UsbPort.startRequest("printer"))
        assertNull(UsbPort.startRequest("off"))
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
