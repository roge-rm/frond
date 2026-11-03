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

    @Test fun staleCardTempsAreRemoved() {
        val root = tmp.newFolder("sessions")
        val dir = File(root, "id").apply { mkdirs() }
        val now = System.currentTimeMillis()
        val old = listOf(".frond.tmp-a", ".frondsd.tmp-b", ".frondms.tmp-c", ".gen-sd.frondsd-tmp-1-2")
            .map { File(dir, it).apply { writeText("tmp"); setLastModified(now - 3_600_000) } }
        val fresh = File(dir, ".frondsd.tmp-fresh").apply { writeText("tmp") }
        val card = File(dir, "sd.frondsd").apply { writeText("card"); setLastModified(now - 3_600_000) }
        val gen = File(dir, ".gen-sd.frondsd-root").apply { writeText("root"); setLastModified(now - 3_600_000) }
        // An SD exchange journal waits to be replayed.
        val journal = File(dir, "card.img.journal").apply { writeText("journal"); setLastModified(now - 3_600_000) }
        // A running machine's candidate card and its generation files stay however old they are.
        val live = listOf(".frondsd.tmp-live-Ab12Cd", ".gen-.frondsd.tmp-live-Ab12Cd-tmp-1-2")
            .map { File(dir, it).apply { writeText("live"); setLastModified(now - 3_600_000) } }
        removeStaleTemps(root, now)
        old.forEach { assertFalse(it.name, it.exists()) }
        assertTrue(fresh.exists() && card.exists() && gen.exists() && journal.exists())
        live.forEach { assertTrue(it.name, it.exists()) }
    }

    @Test fun staleImportsAreRemoved() {
        val root = tmp.newFolder("sessions")
        val now = System.currentTimeMillis()
        fun import(id: String, age: Long, json: Boolean): File {
            val dir = File(root, id).apply { mkdirs() }
            val part = File(dir, ".import.part").apply { mkdirs() }
            File(part, "session.frond").writeText("guest")
            if (json) File(dir, "$id.json").writeText("{}")
            dir.walkTopDown().forEach { it.setLastModified(now - age) }
            return dir
        }
        val killed = import("killed", 3_600_000, json = false)
        val running = import("running", 0, json = false)
        val listed = import("listed", 3_600_000, json = true)
        val stalled = import("stalled", 3_600_000, json = false)
        activeImports.add("stalled")
        try {
            removeStaleTemps(root, now)
        } finally {
            activeImports.remove("stalled")
        }
        assertFalse(killed.exists())
        assertTrue(running.exists() && listed.exists() && stalled.exists())
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

    // v8: 100-byte descriptors ending with the SD content generation.
    private fun v8WithSd(generation: Long, sections: ByteArray = ByteArray(0), metadata: Long = 0, nor: Long = 0): ByteArray {
        val name = "palm-tx".toByteArray(Charsets.US_ASCII).copyOf(32)
        val sdName = "sd.frondsd".toByteArray(Charsets.US_ASCII).copyOf(32)
        return put32(0x19800819) + put32(8) + name +
            put32(0) + put32(0) + put64(metadata) + put64(nor) + ByteArray(24) + put32(1) +
            put32(1) + sdName + put32(1) + put32(0) + ByteArray(32) + put64(512) + put64(512) +
            put64(generation) + sections
    }
    private fun tinyFrondsdV2(generation: Long, payload: ByteArray = ByteArray(512) { 0xa5.toByte() }): ByteArray {
        val table = put32(CRC32().also { it.update(payload) }.value)
        val header = "FRONDSD".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + put32(2) + put32(512) +
            put64(512) + put32(0) + put64(generation) + put64(64 + 512) + put32(1L shl 20) + ByteArray(16)
        val crc = CRC32().also { it.update(header); it.update(table) }.value
        return header.copyOfRange(0, 24) + put32(crc) + header.copyOfRange(28, 64) + payload + table
    }

    // v8 with one Memory Stick descriptor of mb MB.
    private fun v8WithMs(mb: Int, logical: Long = mb.toLong() shl 20): ByteArray {
        val name = "sony-clie-tg50".toByteArray(Charsets.US_ASCII).copyOf(32)
        val msName = "ms.frondms".toByteArray(Charsets.US_ASCII).copyOf(32)
        return put32(0x19800819) + put32(8) + name + put32(0) + put32(0) + put64(0) + put64(0) + ByteArray(24) +
            put32(1) + put32(2) + msName + put32(1) + put32(0) + ByteArray(32) + put64(logical) +
            put64(Device.memoryStickPhysicalBytes(mb)) + put64(0)
    }
    private fun frondms(blocks: Long, pages: Long, physical: Long) =
        "FRONDMS".toByteArray(Charsets.US_ASCII) + byteArrayOf(0) + put32(1) + put32(blocks) + put32(pages) +
            put32(512) + put32(9) + ByteArray(36) + ByteArray(physical.toInt())

    @Test fun memoryStickSizes() {
        val dir = tmp.newFolder("ms")
        val frond = File(dir, "session.frond")
        for ((mb, blocks, pages) in listOf(Triple(4, 512L, 16L), Triple(8, 1024L, 16L), Triple(16, 1024L, 32L), Triple(64, 4096L, 32L))) {
            frond.writeBytes(v8WithMs(mb))
            File(dir, "ms.frondms").writeBytes(frondms(blocks, pages, Device.memoryStickPhysicalBytes(mb)))
            assertNull(checkImportedSiblings(dir, frond))
            // Same size, other shape; then a descriptor whose logical size is another stick's.
            val (otherBlocks, otherPages) = if (pages == 16L) blocks / 2 to 32L else blocks * 2 to 16L
            File(dir, "ms.frondms").writeBytes(frondms(otherBlocks, otherPages, Device.memoryStickPhysicalBytes(mb)))
            assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
            File(dir, "ms.frondms").writeBytes(frondms(blocks, pages, Device.memoryStickPhysicalBytes(mb)))
            frond.writeBytes(v8WithMs(mb, 128L shl 20))
            assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        }
    }

    @Test fun v8SiblingNeedsMatchingGeneration() {
        val dir = tmp.newFolder("v8")
        val frond = File(dir, "session.frond").also { it.writeBytes(v8WithSd(3)) }
        File(dir, "sd.frondsd").writeBytes(tinyFrondsdV2(3))
        assertNull(checkImportedSiblings(dir, frond))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsdV2(2))
        assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsdV2(3).also { it[64 + 7] = 1 })
        assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsd())
        assertEquals("That session's card files don't match.", checkImportedSiblings(dir, frond))
        assertFalse(packableName("sd.frondsd.journal-4"))
        assertTrue(ignoredName("sd.frondsd.journal-4"))
        assertFalse(saveTempName("sd.frondsd.journal-4"))
    }

    // A journal from `from` to from + 1 over a 512-byte card: one record, then the new table.
    private fun journalTo(from: Long, offset: Long, data: ByteArray, newPayload: ByteArray): ByteArray {
        val body = "FRONDJNL".toByteArray(Charsets.US_ASCII) + put32(1) + put32(0) + put64(512) + put64(from) +
            put64(from + 1) + put64(1) + put64(1) + ByteArray(8) + put64(offset) + put32(data.size.toLong()) + data +
            put32(CRC32().also { it.update(newPayload) }.value)
        return body + "FRONDEND".toByteArray(Charsets.US_ASCII) + put32(CRC32().also { it.update(body) }.value) + put32(0)
    }

    private fun unpack(dir: File, name: String): File {
        val zip = ByteArrayOutputStream()
        zipPackable(dir, zip)
        val dest = tmp.newFolder(name)
        assertTrue(unzipPackable(ByteArrayInputStream(zip.toByteArray()), dest))
        return dest
    }

    // A save the core didn't finish: the export gets sd.frondsd settled to the .frond's
    // generation, from the journal or the full copy it needs, and no journals.
    @Test fun exportSettlesUnfinishedSave() {
        val oldPayload = ByteArray(512) { 0xa5.toByte() }
        val newPayload = oldPayload.copyOf().also { "abcd".toByteArray().copyInto(it, 8) }
        val dir = tmp.newFolder("pending")
        File(dir, "session.frond").writeBytes(v8WithSd(4))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsdV2(3, oldPayload))
        val journal = File(dir, "sd.frondsd.journal-4").also { it.writeBytes(journalTo(3, 8, "abcd".toByteArray(), newPayload)) }
        File(dir, "sd.frondsd.journal-9").writeText("stale")
        val dest = unpack(dir, "settled")
        assertArrayEquals(tinyFrondsdV2(4, newPayload), File(dest, "sd.frondsd").readBytes())
        assertEquals(listOf("sd.frondsd", "session.frond"), dest.list()!!.sorted())
        assertNull(checkImportedSiblings(dest, File(dest, "session.frond")))
        // A damaged journal makes no export.
        journal.writeBytes(journalTo(3, 8, "abcd".toByteArray(), newPayload).also { it[80] = 1 })
        assertThrows(java.io.IOException::class.java) { zipPackable(dir, ByteArrayOutputStream()) }
        // A first save's full copy, with no sd.frondsd yet.
        val first = tmp.newFolder("first")
        File(first, "session.frond").writeBytes(v8WithSd(1))
        File(first, "sd.frondsd.journal-1.full").writeBytes(tinyFrondsdV2(1, newPayload))
        val copied = unpack(first, "copied")
        assertArrayEquals(tinyFrondsdV2(1, newPayload), File(copied, "sd.frondsd").readBytes())
        assertEquals(listOf("sd.frondsd", "session.frond"), copied.list()!!.sorted())
        assertNull(checkImportedSiblings(copied, File(copied, "session.frond")))
    }

    // A recovery left with an unfinished save: adopting it moves the journal it needs along with
    // sd.frondsd, or the full copy in place of sd.frondsd.
    @Test fun adoptRecoveryMovesUnfinishedSave() {
        val oldPayload = ByteArray(512) { 0xa5.toByte() }
        val newPayload = oldPayload.copyOf().also { "abcd".toByteArray().copyInto(it, 8) }
        val recovery = tmp.newFolder("before-exchange")
        val frond = File(recovery, "before-exchange.frond").also { it.writeBytes(v8WithSd(4)) }
        File(recovery, "sd.frondsd").writeBytes(tinyFrondsdV2(3, oldPayload))
        val journal = journalTo(3, 8, "abcd".toByteArray(), newPayload)
        File(recovery, "sd.frondsd.journal-4").writeBytes(journal)
        File(recovery, "ms.frondms").writeText("ms")
        val dest = tmp.newFolder("adopted")
        assertTrue(moveRecoveryCards(recovery, frond, dest))
        assertEquals(listOf("ms.frondms", "sd.frondsd", "sd.frondsd.journal-4"), dest.list()!!.sorted())
        assertArrayEquals(journal, File(dest, "sd.frondsd.journal-4").readBytes())
        File(dest, "session.frond").writeBytes(v8WithSd(4))
        assertArrayEquals(tinyFrondsdV2(4, newPayload), File(unpack(dest, "adoptedZip"), "sd.frondsd").readBytes())
        // A full copy goes in as sd.frondsd.
        val full = tmp.newFolder("full")
        val fullFrond = File(full, "session.frond").also { it.writeBytes(v8WithSd(5)) }
        File(full, "sd.frondsd").writeBytes(tinyFrondsdV2(4, oldPayload))
        File(full, "sd.frondsd.journal-5.full").writeBytes(tinyFrondsdV2(5, newPayload))
        val fullDest = tmp.newFolder("fullAdopted")
        assertTrue(moveRecoveryCards(full, fullFrond, fullDest))
        assertEquals(listOf("sd.frondsd"), fullDest.list()!!.sorted())
        assertArrayEquals(tinyFrondsdV2(5, newPayload), File(fullDest, "sd.frondsd").readBytes())
    }

    // An export that fails partway, here on a damaged journal, deletes its target.
    @Test fun failedExportDeletesTarget() {
        val oldPayload = ByteArray(512) { 0xa5.toByte() }
        val newPayload = oldPayload.copyOf().also { "abcd".toByteArray().copyInto(it, 8) }
        val dir = tmp.newFolder("damaged")
        File(dir, "session.frond").writeBytes(v8WithSd(4))
        File(dir, "sd.frondsd").writeBytes(tinyFrondsdV2(3, oldPayload))
        File(dir, "sd.frondsd.journal-4").writeBytes(journalTo(3, 8, "abcd".toByteArray(), newPayload).also { it[80] = 1 })
        val target = File(tmp.root, "out.zip")
        assertNotNull(exportPackable(dir, { target.outputStream() }, { target.delete() }))
        assertFalse(target.exists())
        File(dir, "sd.frondsd.journal-4").writeBytes(journalTo(3, 8, "abcd".toByteArray(), newPayload))
        var deleted = false
        assertNull(exportPackable(dir, { target.outputStream() }, { deleted = true }))
        assertFalse(deleted)
        assertTrue(target.length() > 0)
    }

    @Test fun norMd5ReadsV8() {
        val metadata = byteArrayOf(1, 2, 3)
        val nor = ByteArray(5000) { (it * 7).toByte() }
        val packed = ByteArrayOutputStream().also { out ->
            java.util.zip.DeflaterOutputStream(out).use { it.write(metadata + nor) }
        }.toByteArray()
        val frond = File(tmp.newFolder("md5"), "session.frond")
        frond.writeBytes(v8WithSd(1, packed, metadata.size.toLong(), nor.size.toLong()))
        val want = java.security.MessageDigest.getInstance("MD5").digest(nor).joinToString("") { "%02x".format(it) }
        assertEquals(want, Sessions.norMd5(frond))
    }
}
