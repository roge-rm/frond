package com.rm.frond

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

// What Home does when asked to start a session while an emulator process may still be up.
// A process can outlive its activity: SDL's onDestroy waits for the core to quit, and the core
// saves before it does, so a finished activity's process can stay listed for a few seconds, or
// for good if the core never quits. Home then waits for it, ends it if it stays, and starts.
object EmulatorStart {
    enum class Action { START, BRING_FORWARD, WAIT, END_STALE, GIVE_UP }

    // How long a process with no activity gets to end by itself. Android gives a finishing
    // activity 10 s to be destroyed before it stops waiting for it.
    const val GRACE_MS = 10_000L
    // How long an ended process gets to leave the process list.
    const val END_WAIT_MS = 3_000L
    const val POLL_MS = 250L

    // processListed: an :emulator or :emulator68k process is in the process list.
    // activityShown: one of the app's tasks has an emulator activity on top.
    // waitedMs: time since the start was asked for. ended: END_STALE has already been done.
    fun decide(processListed: Boolean, activityShown: Boolean, waitedMs: Long, ended: Boolean): Action = when {
        activityShown -> Action.BRING_FORWARD
        !processListed -> Action.START
        !ended && waitedMs < GRACE_MS -> Action.WAIT
        !ended -> Action.END_STALE
        waitedMs < GRACE_MS + END_WAIT_MS -> Action.WAIT
        else -> Action.GIVE_UP
    }
}

// One pending start at a time. A tap on another session replaces the one waiting; a repeat tap
// on the same session leaves it alone.
class LatestStart(private val scope: CoroutineScope) {
    private var job: Job? = null
    private var id: String? = null

    fun start(sessionId: String, block: suspend CoroutineScope.() -> Unit) {
        if (job?.isActive == true && id == sessionId) return
        job?.cancel()
        id = sessionId
        job = scope.launch(block = block)
    }

    fun cancel() {
        job?.cancel()
        job = null
        id = null
    }
}
