package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PalmButtonsTest {
    @Test fun physicalHomeUsesTheDescriptorWithoutASilkscreenTap() {
        val key = (1 shl 30) or 63
        val physical = Device.E2.copy(homeKey = key, homeX = -1, homeY = -1, silkscreenHeight = 0)
        assertEquals(key, PalmButtons.sdlKey(PalmButton.HOME, physical))
        assertNull(PalmButtons.tapPoint(physical, PalmButton.HOME, PalmButtons.PORTRAIT))
        assertEquals(0, PalmButtons.sdlKey(PalmButton.HOME, Device.E2))
        assertEquals(PalmButtons.sdlKey(PalmButton.APP1), PalmButtons.sdlKey(PalmButton.APP1, physical))
    }

    @Test fun controllerDefaultsOnlyOfferTheDeviceButtons() {
        val device = Device.TX.copy(buttonsText = "up|down|home|power")
        val map = ControllerMap.defaults(device)
        org.junit.Assert.assertTrue(map.keys.values.all { it.id in device.buttons })
        assertEquals(PalmButton.HOME, map.keys[android.view.KeyEvent.KEYCODE_BUTTON_START])
    }

    @Test fun txMenuFollowsTheStatusBar() {
        assertEquals(54 to 466, PalmButtons.tapPoint(Device.TX, PalmButton.MENU, PalmButtons.PORTRAIT))
        assertEquals(266 to 14, PalmButtons.tapPoint(Device.TX, PalmButton.MENU, PalmButtons.REVERSE_LANDSCAPE))
        assertNull(PalmButtons.tapPoint(Device.TX, PalmButton.MENU, "landscape"))
        assertNull(PalmButtons.tapPoint(Device.TX, PalmButton.HOME, PalmButtons.PORTRAIT))
    }

    @Test fun e3UsesItsGuestStatusBarWithoutAnExtraSilkscreen() {
        val e3 = Device.byId("palm-e2-franken")!!
        assertEquals(0, e3.silkscreenHeight)
        val oldPowerMapping = "97:power,96:centre"
        assertEquals(mapOf(96 to PalmButton.CENTRE), ControllerMap.decode(oldPowerMapping, e3).keys)
        org.junit.Assert.assertFalse(e3.rotatingStatusBar)
        for (orientation in listOf(PalmButtons.PORTRAIT, PalmButtons.REVERSE_LANDSCAPE)) {
            assertEquals(20 to 466, PalmButtons.tapPoint(e3, PalmButton.HOME, orientation))
            assertEquals(88 to 466, PalmButtons.tapPoint(e3, PalmButton.MENU, orientation))
        }
        assertEquals(0, PalmButtons.sdlKey(PalmButton.HOME, e3))
        assertEquals(PalmButtons.sdlKey(PalmButton.APP1, Device.E2), PalmButtons.sdlKey(PalmButton.APP1, e3))
    }

    @Test fun e2SilkscreenDoesNotMove() {
        assertEquals(27 to 405, PalmButtons.tapPoint(Device.E2, PalmButton.MENU, PalmButtons.REVERSE_LANDSCAPE))
        assertEquals(27 to 351, PalmButtons.tapPoint(Device.E2, PalmButton.HOME, PalmButtons.PORTRAIT))
    }

    // The middle of the ROM's Home and Menu areas, inside the 120 rows the digitizer reaches.
    @Test fun zireSilkscreenHomeAndMenu() {
        assertEquals(23 to 352, PalmButtons.tapPoint(Device.ZIRE72, PalmButton.HOME, PalmButtons.PORTRAIT))
        assertEquals(23 to 412, PalmButtons.tapPoint(Device.ZIRE72, PalmButton.MENU, PalmButtons.REVERSE_LANDSCAPE))
        assertNull(PalmButtons.tapPoint(Device.ZIRE72, PalmButton.VOICE, PalmButtons.PORTRAIT))
    }
}
