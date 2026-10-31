package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PalmDatabasesTest {
    private fun hex(text: String) = text.toByteArray(Charsets.ISO_8859_1).joinToString("") { "%02x".format(it.toInt() and 0xff) }

    @Test fun readsTheCoreList() {
        val text = "6170706c 48454c4f 0001 ${hex("zeta")}\n" +
            "44415441 6d656d6f 0008 ${hex("Alpha")}\n" +
            "70616e6c 616c726d 0009 ${hex("Caf\u00e9")}\n"
        val list = PalmDatabases.parseList(text)
        assertEquals(listOf("Alpha", "Caf\u00e9", "zeta"), list.map { it.name })
        assertEquals("appl", list[2].type)
        assertEquals("HELO", list[2].creator)
        assertTrue(list[2].application)
        assertFalse(list[0].resource)
        // A resource database that isn't an application.
        assertTrue(list[1].resource)
        assertFalse(list[1].application)
    }

    @Test fun skipsLinesThatDontParse() {
        val text = "\n6170706c 48454c4f 0001\nzz70706c 48454c4f 0001 41\n6170706c 48454c4f 0001 4\n" +
            "6170706c 48454c4f 0001 ${hex("A".repeat(31))}"
        assertEquals(listOf("A".repeat(31)), PalmDatabases.parseList(text).map { it.name })
        assertTrue(PalmDatabases.parseList("").isEmpty())
    }

    @Test fun notReadyGetsAPlainMessage() {
        val text = PalmDatabases.message(ControlLink.Reply(false, "not-ready", "Wait for Palm OS to reach a supported application boundary."))
        assertTrue(text.startsWith("Palm OS isn't ready."))
        assertEquals("Could not launch.", PalmDatabases.message(ControlLink.Reply(false, "launch", "Could not launch.")))
    }

    @Test fun launchFailureNamesTheDatabaseName() {
        val text = PalmDatabases.launchFailed("Memos")
        assertTrue(text.contains("\"Memos\""))
        assertTrue(text.contains("Memos-PMem"))
    }
}
