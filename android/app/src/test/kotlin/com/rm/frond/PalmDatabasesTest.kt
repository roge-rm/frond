package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class PalmDatabasesTest {
    private fun database(name: String, type: String, creator: String, resource: Boolean, size: Int = 96): ByteArray {
        val bytes = ByteArray(size)
        name.toByteArray(Charsets.ISO_8859_1).copyInto(bytes)
        if (resource) bytes[33] = 1
        bytes[32] = 0x08 // backup bit, ignored
        type.toByteArray(Charsets.ISO_8859_1).copyInto(bytes, 60)
        creator.toByteArray(Charsets.ISO_8859_1).copyInto(bytes, 64)
        return bytes
    }

    @Test fun readsHeader() {
        val info = PalmDatabases.header(database("Hello World", "appl", "HELO", true))!!
        assertEquals("Hello World", info.name)
        assertEquals("appl", info.type)
        assertEquals("HELO", info.creator)
        assertTrue(info.resource)
        assertTrue(info.application)
    }

    @Test fun recordDatabaseIsNotAnApplication() {
        val info = PalmDatabases.header(database("MemoDB", "DATA", "memo", false))!!
        assertFalse(info.resource)
        assertFalse(info.application)
    }

    @Test fun fullLengthNameWithoutTerminator() {
        val name = "A".repeat(32)
        val bytes = database(name, "appl", "TEST", true)
        assertEquals(name, PalmDatabases.header(bytes)!!.name)
    }

    @Test fun refusesShortOrNamelessHeaders() {
        assertNull(PalmDatabases.header(ByteArray(PalmDatabases.HEADER_BYTES - 1)))
        assertNull(PalmDatabases.header(ByteArray(PalmDatabases.HEADER_BYTES)))
    }

    @Test fun listsArchiveSortedAndSkipsJunk() {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            fun add(entry: String, bytes: ByteArray) {
                zip.putNextEntry(ZipEntry(entry))
                zip.write(bytes)
                zip.closeEntry()
            }
            add("zeta.prc", database("zeta", "appl", "ZETA", true, 4000))
            add("Alpha.pdb", database("Alpha", "DATA", "ALPH", false))
            add("short.bin", ByteArray(10))
        }
        val list = PalmDatabases.readArchive(ByteArrayInputStream(out.toByteArray()))
        assertEquals(listOf("Alpha", "zeta"), list.map { it.name })
        assertTrue(list[1].application)
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
