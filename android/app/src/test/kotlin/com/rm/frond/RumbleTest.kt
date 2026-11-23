package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RumbleTest {
    private fun t(enabled: Boolean, connected: Boolean, pad: Boolean, phone: Boolean) =
        RumbleChoice.target(enabled, connected, pad, phone)

    @Test fun controllerConnectedNeverUsesPhone() {
        assertEquals(RumbleTarget.CONTROLLER, t(true, true, true, true))
        assertEquals(RumbleTarget.CONTROLLER, t(true, true, true, false))
        assertEquals(RumbleTarget.NONE, t(true, true, false, true))
        assertEquals(RumbleTarget.NONE, t(true, true, false, false))
    }

    @Test fun noControllerUsesPhone() {
        assertEquals(RumbleTarget.PHONE, t(true, false, false, true))
        assertEquals(RumbleTarget.NONE, t(true, false, false, false))
    }

    @Test fun offGoesNowhere() {
        assertEquals(RumbleTarget.NONE, t(false, true, true, true))
        assertEquals(RumbleTarget.NONE, t(false, false, false, true))
    }

    @Test fun picksLastUsedThenFirstThatVibrates() {
        val v = { id: Int -> id != 2 }
        assertEquals(3, RumbleChoice.pick(listOf(1, 2, 3), v, 3))
        assertEquals(1, RumbleChoice.pick(listOf(1, 2, 3), v, 2))
        assertEquals(1, RumbleChoice.pick(listOf(1, 2, 3), v, 9))
        assertEquals(3, RumbleChoice.pick(listOf(2, 3), v, null))
        assertEquals(null, RumbleChoice.pick(listOf(2), v, 2))
        assertEquals(null, RumbleChoice.pick(emptyList(), v, null))
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
