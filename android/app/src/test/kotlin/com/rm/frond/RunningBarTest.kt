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

    @Test fun barModeIsRemembered() {
        assertEquals(BarMode.AUTO_HIDE, MachineSettings().barMode)
        assertEquals(BarMode.AUTO_HIDE, MachineSettings.from(JSONObject()).barMode)
        val space = MachineSettings(barMode = BarMode.OWN_SPACE)
        assertEquals(space, MachineSettings.from(space.json()))
        assertEquals(BarMode.AUTO_HIDE, MachineSettings.from(JSONObject().put("barMode", "nonsense")).barMode)
    }

    @Test fun landscapeBarIsShorter() {
        assertTrue(RunningBar.heightDp(false) < RunningBar.heightDp(true))
    }

    @Test fun ownSpaceMovesThePalmDownInBothOrientations() {
        for (bar in listOf(RunningBar.HEIGHT_DP, RunningBar.LANDSCAPE_HEIGHT_DP)) {
            assertEquals(bar, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, true, bar, 800, 200, 0))
            assertEquals(0, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, false, bar, 800, 200, 0))
            assertEquals(0, RunningBar.surfaceTopMargin(BarMode.AUTO_HIDE, true, bar, 800, 200, 0))
        }
    }

    @Test fun ownSpaceOverlaysWhenThePalmWouldGetTooLittleRoom() {
        assertTrue(RunningBar.takesSpace(BarMode.OWN_SPACE, true, 100, 300, 200))
        assertFalse(RunningBar.takesSpace(BarMode.OWN_SPACE, true, 100, 299, 200))
        assertEquals(0, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, true, 100, 250, 200, 0))
    }

    @Test fun softKeyboardButtonKeepsTheSurfaceBelowIt() {
        assertEquals(120, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, true, 100, 800, 200, 120))
        assertEquals(120, RunningBar.surfaceTopMargin(BarMode.AUTO_HIDE, false, 100, 800, 200, 120))
        assertEquals(130, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, true, 130, 800, 200, 120))
    }

    @Test fun autoHideWaitsOutMenusSheetsAndDialogs() {
        assertTrue(RunningBar.autoHides(BarMode.AUTO_HIDE, true, false, false, false))
        assertFalse(RunningBar.autoHides(BarMode.AUTO_HIDE, false, false, false, false))
        assertFalse(RunningBar.autoHides(BarMode.OWN_SPACE, true, false, false, false))
        assertFalse(RunningBar.autoHides(BarMode.AUTO_HIDE, true, true, false, false))
        assertFalse(RunningBar.autoHides(BarMode.AUTO_HIDE, true, false, true, false))
        assertFalse(RunningBar.autoHides(BarMode.AUTO_HIDE, true, false, false, true))
    }

    @Test fun sheetsAreSideSheetsInLandscape() {
        assertFalse(RunningBar.sideSheet(true))
        assertTrue(RunningBar.sideSheet(false))
        assertEquals(360, RunningBar.sideSheetWidthDp(900))
        assertEquals(270, RunningBar.sideSheetWidthDp(600))
    }

    @Test fun cornerButtonsShowWhenBarIsHidden() {
        assertTrue(RunningBar.cornerButtons(false))
        assertFalse(RunningBar.cornerButtons(true))
    }

    @Test fun onlyTheRequestPermissionsActionNeedsRequestPermissions() {
        assertTrue(BarResults.isPermissionRequest("androidx.activity.result.contract.action.REQUEST_PERMISSIONS"))
        assertFalse(BarResults.isPermissionRequest("android.intent.action.OPEN_DOCUMENT"))
        assertFalse(BarResults.isPermissionRequest(null))
    }
}
