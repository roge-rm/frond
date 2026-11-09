package com.rm.frond

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException

class PrintStoreTest {
    @get:Rule val tmp = TemporaryFolder()

    private class Core(var data: String = "hello", var clearOk: Boolean = true, var saveOk: Boolean = true, var attachOk: Boolean = true) {
        val log = mutableListOf<String>()
        fun reply(line: String): ControlLink.Reply {
            val command = line.substringBefore(' ')
            val argument = line.substringAfter(' ', "")
            log += command
            return when (command) {
                "status" -> ControlLink.Reply(true, "ok", "attached idle bytes=${data.length} last=${if (data.isEmpty()) "none" else "received"} idle=4000".let {
                    if (data.isEmpty()) "attached collecting bytes=0 last=none idle=-1" else "attached collecting bytes=${data.length} last=none idle=4000"
                })
                "save" -> if (saveOk) { File(argument).writeText(data); data = ""; ControlLink.Reply(true, "ok", "saved") } else ControlLink.Reply(false, "timeout", "timed out")
                "clear" -> if (clearOk) { data = ""; ControlLink.Reply(true, "ok", "") } else ControlLink.Reply(false, "error", "no")
                "attach" -> if (attachOk) ControlLink.Reply(true, "ok", "") else ControlLink.Reply(false, "error", "no printer")
                else -> ControlLink.Reply(false, "error", "?")
            }
        }
    }

    private fun store(core: Core) = PrintStore(tmp.newFolder()) { core.reply(it) }
    private val none: (Long, Int) -> Unit = { _, _ -> }

    @Test fun pollAppendsJobsToTheHeldFile() = runBlocking {
        val core = Core("abc"); val s = store(core)
        assertTrue(s.poll(none) is PrintStore.Poll.Added)
        core.data = "def"
        val again = s.poll(none) as PrintStore.Poll.Added
        assertEquals(6L, again.size)
        assertEquals("abcdef", String(s.heldBytes()))
        assertEquals(2, again.version)
    }

    @Test fun failedSaveKeepsNothingAndReports() = runBlocking {
        val core = Core("abc", saveOk = false); val s = store(core)
        assertTrue(s.poll(none) is PrintStore.Poll.Failed)
        assertEquals(0L, s.heldSize())
    }

    @Test fun discardThenPollMakesNoEmptyJob() = runBlocking {
        val core = Core("abc"); val s = store(core)
        s.poll(none)
        assertNull(s.discard(none))
        assertEquals(0L, s.heldSize())
        val shown = mutableListOf<Long>()
        assertFalse(s.poll { size, _ -> shown += size } is PrintStore.Poll.Added)
        assertTrue(shown.isEmpty())
    }

    @Test fun failedDiscardKeepsTheJob() = runBlocking {
        val core = Core("abc", clearOk = false); val s = store(core)
        s.poll(none)
        assertEquals("no", s.discard(none))
        assertEquals(3L, s.heldSize())
    }

    @Test fun saveRawDropsOnlyOnSuccess() = runBlocking {
        val core = Core("abc"); val s = store(core)
        s.poll(none)
        assertEquals("disk full", s.saveRaw(none) { throw IOException("disk full") })
        assertEquals(3L, s.heldSize())
        val out = tmp.newFile()
        assertNull(s.saveRaw(none) { it.copyTo(out, true) })
        assertEquals("abc", out.readText())
        assertEquals(0L, s.heldSize())
    }

    @Test fun appendDuringSaveRawIsKept() = runBlocking {
        val core = Core("abc"); val s = store(core)
        s.poll(none)
        var added: PrintStore.Poll? = null
        val t = Thread {
            core.data = "xyz"
            added = runBlocking { s.poll(none) }
        }
        assertNull(s.saveRaw(none) {
            t.start(); Thread.sleep(200)
            assertEquals("abc", it.readText())
        })
        t.join()
        assertTrue(added is PrintStore.Poll.Added)
        assertEquals("xyz", String(s.heldBytes()))
    }

    @Test fun printDropsOnlyThePrintedVersion() = runBlocking {
        val core = Core("abc"); val s = store(core)
        s.poll(none)
        core.data = "d"; s.poll(none)
        assertFalse(s.dropIfVersion(1, none))
        assertEquals(4L, s.heldSize())
        assertTrue(s.dropIfVersion(2, none))
        assertEquals(0L, s.heldSize())
    }

    @Test fun timedOutSaveDoesNotDeleteALateFile() = runBlocking {
        val core = Core("abc", saveOk = false); val dir = tmp.newFolder(); val s = PrintStore(dir) { core.reply(it) }
        s.poll(none)
        val late = File(dir, "job-late.raw").apply { writeText("late") }
        s.poll(none)
        assertTrue(late.exists())
        s.sweep()
        assertFalse(late.exists())
    }

    @Test fun attachReportsFailure() {
        assertNull(store(Core()).attach(true))
        assertEquals("no printer", store(Core(attachOk = false)).attach(true))
    }
}
