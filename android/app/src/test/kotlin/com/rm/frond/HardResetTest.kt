package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class HardResetTest {
    private fun run(path: String?, saveFirst: Boolean, saveOk: Boolean = true): Pair<String?, List<String>> {
        val sent = mutableListOf<String>()
        val error = PeripheralsModel.runHardReset(path, saveFirst) { command, argument ->
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
}
