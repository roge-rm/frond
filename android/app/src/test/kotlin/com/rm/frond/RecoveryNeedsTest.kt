package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RecoveryNeedsTest {
    @Test fun savedSessionNeedsItsSizeWithSlack() {
        val size = 43L shl 20
        val needs = Sessions.recoveryNeeds(size, Device.TX)
        assertTrue(needs > size)
        assertEquals(size + size / 4 + (16L shl 20), needs)
    }

    @Test fun unsavedSessionNeedsItsFirmware() {
        assertEquals(Device.E2.norSize + Device.E2.nandSize + (64L shl 20), Sessions.recoveryNeeds(null, Device.E2))
        assertEquals(Sessions.recoveryNeeds(null, Device.TX), Sessions.recoveryNeeds(0, Device.TX))
    }
}
