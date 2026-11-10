package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

class HardResetTest {
    private fun run(path: String?, saveFirst: Boolean, saveOk: Boolean = true, onSaved: () -> Unit = {}): Pair<String?, List<String>> {
        val sent = mutableListOf<String>()
        val error = RunningModel.runHardReset(path, saveFirst, onSaved) { command, argument ->
            sent += if (argument.isEmpty()) command else "$command $argument"
            if (command == "save" && !saveOk) ControlLink.Reply(false, "write", "Disk full")
            else ControlLink.Reply(true, "done", "Done.")
        }
        return error to sent
    }

    @Test fun eraseSendsOnlyHardReset() {
        val (error, sent) = run("/s/tx.frond", saveFirst = false)
        assertNull(error)
        assertEquals(listOf("hard-reset"), sent)
    }

    @Test fun saveFirstSavesTheSessionThenErases() {
        val (error, sent) = run("/s/tx.frond", saveFirst = true)
        assertNull(error)
        assertEquals(listOf("save /s/tx.frond", "hard-reset"), sent)
    }

    @Test fun aFailedSaveErasesNothing() {
        val (error, sent) = run("/s/tx.frond", saveFirst = true, saveOk = false)
        assertEquals("Disk full", error)
        assertEquals(listOf("save /s/tx.frond"), sent)
    }

    @Test fun saveFirstWithoutASessionErasesNothing() {
        val (error, sent) = run(null, saveFirst = true)
        assertEquals("There's no session to save.", error)
        assertEquals(emptyList<String>(), sent)
    }

    // The save before a hard reset clears the slot's swap, as any good save does; a failed one keeps it.
    @Test fun saveFirstClearsTheSlotSwap() {
        val dir = Files.createTempDirectory("hard-reset").toFile()
        try {
            Cards.writeSlot(dir, Cards.Slot(true, null, null, "tx", true))
            run("/s/tx.frond", saveFirst = true, saveOk = false) { Cards.clearSwap(dir) }
            assertTrue(Cards.readSlot(dir).swap)
            run("/s/tx.frond", saveFirst = true) { Cards.clearSwap(dir) }
            assertFalse(Cards.readSlot(dir).swap)
        } finally {
            dir.deleteRecursively()
        }
    }
}
