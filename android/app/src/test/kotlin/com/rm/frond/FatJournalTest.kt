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

// Each card change is cut off after every step and must read as the old or the new card.
class FatJournalTest {
    private class Crash : RuntimeException()

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

    private fun crashTest(tag: String, prepare: (FatImage) -> Unit, change: (FatImage) -> Unit) {
        val expectNew = mapOf("data" to false, "journal" to true, "partly" to true, "applied" to true, "damaged" to false)
        for ((step, isNew) in expectNew) {
            val file = card(32, tag)
            try {
                FatImage.open(file, true).use(prepare)
                val old = snapshot(file)
                val reference = card(32, "$tag-ref")
                FatImage.open(reference, true).use { prepare(it); change(it) }
                val new = snapshot(reference)
                reference.delete()
                FatImage.open(file, true).use { fat ->
                    fat.crashHook = { if (it == (if (step == "damaged") "journal" else step)) throw Crash() }
                    try { change(fat); fail("$step didn't crash") } catch (_: Crash) { }
                    assertFalse(fat.committed)
                }
                val journal = FatImage.journalOf(file)
                if (step == "damaged") journal.writeBytes(journal.readBytes().copyOf(journal.length().toInt() - 7))
                assertEquals("$tag $step journal", step != "data", journal.exists())
                val after = snapshot(file)
                assertFalse(journal.exists())
                assertEquals("$tag $step", if (isNew) new else old, after)
                // The card still takes changes after recovery.
                FatImage.open(file, true).use { it.add("/", "after.txt", byteArrayOf(9).inputStream(), 1) }
                assertTrue(snapshot(file).first.containsKey("/after.txt"))
            } finally {
                file.delete()
                FatImage.journalOf(file).delete()
            }
        }
    }

    private val data = Random(7).nextBytes(300_001)

    @Test fun addSurvivesCrashes() = crashTest("add", { it.makeFolder("/", "PALM") }) {
        it.add("/PALM", "A long file name.prc", data.inputStream(), data.size.toLong())
    }

    @Test fun makeFolderSurvivesCrashes() = crashTest("mkdir", { it.makeFolder("/", "PALM") }) {
        it.makeFolder("/PALM", "Launcher")
    }

    // A full subfolder grows by a cluster in the same change.
    @Test fun growingFolderSurvivesCrashes() = crashTest("grow", { fat ->
        fat.makeFolder("/", "PALM")
        for (i in 0 until 60) fat.add("/PALM", "Long file name number $i.pdb", byteArrayOf(i.toByte()).inputStream(), 1)
    }) {
        it.add("/PALM", "One more long file name.pdb", data.inputStream(), data.size.toLong())
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

    // A 1 MB add on a 2 GiB card writes the data, a small journal and the sectors it names.
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
