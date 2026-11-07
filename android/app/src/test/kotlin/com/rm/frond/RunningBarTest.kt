package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RunningBarTest {
    @Test fun barShowsByDefaultAndIsRemembered() {
        assertTrue(MachineSettings().barShown)
        assertTrue(MachineSettings.from(JSONObject()).barShown)
        val hidden = MachineSettings(barShown = false)
        assertFalse(hidden.json().getBoolean("barShown"))
        assertEquals(hidden, MachineSettings.from(hidden.json()))
        assertEquals(hidden.copy(barShown = true), MachineSettings.from(hidden.copy(barShown = true).json()))
    }

    @Test fun powerFollowsTheButtonBar() {
        for (device in Device.entries) assertEquals(device.id, "power" in device.buttons, RunningBar.powerKey(device))
        val tx = Device.TX
        assertFalse(RunningBar.powerKey(tx.copy(buttonsText = tx.buttons.filter { it != "power" }.joinToString("|"))))
    }

    @Test fun portraitMakesRoomOnlyWhileShown() {
        assertEquals(100, RunningBar.surfaceTopMargin(true, true, 100, 0))
        assertEquals(0, RunningBar.surfaceTopMargin(false, true, 100, 0))
        assertEquals(0, RunningBar.surfaceTopMargin(true, false, 100, 0))
        assertEquals(120, RunningBar.surfaceTopMargin(true, true, 100, 120))
        assertEquals(120, RunningBar.surfaceTopMargin(false, true, 100, 120))
    }

    @Test fun cornerButtonsShowWhenBarIsHidden() {
        assertTrue(RunningBar.cornerButtons(false))
        assertFalse(RunningBar.cornerButtons(true))
    }
}
