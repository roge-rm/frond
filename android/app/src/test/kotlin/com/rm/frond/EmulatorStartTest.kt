package com.rm.frond

import com.rm.frond.EmulatorStart.Action
import com.rm.frond.EmulatorStart.END_WAIT_MS
import com.rm.frond.EmulatorStart.GRACE_MS
import com.rm.frond.EmulatorStart.decide
import org.junit.Assert.assertEquals
import org.junit.Test

class EmulatorStartTest {
    @Test fun startsWithNoEmulatorProcess() {
        assertEquals(Action.START, decide(processListed = false, activityShown = false, waitedMs = 0, ended = false))
    }

    @Test fun bringsAShownEmulatorForward() {
        assertEquals(Action.BRING_FORWARD, decide(processListed = true, activityShown = true, waitedMs = 0, ended = false))
    }

    @Test fun waitsForAClosingProcessThenEndsIt() {
        assertEquals(Action.WAIT, decide(processListed = true, activityShown = false, waitedMs = 0, ended = false))
        assertEquals(Action.WAIT, decide(processListed = true, activityShown = false, waitedMs = GRACE_MS - 1, ended = false))
        assertEquals(Action.END_STALE, decide(processListed = true, activityShown = false, waitedMs = GRACE_MS, ended = false))
    }

    @Test fun startsOnceTheProcessHasGone() {
        assertEquals(Action.START, decide(processListed = false, activityShown = false, waitedMs = 2_000, ended = false))
        assertEquals(Action.START, decide(processListed = false, activityShown = false, waitedMs = GRACE_MS + 100, ended = true))
    }

    @Test fun givesUpWhenAnEndedProcessStays() {
        assertEquals(Action.WAIT, decide(processListed = true, activityShown = false, waitedMs = GRACE_MS + END_WAIT_MS - 1, ended = true))
        assertEquals(Action.GIVE_UP, decide(processListed = true, activityShown = false, waitedMs = GRACE_MS + END_WAIT_MS, ended = true))
    }
}
