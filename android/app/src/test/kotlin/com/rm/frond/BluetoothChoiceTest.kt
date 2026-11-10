package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BluetoothChoiceTest {
    private val paired = listOf("aa11" to "Desk", "bb22" to "Laptop")

    @Test fun settingRoundTripsAndDefaultsOff() {
        assertEquals("off", MachineSettings().bluetooth)
        for (v in listOf("radio", "link:aa11")) {
            val s = MachineSettings(bluetooth = v)
            assertEquals(v, s.json().getString("bluetooth"))
            assertEquals(s, MachineSettings.from(s.json()))
        }
        assertEquals("off", MachineSettings.from(JSONObject()).bluetooth)
        // The old Host and Join modes, the desktop's controller socket and This computer are off here.
        for (v in listOf("host:5032", "join:10.0.2.2:5032", "socket:/tmp/bt", "link:local", "link:", "on", ""))
            assertEquals(v, "off", MachineSettings.from(JSONObject().put("bluetooth", v)).bluetooth)
    }

    @Test fun pickerAsTheDesktopLabelsIt() {
        assertEquals(listOf("off" to "Off", "radio" to "This Frond", "link:aa11" to "Desk", "link:bb22" to "Laptop"),
            BluetoothChoice.choices("radio", paired))
        assertEquals(listOf("off" to "Off", "radio" to "This Frond", "link:cc33" to "Unknown connection"),
            BluetoothChoice.choices("link:cc33", emptyList()))
        assertEquals("radio", BluetoothChoice.active("radio", emptyList()))
        assertEquals("aa11", BluetoothChoice.active("link:aa11", paired))
        assertNull(BluetoothChoice.active("link:cc33", paired))
        assertNull(BluetoothChoice.active("off", paired))
    }

    @Test fun statusLine() {
        assertEquals("Off", BluetoothLink.Status().text)
        assertEquals("Bluetooth: Unknown connection", BluetoothLink.Status("link:cc33").text)
        assertEquals("Off. Couldn't make the Bluetooth socket.", BluetoothLink.Status("radio", error = "Couldn't make the Bluetooth socket.").text)
        assertEquals("Bluetooth: This Frond's radio", BluetoothLink.Status("radio", active = true).text)
        assertEquals("Bluetooth: connecting", BluetoothLink.Status("link:aa11", active = true).text)
        fun text(state: String, listening: Boolean = true) = LinkWire.bluetoothText(LinkWire.BtStatus(state, "Desk", listening))
        assertEquals("Bluetooth: This Frond's radio", text("radio"))
        assertEquals("Bluetooth: this Frond has no radio", text("noradio"))
        assertEquals("Bluetooth: this Frond's radio, with Desk", text("here"))
        assertEquals("Bluetooth: Desk's radio", text("there"))
        assertEquals("Bluetooth: Desk not found", text("notfound"))
        assertEquals("Bluetooth: Desk key changed, pair again", text("keychanged"))
        assertEquals("Bluetooth: pair again with Desk", text("forgotten"))
        assertEquals("Bluetooth: Unknown connection", text("unknown"))
        assertEquals("Bluetooth: local network access is off", text("nonetwork"))
        assertEquals("Bluetooth: connecting to Desk", text("connecting"))
        assertEquals("Bluetooth: couldn't make the controller socket", text("there", listening = false))
    }

    @Test fun radioRule() {
        val low = "00" + "11".repeat(15)
        val high = "ff" + "11".repeat(15)
        // Nobody chose: the lower install id holds the radio, as both sides work out the same.
        assertTrue(LinkWire.radioHere(low, high, "here", 0))
        assertFalse(LinkWire.radioHere(high, low, "here", 0))
        assertFalse(LinkWire.radioHere(low, high, "there", 5))
        assertTrue(LinkWire.radioHere(high, low, "here", 5))
        // A later choice wins: the peer says it holds the radio, so it's there for us.
        assertEquals(LinkWire.RadioUpdate.Take(false, 20), LinkWire.radioUpdate(low, high, "here", 10, senderHolds = true, at = 20))
        // An older one gets ours back.
        assertEquals(LinkWire.RadioUpdate.Answer, LinkWire.radioUpdate(low, high, "here", 10, senderHolds = true, at = 5))
        // The same choice at the same time changes nothing.
        assertEquals(LinkWire.RadioUpdate.Keep, LinkWire.radioUpdate(low, high, "here", 10, senderHolds = false, at = 10))
        // The same time and different choices: the lower id's wins on both sides.
        assertEquals(LinkWire.RadioUpdate.Keep, LinkWire.radioUpdate(low, high, "here", 10, senderHolds = true, at = 10))
        assertEquals(LinkWire.RadioUpdate.Take(false, 10), LinkWire.radioUpdate(high, low, "here", 10, senderHolds = true, at = 10))
        // Nobody chose on either side.
        assertEquals(LinkWire.RadioUpdate.Keep, LinkWire.radioUpdate(low, high, "here", 0, senderHolds = false, at = 0))
        // A newer time with the same side is kept with its time.
        assertEquals(LinkWire.RadioUpdate.Take(true, 30), LinkWire.radioUpdate(low, high, "here", 10, senderHolds = false, at = 30))
    }

    @Test fun connectionRowsCarryTheRadio() {
        val e = JSONObject().put("connections", org.json.JSONArray()
            .put(JSONObject().put("id", "aa").put("name", "Desk").put("bluetoothRadio", "there"))
            .put(JSONObject().put("id", "bb").put("name", "Old")))
        assertEquals(listOf("there", "here"), ConnectionsModel.parseRows(e).map { it.bluetoothRadio })
    }
}
