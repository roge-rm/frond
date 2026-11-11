package com.rm.frond

import com.rm.frond.ui.bluetoothText
import org.junit.Assert.assertEquals
import org.junit.Test

class BluetoothTextTest {
    private val radio = BluetoothLink.Status(BluetoothChoice.RADIO, active = true)

    @Test fun radioNoteStartsASentence() {
        assertEquals("Bluetooth: This Frond's radio. Configured; radio off", bluetoothText(radio, "configured; radio off"))
        assertEquals("Bluetooth: This Frond's radio", bluetoothText(radio, "off"))
        assertEquals("Bluetooth: This Frond's radio", bluetoothText(radio, null))
    }
}
