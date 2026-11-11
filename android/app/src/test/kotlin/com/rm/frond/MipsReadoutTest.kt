package com.rm.frond

import org.junit.Test
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull

class MipsReadoutTest {
    @Test fun parsesLayoutAndRejectsBadText() {
        assertEquals(MipsReadout.Layout(0, 120, 1080, 1440, 1080, 1800), MipsReadout.parseLayout("0 120 1080 1440 1080 1800"))
        assertNull(MipsReadout.parseLayout("0 120 1080"))
        assertNull(MipsReadout.parseLayout("0 0 0 10 10 10"))
        assertNull(MipsReadout.parseLayout("a b c d e f"))
    }

    @Test fun parsesMips() {
        assertEquals(148L, MipsReadout.parseMips("148.00 MIPS, limit 200.00 MIPS, 74%, 0 underruns"))
        assertEquals(null, MipsReadout.parseMips("unknown"))
    }

    @Test fun bottomRightOfTheFittedDeviceNotTheView() {
        // A 1080x1800 view whose Palm fills 1080x1440 from y=120: the black bar below it stays clear.
        val layout = MipsReadout.parseLayout("0 120 1080 1440 1080 1800")!!
        assertEquals(Pair(8, 240 + 8), MipsReadout.margins(layout, 0, 0, 1080, 1800, 1080, 1800, 8))
    }

    @Test fun scalesWindowUnitsAndSurfaceOffset() {
        // The core's window is 540x900 shown on a 1080x1800 surface that starts 100 px down a 1080x1900 parent.
        val layout = MipsReadout.parseLayout("45 0 450 800 540 900")!!
        assertEquals(Pair(90 + 4, 200 + 4), MipsReadout.margins(layout, 0, 100, 1080, 1800, 1080, 1900, 4))
    }
}
