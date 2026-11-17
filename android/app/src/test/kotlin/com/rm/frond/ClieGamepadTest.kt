package com.rm.frond

import android.view.KeyEvent
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ClieGamepadTest {
    private val tg50 get() = Device.byId("sony-clie-tg50")!!

    @Test fun onlyTheTg50TakesTheGamepad() {
        assertEquals(listOf("sony-clie-tg50"), Device.entries.filter { it.clieGamepad }.map { it.id })
    }

    @Test fun settingSavesAndLoads() {
        assertNull(MachineSettings().gamepad)
        assertFalse(MachineSettings().json().has("gamepad"))
        for (on in listOf(true, false)) {
            val saved = MachineSettings(gamepad = on).json()
            assertEquals(on, saved.getBoolean("gamepad"))
            assertEquals(on, MachineSettings.from(JSONObject(saved.toString()), MachineSettings()).gamepad)
        }
        assertEquals(true, MachineSettings.from(JSONObject(), MachineSettings(gamepad = true)).gamepad)
    }

    @Test fun startArgumentOnlyWhenChosenAndSupported() {
        assertEquals(listOf("--gamepad", "on"), MachineSettings(gamepad = true).arguments(gamepadSupported = true).let { it.subList(it.indexOf("--gamepad"), it.indexOf("--gamepad") + 2) })
        assertTrue("off" in MachineSettings(gamepad = false).arguments(gamepadSupported = true))
        assertFalse("--gamepad" in MachineSettings().arguments(gamepadSupported = true))
        assertFalse("--gamepad" in MachineSettings(gamepad = true).arguments())
    }

    @Test fun menuRowOnlyOnTheTg50() {
        for (device in Device.entries) {
            assertEquals(device.id, device.clieGamepad, RunningMenu.Id.GAMEPAD in RunningMenu.rows(device, RunningMenu.State()).map { it.id })
        }
    }

    @Test fun menuRowFollowsTheCore() {
        fun row(gamepad: String?, busy: Boolean = false) =
            RunningMenu.rows(tg50, RunningMenu.State(gamepad = gamepad, busy = busy)).first { it.id == RunningMenu.Id.GAMEPAD }
        assertFalse(row(null).enabled)
        assertEquals("On", row("attached").value)
        assertEquals("Off", row("detached").value)
        assertTrue(row("detached").enabled)
        assertFalse(row("detached", busy = true).enabled)
        assertEquals("Game controller (connector in use)", row("busy").label)
        assertFalse(row("busy").enabled)
        assertEquals("busy", RunningMenu.gamepadState("detached busy"))
        assertEquals("attached", RunningMenu.gamepadState("attached identified ids=1"))
        assertNull(RunningMenu.gamepadState("unavailable"))
    }

    @Test fun defaultMapHasThePadButtons() {
        val map = ControllerMap.defaults(tg50)
        assertEquals(PalmButton.UP, map.keys[KeyEvent.KEYCODE_DPAD_UP])
        assertEquals(PalmButton.RIGHT, map.keys[KeyEvent.KEYCODE_DPAD_RIGHT])
        assertEquals(PalmButton.PAD_A, map.keys[KeyEvent.KEYCODE_BUTTON_A])
        assertEquals(PalmButton.PAD_B, map.keys[KeyEvent.KEYCODE_BUTTON_B])
        assertEquals(PalmButton.PAD_X, map.keys[KeyEvent.KEYCODE_BUTTON_X])
        assertEquals(PalmButton.PAD_Y, map.keys[KeyEvent.KEYCODE_BUTTON_Y])
        assertEquals(PalmButton.MENU, map.keys[KeyEvent.KEYCODE_BUTTON_START])
        assertEquals(PalmButton.HOME, map.keys[KeyEvent.KEYCODE_BUTTON_SELECT])
        assertEquals(PalmButton.PAD_HOLD, map.keys[KeyEvent.KEYCODE_BUTTON_THUMBR])
        assertTrue(map.keys.values.all { it in PalmButton.on(tg50) })
        assertEquals(map, ControllerMap.decode(map.encode(), tg50))
    }

    @Test fun otherDevicesHaveNoPadButtons() {
        for (device in Device.entries.filter { !it.clieGamepad }) {
            assertTrue(device.id, PalmButton.on(device).none { it.gamepadOnly })
        }
        assertEquals(11, listOf(PalmButton.UP, PalmButton.DOWN, PalmButton.LEFT, PalmButton.RIGHT, PalmButton.HOME, PalmButton.MENU)
            .plus(PalmButton.PAD).count { PalmButtons.padScancode(it) != 0 })
    }
}
