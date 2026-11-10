package com.rm.frond

import com.rm.frond.LinkAttach.Step
import org.junit.Assert.assertEquals
import org.junit.Test

class LinkAttachTest {
    private fun choose(hostedHere: Boolean = false, reached: Boolean = false, reachedOurs: Boolean = false,
                       preferHost: Boolean = true, holding: Boolean = false) =
        LinkAttach.choose(hostedHere, reached, reachedOurs, preferHost, holding)

    @Test fun secondLinkKindInTheEmulatorJoinsItsEngine() {
        // Infrared hosts the engine here; serial or Bluetooth turned on later shares it and never yields it.
        assertEquals(Step.HOST_HERE, choose(hostedHere = true, reached = true, reachedOurs = true))
        assertEquals(Step.HOST_HERE, choose(hostedHere = true))
    }

    @Test fun ourOwnStoppingEngineIsWaitedOut() {
        assertEquals(Step.WAIT, choose(reached = true, reachedOurs = true))
        assertEquals(Step.WAIT, choose(reached = true, reachedOurs = true, preferHost = false))
    }

    @Test fun theEmulatorTakesOverFromTheHomeProcess() {
        assertEquals(Step.YIELD_THEN_HOST, choose(reached = true))
    }

    @Test fun theHomeProcessJoinsTheEmulatorsEngine() {
        assertEquals(Step.JOIN_REMOTE, choose(reached = true, preferHost = false))
        assertEquals(Step.JOIN_REMOTE, choose(reached = true, preferHost = false, holding = true))
    }

    @Test fun nobodyHostsSoHostHere() {
        assertEquals(Step.HOST_HERE, choose())
        assertEquals(Step.HOST_HERE, choose(holding = true))
        assertEquals(Step.HOST_HERE, choose(preferHost = false))
        // The home process just yielded: it leaves the emulator time to start.
        assertEquals(Step.WAIT, choose(preferHost = false, holding = true))
    }
}
