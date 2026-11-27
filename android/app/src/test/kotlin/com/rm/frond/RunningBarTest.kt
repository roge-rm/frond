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

    @Test fun recordFollowsTheVoiceKey() {
        val withKey = Device.entries.filter { RunningBar.recordKey(it) }.map { it.id }.toSet()
        assertEquals(setOf("palm-zire-72", "sony-clie-tg50", "hp-ipaq-rx1950"), withKey)
        for (device in Device.entries) assertEquals(device.id, device.voiceKey, RunningSheets.AudioRow.RECORD_KEY in RunningSheets.audioRows(device))
    }

    @Test fun recordKeyChoiceMapsToPhoneKeysAndIsRemembered() {
        assertEquals(RecordKey.NONE, MachineSettings().recordKey)
        assertFalse(MachineSettings().json().has("recordKey"))
        assertFalse(RecordKey.NONE.matches(0) || RecordKey.NONE.matches(android.view.KeyEvent.KEYCODE_VOLUME_DOWN))
        assertTrue(RecordKey.VOLUME_DOWN.matches(android.view.KeyEvent.KEYCODE_VOLUME_DOWN))
        assertFalse(RecordKey.VOLUME_DOWN.matches(android.view.KeyEvent.KEYCODE_VOLUME_UP))
        assertTrue(RecordKey.VOLUME_UP.matches(android.view.KeyEvent.KEYCODE_VOLUME_UP))
        assertTrue(RecordKey.L3.matches(android.view.KeyEvent.KEYCODE_BUTTON_THUMBL))
        assertTrue(RecordKey.SELECT.matches(android.view.KeyEvent.KEYCODE_BUTTON_SELECT))
        for (key in RecordKey.entries) {
            val settings = MachineSettings(recordKey = key)
            assertEquals(key, MachineSettings.from(settings.json()).recordKey)
        }
        assertEquals(RecordKey.NONE, RecordKey.byArg("junk"))
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

    @Test fun landscapeHandleSitsBesideThePalm() {
        assertTrue(RunningBar.handleCentred(portrait = true))
        assertFalse(RunningBar.handleCentred(portrait = false))
    }

    @Test fun onlyTheRequestPermissionsActionNeedsRequestPermissions() {
        assertTrue(BarResults.isPermissionRequest("androidx.activity.result.contract.action.REQUEST_PERMISSIONS"))
        assertFalse(BarResults.isPermissionRequest("android.intent.action.OPEN_DOCUMENT"))
        assertFalse(BarResults.isPermissionRequest(null))
    }

    @Test fun controllerDetectionIgnoresOnScreenDevices() {
        val pad = android.view.InputDevice.SOURCE_GAMEPAD or android.view.InputDevice.SOURCE_KEYBOARD
        assertTrue(RunningBar.isGameController(pad, false))
        assertTrue(RunningBar.isGameController(android.view.InputDevice.SOURCE_JOYSTICK, false))
        assertFalse(RunningBar.isGameController(pad, true))
        assertFalse(RunningBar.isGameController(android.view.InputDevice.SOURCE_TOUCHSCREEN, false))
        assertFalse(RunningBar.isGameController(android.view.InputDevice.SOURCE_KEYBOARD, false))
        assertFalse(RunningBar.isGameController(android.view.InputDevice.SOURCE_DPAD, false))
        assertFalse(RunningBar.isGameController(pad, false, external = false))
    }

    @Test fun touchControlsGoOnlyWithTheSettingAndAController() {
        assertTrue(RunningBar.touchHidden(true, true))
        assertFalse(RunningBar.touchHidden(true, false))
        assertFalse(RunningBar.touchHidden(false, true))
        assertEquals(0, RunningBar.surfaceTopMargin(BarMode.OWN_SPACE, true, 100, 800, 200, 96, touchHidden = true))
        assertEquals("fit=fit smooth=off jog=hidden jog-landscape=hidden buttons=off",
            MachineSettings().displayArgument(jogControls = true, gamepadSupported = true, touchHidden = true))
        val launch = MachineSettings().arguments(jogControls = true, gamepadSupported = true, touchHidden = true)
        assertTrue("--hide-buttons" in launch)
        assertEquals("hidden", launch[launch.indexOf("--jog-controls") + 1])
        assertFalse("--hide-buttons" in MachineSettings().arguments())
    }

    @Test fun touchHiddenDisplayArgumentTurnsTheButtonBarOffOnAnyDevice() {
        assertEquals("fit=fit smooth=off buttons=off", MachineSettings().displayArgument(touchHidden = true))
        assertEquals("fit=fit smooth=off buttons=on", MachineSettings().displayArgument())
    }

    @Test fun backOpensTheBarWhileTouchControlsAreHidden() {
        val b = RunningBar.BackAction.values().associateBy { it.name }
        assertEquals(b["LEAVE"], RunningBar.backAction(false, false, false, false))
        assertEquals(b["SHOW_BAR"], RunningBar.backAction(true, false, false, false))
        assertEquals(b["HIDE_BAR"], RunningBar.backAction(true, true, false, false))
        assertEquals(b["CLOSE_MENU"], RunningBar.backAction(true, true, false, true))
        assertEquals(b["CLOSE_SHEET"], RunningBar.backAction(true, true, true, true))
        assertEquals(b["CLOSE_SHEET"], RunningBar.backAction(false, false, true, false))
    }
}
