package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import kotlin.random.Random

// Raw images through the storage library: a failed change leaves the card alone and an add writes
// little. The old SD exchange's raw image journal is still replayed for its leftovers.
class FatJournalTest {
    private fun card(mib: Int, tag: String): File {
        val blank = FatImageTest().blank(mib)
        val file = File(blank.parentFile, "journal-$tag-$mib.img")
        file.delete()
        FatImage.journalOf(file).delete()
        assertTrue(blank.renameTo(file))
        return file
    }

    // What a reader sees: every path with its bytes ("dir" for folders), and the free space.
    private fun snapshot(file: File): Pair<Map<String, String>, Long> = FatImage.open(file, false).use { fat ->
        assertTrue(fat.fatCopiesMatch())
        val out = sortedMapOf<String, String>()
        fun walk(path: String) {
            for (e in fat.list(path)) {
                val full = SdCard.joined(path, e.name)
                if (e.directory) { out[full] = "dir"; walk(full) } else {
                    val bytes = ByteArrayOutputStream().also { fat.read(e, it) }.toByteArray()
                    out[full] = bytes.contentHashCode().toString() + ":" + bytes.size
                }
            }
        }
        walk("/")
        out to fat.freeBytes
    }

    // A complete journal is replayed and goes; a damaged one just goes.
    @Test fun oldExchangeJournalsAreReplayed() {
        for (damaged in listOf(false, true)) {
            val file = card(32, "old-$damaged")
            try {
                val before = file.readBytes()
                val sector = Random(5).nextBytes(512)
                val journal = FatImage.encodeJournal(listOf(4096L to sector), file.length())
                FatImage.journalOf(file).writeBytes(if (damaged) journal.copyOf(journal.size - 7) else journal)
                FatImage.recover(file)
                assertFalse(FatImage.journalOf(file).exists())
                val after = file.readBytes()
                if (damaged) assertArrayEquals(before, after)
                else assertArrayEquals(sector, after.copyOfRange(4096, 4608))
            } finally {
                file.delete()
                FatImage.journalOf(file).delete()
            }
        }
    }

    @Test fun failedChangeLeavesTheCardAlone() {
        val file = card(32, "fail")
        try {
            val old = snapshot(file)
            FatImage.open(file, true).use { fat ->
                try { fat.add("/", "x", ByteArray(10).inputStream(), 1000); fail("short input") } catch (_: FatImage.FatException) { }
            }
            assertEquals(old, snapshot(file))
        } finally {
            file.delete()
        }
    }

    // A 1 MB add on a 2 GiB card writes the data and little else.
    @Test fun addOn2GiBCardWritesLittle() {
        val file = card(2048, "big")
        try {
            val big = Random(3).nextBytes(1 shl 20)
            val written = FatImage.open(file, true).use { fat ->
                fat.add("/", "one.bin", big.inputStream(), big.size.toLong())
                fat.bytesWritten
            }
            println("1 MiB add on a 2 GiB card wrote $written bytes")
            assertTrue("wrote $written", written < (1 shl 20) + (16 shl 10))
            FatImage.open(file, false).use { fat ->
                val out = ByteArrayOutputStream().also { fat.read(fat.find("/one.bin")!!, it) }
                assertArrayEquals(big, out.toByteArray())
            }
            assertEquals(2048L shl 20, file.length())
        } finally {
            file.delete()
        }
    }
}
