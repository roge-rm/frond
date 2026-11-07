package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class InstallBatchTest {
    @Test fun installsInOrderAndReportsProgress() {
        val seen = mutableListOf<String>()
        val steps = mutableListOf<Pair<Int, Int>>()
        val r = InstallBatch.run(listOf("b", "a", "c"), { it }, { n, t -> steps += n to t }) { seen += it }
        assertEquals(listOf("b", "a", "c"), seen)
        assertEquals(listOf(1 to 3, 2 to 3, 3 to 3), steps)
        assertNull(r.summary())
    }

    @Test fun keepsGoingAfterAFailureAndNamesTheFailedFiles() {
        val seen = mutableListOf<String>()
        val r = InstallBatch.run(listOf("one.prc", "bad.prc", "two.prc", "worse.pdb"), { it }, { _, _ -> }) {
            seen += it
            if (it.startsWith("bad")) throw PalmDatabases.Failed("Not a Palm database.")
            if (it.startsWith("worse")) throw java.io.IOException()
        }
        assertEquals(listOf("one.prc", "bad.prc", "two.prc", "worse.pdb"), seen)
        assertEquals(
            "2 of 4 installed.\n\nbad.prc: Not a Palm database.\nworse.pdb: Couldn't read or write the file.",
            r.summary(),
        )
    }

    @Test fun oneFileFailsWithJustTheReason() {
        val r = InstallBatch.run(listOf("a.prc"), { it }, { _, _ -> }) { throw PalmDatabases.Failed("No room.") }
        assertEquals("No room.", r.summary())
    }

    @Test fun otherExceptionsDoNotShowTheirMessage() {
        val r = InstallBatch.run(listOf("a.prc"), { it }, { _, _ -> }) { throw java.io.IOException("/data/secret/path") }
        assertEquals("Couldn't read or write the file.", r.summary())
    }
}
