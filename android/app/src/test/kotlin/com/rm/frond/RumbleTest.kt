package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RumbleTest {
    @Test fun controllerFirstThenPhone() {
        assertEquals(RumbleTarget.CONTROLLER, RumbleChoice.target(enabled = true, controllerVibrates = true, phoneVibrates = true))
        assertEquals(RumbleTarget.CONTROLLER, RumbleChoice.target(enabled = true, controllerVibrates = true, phoneVibrates = false))
        assertEquals(RumbleTarget.PHONE, RumbleChoice.target(enabled = true, controllerVibrates = false, phoneVibrates = true))
        assertEquals(RumbleTarget.NONE, RumbleChoice.target(enabled = true, controllerVibrates = false, phoneVibrates = false))
    }

    @Test fun offGoesNowhere() {
        assertEquals(RumbleTarget.NONE, RumbleChoice.target(enabled = false, controllerVibrates = true, phoneVibrates = true))
        assertEquals(RumbleTarget.NONE, RumbleChoice.target(enabled = false, controllerVibrates = false, phoneVibrates = true))
    }

    @Test fun settingIsOnByDefaultAndSaved() {
        val zodiac = Device.byId(Device.ZODIAC_ID)!!
        assertTrue(MachineSettings.defaults(zodiac).rumble)
        // Settings saved before the setting have it on.
        assertTrue(MachineSettings.from(JSONObject().put("engine", "fast")).rumble)
        val off = MachineSettings.defaults(zodiac).copy(rumble = false)
        assertFalse(off.json().getBoolean("rumble"))
        assertEquals(off, MachineSettings.from(off.json()))
        assertTrue(MachineSettings.from(MachineSettings().json()).rumble)
    }
}
