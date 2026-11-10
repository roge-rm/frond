package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RunningMenuTest {
    private fun rows(device: Device, state: RunningMenu.State = RunningMenu.State()) = RunningMenu.rows(device, state)
    private fun ids(device: Device) = rows(device).map { it.id }

    @Test fun everyDeviceGetsTheFixedRowsInOrder() {
        val fixed = listOf(
            RunningMenu.Id.DISPLAY, RunningMenu.Id.CONNECTIONS, RunningMenu.Id.SCREENSHOT, RunningMenu.Id.HIDE_BAR,
            RunningMenu.Id.SAVE_COPY, RunningMenu.Id.LOG, RunningMenu.Id.RESET, RunningMenu.Id.HARD_RESET,
        )
        for (device in Device.entries) assertEquals(device.id, fixed, ids(device).filter { it in fixed })
    }

    @Test fun holdShowsOnlyWhereTheDeviceHasIt() {
        for (device in Device.entries) assertEquals(device.id, device.holdPresent, RunningMenu.Id.HOLD in ids(device))
        assertTrue(Device.entries.any { it.holdPresent })
    }

    @Test fun holdFollowsTheCoreAndBusy() {
        val device = Device.entries.first { it.holdPresent && it.holdSupported }
        fun hold(s: RunningMenu.State) = rows(device, s).first { it.id == RunningMenu.Id.HOLD }
        assertFalse(hold(RunningMenu.State()).enabled)
        assertNull(hold(RunningMenu.State()).value)
        assertEquals("On", hold(RunningMenu.State(hold = true)).value)
        assertEquals("Off", hold(RunningMenu.State(hold = false)).value)
        assertTrue(hold(RunningMenu.State(hold = false)).enabled)
        assertFalse(hold(RunningMenu.State(hold = false, busy = true)).enabled)
    }

    @Test fun a68kDeviceListsOnlyItsHardware() {
        for (device in Device.entries.filter { it.runs68k }) {
            val ids = ids(device)
            assertEquals(device.id, device.keyboard, RunningMenu.Id.KEYBOARD in ids)
            assertEquals(device.id, device.microphone, RunningMenu.Id.MICROPHONE in ids)
            assertEquals(device.id, device.network, RunningMenu.Id.NETWORK in ids)
        }
    }

    @Test fun otherDevicesGreyOutWhatIsMissing() {
        for (device in Device.entries.filter { !it.runs68k }) {
            val byId = rows(device, RunningMenu.State(keyboard = false, network = NetworkMode.DISABLED)).associateBy { it.id }
            assertEquals(device.id, device.keyboard, byId.getValue(RunningMenu.Id.KEYBOARD).enabled)
            assertEquals(device.id, device.microphone, byId.getValue(RunningMenu.Id.MICROPHONE).enabled)
            assertEquals(device.id, device.network, byId.getValue(RunningMenu.Id.NETWORK).enabled)
        }
    }

    @Test fun keyboardNeedsTheCoreAndFreeInfraredPort() {
        val device = Device.entries.first { it.keyboard }
        fun row(s: RunningMenu.State) = rows(device, s).first { it.id == RunningMenu.Id.KEYBOARD }
        assertFalse(row(RunningMenu.State()).enabled)
        assertTrue(row(RunningMenu.State(keyboard = true)).enabled)
        assertEquals("On", row(RunningMenu.State(keyboard = true)).value)
        assertFalse(row(RunningMenu.State(keyboard = false, beamOff = false)).enabled)
    }

    @Test fun networkShowsItsModeOnceKnown() {
        val device = Device.entries.first { it.network }
        fun row(s: RunningMenu.State) = rows(device, s).first { it.id == RunningMenu.Id.NETWORK }
        assertFalse(row(RunningMenu.State()).enabled)
        assertEquals(NetworkMode.INTERNET.label, row(RunningMenu.State(network = NetworkMode.INTERNET)).value)
        assertTrue(row(RunningMenu.State(network = NetworkMode.INTERNET)).enabled)
    }

    @Test fun microphoneStates() {
        assertEquals("On", RunningMenu.microphoneValue("on"))
        assertEquals("On", RunningMenu.microphoneValue("pending"))
        assertEquals("Off", RunningMenu.microphoneValue("off"))
        assertEquals("Permission denied", RunningMenu.microphoneValue("off Permission denied"))
    }

    @Test fun onlyHardResetIsRedAndGroupsOnlyGrow() {
        for (device in Device.entries) {
            val rows = rows(device)
            assertEquals(listOf(RunningMenu.Id.HARD_RESET), rows.filter { it.destructive }.map { it.id })
            assertEquals(rows.map { it.group }.sorted(), rows.map { it.group })
        }
    }

    @Test fun screenshotNamesAreFileSafe() {
        assertEquals("Frond Palm TX 20261107-133000.png", RunningMenu.screenshotName("Palm TX", "20261107-133000"))
        assertEquals("Frond a_b 1.png", RunningMenu.screenshotName("a/b", "1"))
        assertEquals("Frond Palm 1.png", RunningMenu.screenshotName(" ", "1"))
    }
}
