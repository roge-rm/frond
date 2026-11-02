package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.CRC32

class SessionsLayoutTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test fun packableNamesIgnoreTempsAndGenerations() {
        assertTrue(packableName("session.frond"))
        assertTrue(packableName("Palm TX.frond"))
        assertTrue(packableName("Palm TX.frond.settings.json"))
        assertTrue(packableName("sd.frondsd"))
        assertTrue(packableName("ms.frondms"))
        assertFalse(packableName("session.frond.new"))
        assertFalse(packableName(".frond.tmp-abc"))
        assertFalse(packableName("session.frond.tmp-abc"))
        assertFalse(packableName(".gen-sd.frondsd-1"))
        assertFalse(packableName("id.json"))
        assertFalse(packableName("nested/sd.frondsd"))
    }

    @Test fun zipRoundTripCopiesPackableFilesAtArchiveRoot() {
        val dir = tmp.newFolder("machine")
        File(dir, "session.frond").writeText("guest")
        File(dir, "sd.frondsd").writeText("card")
        File(dir, ".frond.tmp-leftover").writeText("tmp")
        File(dir, ".gen-sd.frondsd-1").writeText("gen")
        val zip = ByteArrayOutputStream()
        zipPackable(dir, zip)
        val dest = tmp.newFolder("unpacked")
        assertTrue(unzipPackable(ByteArrayInputStream(zip.toByteArray()), dest))
        assertEquals("guest", File(dest, "session.frond").readText())
        assertEquals("card", File(dest, "sd.frondsd").readText())
        assertFalse(File(dest, ".frond.tmp-leftover").exists())
        assertFalse(File(dest, ".gen-sd.frondsd-1").exists())
        assertEquals(File(dir, "session.frond").length() + File(dir, "sd.frondsd").length(), packableSize(dir))
        assertEquals(File(dir, "session.frond"), sessionFile(dir))
    }

    @Test fun sessionFilePrefersFolderNameThenSessionFrond() {
        val dir = tmp.newFolder("Trip")
        File(dir, "other.frond").writeText("a")
        File(dir, "session.frond").writeText("b")
        assertEquals(File(dir, "session.frond"), sessionFile(dir))
        File(dir, "Trip.frond").writeText("c")
        assertEquals(File(dir, "Trip.frond"), sessionFile(dir))
    }

    private fun put32(n: Long) = byteArrayOf(
        (n and 0xff).toByte(), ((n shr 8) and 0xff).toByte(),
        ((n shr 16) and 0xff).toByte(), ((n shr 24) and 0xff).toByte(),
    )
    private fun put64(n: Long) = put32(n) + put32(n ushr 32)
    private fun v7WithSd(): ByteArray {
        val name = "palm-tx".toByteArray(Charsets.US_ASCII).copyOf(32)
        val sdName = "sd.frondsd".toByteArray(Charsets.US_ASCII).copyOf(32)
        return put32(0x19800819) + put32(7) + name +
            put32(0) + put32(0) + ByteArray(40) + put32(1) +
            put32(1) + sdName + put32(1) + put32(0) + ByteArray(32) + put64(512) + put64(512)
    }
    private fun tinyFrondsd(): ByteArray {
        val payload = ByteArray(512) { 0xa5.toByte() }
        val crc = CRC32().also { it.update(payload) }.value
        return "FRONDSD".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + put32(1) + put32(512) +
            put64(512) + put32(crc) + ByteArray(36) + payload
    }

    @Test fun v7MissingSiblingIsRefused() {
        val dir = tmp.newFolder("machine")
        val frond = File(dir, "session.frond").also { it.writeBytes(v7WithSd()) }
        assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsd())
        assertNull(checkImportedSiblings(dir, frond))
    }

    @Test fun v7StubWithoutMediaImports() {
        val dir = tmp.newFolder("stub")
        val header = put32(0x19800819) + put32(7) + "palm-tx".toByteArray(Charsets.US_ASCII).copyOf(32) +
            "body".toByteArray()
        val frond = File(dir, "session.frond").also { it.writeBytes(header) }
        assertNull(checkImportedSiblings(dir, frond))
    }
}
