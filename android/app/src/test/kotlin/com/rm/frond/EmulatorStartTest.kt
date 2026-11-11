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

class LatestStartTest {
    private fun scope() = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.Unconfined)

    @Test fun latestTapReplacesTheWaitingOne() {
        val starts = LatestStart(scope())
        val ran = mutableListOf<String>()
        val first = kotlinx.coroutines.CompletableDeferred<Unit>()
        starts.start("a") { try { first.await(); ran += "a" } catch (e: kotlinx.coroutines.CancellationException) { ran += "a cancelled"; throw e } }
        starts.start("b") { ran += "b" }
        assertEquals(listOf("a cancelled", "b"), ran)
    }

    @Test fun repeatTapOnTheSameSessionKeepsTheWait() {
        val starts = LatestStart(scope())
        var runs = 0
        val gate = kotlinx.coroutines.CompletableDeferred<Unit>()
        starts.start("a") { runs++; gate.await() }
        starts.start("a") { runs++ }
        assertEquals(1, runs)
    }

    @Test fun cancelDropsThePendingStart() {
        val starts = LatestStart(scope())
        var done = false
        val gate = kotlinx.coroutines.CompletableDeferred<Unit>()
        starts.start("a") { gate.await(); done = true }
        starts.cancel()
        gate.complete(Unit)
        assertEquals(false, done)
    }
}
