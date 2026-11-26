package com.rm.frond

import android.view.InputDevice
import org.junit.Assert.assertEquals
import org.junit.Test

class UnbufferedInputTest {
    @Test
    fun touchesAndSticksAreUnbufferedFromAndroid11() {
        assertEquals(InputDevice.SOURCE_CLASS_POINTER or InputDevice.SOURCE_CLASS_JOYSTICK, UnbufferedInput.sources(30))
        assertEquals(InputDevice.SOURCE_CLASS_POINTER or InputDevice.SOURCE_CLASS_JOYSTICK, UnbufferedInput.sources(36))
        // Older versions ask a touch at a time in dispatchTouchEvent; sticks stay batched there.
        assertEquals(0, UnbufferedInput.sources(29))
    }
}
