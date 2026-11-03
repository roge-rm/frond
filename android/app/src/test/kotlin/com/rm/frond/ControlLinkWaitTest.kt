package com.rm.frond

import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeoutException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ControlLinkWaitTest {
    @Test
    fun progressRestartsTheWait() {
        val reply = CompletableFuture<String>()
        val last = java.util.concurrent.atomic.AtomicLong(0L)
        val worker = Thread {
            repeat(4) { Thread.sleep(150); last.set(System.nanoTime()) }
            reply.complete("done")
        }
        worker.start()
        // 600 ms of work against a 300 ms timeout that restarts at each event.
        assertEquals("done", ControlLink.awaitReply(reply, 300_000_000L, { last.get() }))
    }

    @Test
    fun silenceTimesOut() {
        val reply = CompletableFuture<String>()
        val started = System.nanoTime()
        try {
            ControlLink.awaitReply(reply, 200_000_000L, { 0L })
            assertTrue(false)
        } catch (_: TimeoutException) {
        }
        assertTrue(System.nanoTime() - started >= 200_000_000L)
    }

    @Test
    fun memoryStickJobsCanBeCancelled() {
        assertTrue(ControlLink.FILE_COMMANDS.containsAll(listOf("ms-create", "ms-insert", "ms-export")))
    }
}
