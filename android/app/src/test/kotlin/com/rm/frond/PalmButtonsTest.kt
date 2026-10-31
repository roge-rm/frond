package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PalmButtonsTest {
    @Test fun txMenuFollowsTheStatusBar() {
        assertEquals(54 to 466, PalmButtons.tapPoint(Device.TX, PalmButton.MENU, PalmButtons.PORTRAIT))
        assertEquals(266 to 14, PalmButtons.tapPoint(Device.TX, PalmButton.MENU, PalmButtons.REVERSE_LANDSCAPE))
        assertNull(PalmButtons.tapPoint(Device.TX, PalmButton.MENU, "landscape"))
        assertNull(PalmButtons.tapPoint(Device.TX, PalmButton.HOME, PalmButtons.PORTRAIT))
    }

    @Test fun e2SilkscreenDoesNotMove() {
        assertEquals(27 to 405, PalmButtons.tapPoint(Device.E2, PalmButton.MENU, PalmButtons.REVERSE_LANDSCAPE))
        assertEquals(27 to 351, PalmButtons.tapPoint(Device.E2, PalmButton.HOME, PalmButtons.PORTRAIT))
    }
}
