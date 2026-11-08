package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import kotlin.random.Random

// Files… edits a .frondsd or .frondmsp in place: few writes, one generation per visit, and a cut-off
// change settles to a card that loads.
class CardFileTest {
    private class Crash : RuntimeException()

    private fun dir(tag: String): File =
        File(System.getProperty("frond.fatTestDir") ?: "build/fat-test", "cardfile-$tag").also { it.deleteRecursively(); it.mkdirs() }

    // A 32 MiB FAT16 card as a v2 card file at `generation`, or v1.
    private fun card(tag: String, name: String = "sd.frondsd", generation: Long = 5, v1: Boolean = false): File {
        val raw = FatImageTest().blank(32)
        val payload = raw.readBytes()
        raw.delete()
        val file = File(dir(tag), name)
        if (v1) {
            val head = ByteArray(64)
            "FRONDSD\u0000".toByteArray(Charsets.US_ASCII).copyInto(head)
            val b = java.nio.ByteBuffer.wrap(head).order(java.nio.ByteOrder.LITTLE_ENDIAN)
            b.putInt(8, 1).putInt(12, 512).putLong(16, payload.size.toLong())
            b.putInt(24, java.util.zip.CRC32().also { it.update(payload) }.value.toInt())
            file.writeBytes(head + payload)
        } else file.writeBytes(CardFile.encodeV2(payload, generation))
        assertEquals(if (v1) 0L else generation, CardFile.verify(file))
        return file
    }

    private fun visit(file: File, block: (FatImage) -> Unit) = CardFile.open(file, true).use { c -> FatImage.over(c).use(block) }

    private fun FatImage.put(folder: String, name: String, data: ByteArray) = add(folder, name, data.inputStream(), data.size.toLong())

    // Every path with its bytes ("dir" for folders), and the free space.
    private fun snapshot(file: File): Pair<Map<String, String>, Long> = CardFile.open(file, false).use { c ->
        FatImage.over(c).use { fat ->
            assertEquals(emptyList<String>(), fat.check())
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
    }

    @Test fun anEditWritesOnlyWhatItTouches() {
        for (name in listOf("sd.frondsd", "ms.frondmsp")) {
            val file = card("few-$name", name)
            visit(file) { it.makeFolder("/", "PALM") }
            val before = file.readBytes()
            val data = Random(1).nextBytes(5000)
            val (writes, bytes) = CardFile.open(file, true).use { c ->
                FatImage.over(c).use { it.put("/PALM", "A long file name.prc", data) }
                c.writes.toList() to c.cardBytesWritten
            }
            val after = file.readBytes()
            assertEquals(before.size, after.size)
            // The data clusters (5000 bytes in 2 KiB clusters), a FAT sector in each copy, the folder
            // sector, one table entry and the header.
            println("$name: ${writes.size} writes, $bytes bytes: $writes")
            assertTrue("wrote $bytes", bytes <= 3 * 2048 + 3 * 512 + 4 + 64)
            assertTrue(writes.size <= 8)
            for (i in before.indices) if (before[i] != after[i]) {
                assertTrue("byte $i changed outside a write", writes.any { (at, n) -> i >= at && i < at + n })
            }
            assertEquals(7L, CardFile.verify(file))
            val read = CardFile.open(file, false).use { c -> FatImage.over(c).use { f -> ByteArrayOutputStream().also { f.read(f.find("/PALM/A long file name.prc")!!, it) } } }
            assertArrayEquals(data, read.toByteArray())
        }
    }

    @Test fun theGenerationMovesOncePerVisit() {
        val file = card("generation")
        var marked = 0
        CardFile.open(file, true).use { c ->
            c.beforeFirstWrite = { marked++ }
            FatImage.over(c).use { assertTrue(it.list("/").isEmpty()) }
            assertEquals(0, marked)
            FatImage.over(c).use { it.makeFolder("/", "PALM") }
            FatImage.over(c).use { it.put("/PALM", "one.txt", byteArrayOf(1, 2, 3)) }
            FatImage.over(c).use { it.remove("/PALM/one.txt") }
            assertEquals(6L, c.generation)
        }
        assertEquals(1, marked)
        assertEquals(6L, CardFile.verify(file))
        // Looking only changes nothing.
        CardFile.open(file, true).use { c -> FatImage.over(c).use { it.list("/PALM") } }
        assertEquals(6L, CardFile.verify(file))
        visit(file) { it.makeFolder("/", "DCIM") }
        assertEquals(7L, CardFile.verify(file))
    }

    @Test fun aV1CardBecomesV2() {
        val file = card("v1", v1 = true)
        visit(file) { it.put("/", "x.txt", byteArrayOf(7)) }
        assertEquals(1L, CardFile.verify(file))
        assertEquals(64 + (32L shl 20) + 4 * 32, file.length())
        assertTrue(snapshot(file).first.containsKey("/x.txt"))
    }

    @Test fun aFailedChangeLeavesALoadableCard() {
        val file = card("failed")
        val old = snapshot(file)
        CardFile.open(file, true).use { c ->
            FatImage.over(c).use { fat ->
                // The input is short: some data was written, then the change failed.
                try { fat.add("/", "x", ByteArray(5000).inputStream(), 9000); fail("short input") } catch (_: FatImage.FatException) { }
            }
        }
        assertFalse(CardFile.unsettled(file))
        assertEquals(6L, CardFile.verify(file))
        assertEquals(old, snapshot(file))
    }

    @Test fun oneEditorAtATime() {
        val file = card("lock")
        CardFile.open(file, true).use {
            assertTrue(CardFile.inUse(file))
            assertFalse(Cards.ready(file))
            try { CardFile.open(file, true); fail("opened twice") } catch (e: IOException) { assertEquals(CardFile.IN_USE, e.message) }
        }
        assertFalse(CardFile.inUse(file))
        assertTrue(Cards.ready(file))
    }

    // Whether another process can take the file's lock (python3's lockf, a POSIX lock as the
    // FileChannel's is). Null when python3 isn't here.
    private fun lockedElsewhere(file: File): Boolean? {
        val script = "import fcntl,sys\nf=open(sys.argv[1],'r+')\ntry:\n fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB);print('free')\nexcept OSError:\n print('held')\n"
        val p = try { ProcessBuilder("python3", "-c", script, file.path).redirectErrorStream(true).start() } catch (e: IOException) { return null }
        val out = p.inputStream.bufferedReader().readText().trim()
        p.waitFor()
        return when (out) { "held" -> true; "free" -> false; else -> null }
    }

    // A second try, a check or a read of a file this process holds locked never opens and closes
    // another descriptor of it, which would drop the lock for the whole process.
    @Test fun checksNeverDropAHeldLock() {
        val file = card("held")
        val machine = dir("held-machine")
        CardFile.open(file, true).use {
            Cards.tryRunLock(machine)!!.use {
                assertNull(Cards.tryRunLock(machine))
                Cards.tryRunLock(machine)?.close()
                assertTrue(CardFile.inUse(file))
                assertFalse(Cards.ready(file))
                assertTrue(CardFile.settle(file))
                assertEquals(5L, Cards.cardHeader(file)!!.first)
                val elsewhere = lockedElsewhere(File(machine, Cards.RUN_LOCK)) ?: return
                assertTrue("run lock dropped", elsewhere)
                assertEquals("card lock dropped", true, lockedElsewhere(file))
            }
            assertEquals(false, lockedElsewhere(File(machine, Cards.RUN_LOCK)))
            assertEquals(true, lockedElsewhere(file))
        }
        assertEquals(false, lockedElsewhere(file))
    }

    @Test fun aWriteProtectedCardRefusesChanges() {
        val file = card("protected")
        val before = file.readBytes()
        CardFile.open(file, false).use { c ->
            FatImage.over(c).use { fat -> try { fat.makeFolder("/", "PALM"); fail("wrote") } catch (_: FatImage.FatException) { } }
        }
        assertArrayEquals(before, file.readBytes())
    }

    // Cut off at each step, as a killed process would be, then settled by the next open or load.
    private fun crashTest(tag: String, prepare: (FatImage) -> Unit, change: (FatImage) -> Unit) {
        val expectNew = mapOf("writing" to false, "data" to false, "journal" to true, "partly" to true, "applied" to true)
        for ((step, isNew) in expectNew) {
            val file = card("$tag-$step")
            visit(file, prepare)
            val old = snapshot(file)
            val reference = card("$tag-$step-ref")
            visit(reference) { prepare(it); change(it) }
            val new = snapshot(reference)
            CardFile.open(file, true).use { c ->
                c.crashHook = { if (it == step) { c.dead = true; throw Crash() } }
                FatImage.over(c).use { fat -> try { change(fat); fail("$tag $step didn't crash") } catch (_: Crash) { } }
            }
            assertTrue("$tag $step leaves a journal", CardFile.unsettled(file))
            // A standalone journal once the change's metadata is written, the pending list before.
            assertEquals("$tag $step", if (isNew) listOf(".journal-7", ".journal-7.pending") else listOf(".journal-7.pending"), besides(file))
            // The card the core would load now, before the settle, may be refused; after it, it loads.
            assertTrue(CardFile.settle(file))
            assertFalse(CardFile.unsettled(file))
            assertEquals("$tag $step generation", 7L, CardFile.verify(file))
            assertEquals("$tag $step", if (isNew) new else old, snapshot(file))
            // The card still takes changes, and the next visit moves the generation on again.
            visit(file) { it.put("/", "after.txt", byteArrayOf(9)) }
            assertTrue(snapshot(file).first.containsKey("/after.txt"))
            assertEquals(8L, CardFile.verify(file))
            file.parentFile!!.deleteRecursively()
            reference.parentFile!!.deleteRecursively()
        }
    }

    private val data = Random(7).nextBytes(300_001)

    @Test fun addSurvivesCrashes() = crashTest("add", { it.makeFolder("/", "PALM") }) {
        it.put("/PALM", "A long file name.prc", data)
    }

    @Test fun makeFolderSurvivesCrashes() = crashTest("mkdir", { it.makeFolder("/", "PALM") }) {
        it.makeFolder("/PALM", "Launcher")
    }

    @Test fun copySurvivesCrashes() = crashTest("copy", { fat ->
        fat.makeFolder("/", "PALM")
        fat.put("/PALM", "a.pdb", data)
        fat.makeFolder("/", "Backup")
    }) { it.copy("/PALM", "/Backup") }

    // A crash that leaves only the intent file, on a v1 card: the settle makes it a v2 card.
    @Test fun aV1CardSettlesToV2() {
        val file = card("v1-crash", v1 = true)
        val old = snapshot(file)
        CardFile.open(file, true).use { c ->
            c.crashHook = { if (it == "data") { c.dead = true; throw Crash() } }
            FatImage.over(c).use { fat -> try { fat.put("/", "x.bin", data); fail() } catch (_: Crash) { } }
        }
        // As a v1 card the whole CRC no longer matches.
        assertNull(CardFile.verify(file))
        assertTrue(CardFile.settle(file))
        assertEquals(1L, CardFile.verify(file))
        assertEquals(old, snapshot(file))
    }

    // A standalone journal's temp left by a crash goes at the next settle.
    @Test fun aStaleJournalTempGoes() {
        val file = card("temp")
        val temp = File(file.parentFile, ".frondsd.tmp-edit-sd.frondsd-abc123").also { it.writeBytes(ByteArray(100)) }
        val session = File(file.parentFile, ".frondsd.tmp-xyz789").also { it.writeBytes(ByteArray(100)) }
        assertTrue(CardFile.unsettled(file))
        assertNull(CardFile.notReady(file))
        assertFalse(temp.exists())
        assertTrue(session.exists())
        assertEquals(5L, CardFile.verify(file))
    }

    // A change left for another copy of the card says so, rather than that the card is open.
    @Test fun aMismatchedJournalSaysSo() {
        val file = card("mismatch")
        CardFile.open(file, true).use { c ->
            c.crashHook = { if (it == "journal") { c.dead = true; throw Crash() } }
            FatImage.over(c).use { fat -> try { fat.makeFolder("/", "PALM"); fail() } catch (_: Crash) { } }
        }
        val other = card("mismatch-other", generation = 9)
        other.copyTo(file, true)
        val message = CardFile.notReady(file)
        assertTrue(message, message != null && message != Cards.IN_FILES && "doesn't match" in message)
        try { CardFile.open(file, true).close(); fail("opened") } catch (e: IOException) { assertEquals(message, e.message) }
        CardFile.open(other, true).use { assertEquals(Cards.IN_FILES, CardFile.notReady(other)) }
    }

    // A 16 MiB card formats as FAT12 and takes the same changes.
    @Test fun aSmallCardIsFat12() {
        val file = File(dir("fat12"), "sd.frondsd").also { it.writeBytes(CardFile.encodeV2(ByteArray(16 shl 20), 3)) }
        CardFile.open(file, true).use { it.format() }
        assertEquals(4L, CardFile.verify(file))
        val data = Random(5).nextBytes(100_000)
        visit(file) { fat ->
            assertEquals(12, fat.fatBits)
            assertEquals(16384, fat.clusterBytes)
            fat.makeFolder("/", "PALM")
            fat.put("/PALM", "A long file name.prc", data)
            fat.copy("/PALM", "/", "PALM 2")
            assertEquals(emptyList<String>(), fat.check())
        }
        assertEquals(5L, CardFile.verify(file))
        assertEquals(snapshot(file).first["/PALM/A long file name.prc"], snapshot(file).first["/PALM 2/A long file name.prc"])
    }

    // ---- Leftovers shared with the desktop ----
    // tests/fixtures/card-leftovers/<side>/: a card cut off at the commit file ("commit") and at
    // the data ("intent"), written by each app's CardFile, with what it holds once settled. Each
    // app settles both sides' fixtures. FROND_FIXTURE_OUT=<dir> writes this side's again.

    private val leftovers: File?
        get() = generateSequence(File("").absoluteFile) { it.parentFile }
            .map { File(it, "tests/fixtures/card-leftovers") }.firstOrNull { it.isDirectory }

    // The card file as its 512-byte blocks that aren't all zero: "FRONDSP1", the length, then each
    // block's offset and bytes (little endian).
    private fun sparse(file: File): ByteArray {
        val b = file.readBytes()
        val out = java.io.ByteArrayOutputStream()
        val head = java.nio.ByteBuffer.allocate(16).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        head.put("FRONDSP1".toByteArray(Charsets.US_ASCII)).putLong(b.size.toLong())
        out.write(head.array())
        for (at in b.indices step 512) {
            val block = b.copyOfRange(at, minOf(at + 512, b.size)).copyOf(512)
            if (block.all { it == 0.toByte() }) continue
            out.write(java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(at.toLong()).array())
            out.write(block)
        }
        return out.toByteArray()
    }

    private fun unsparse(b: ByteArray, to: File) {
        val buf = java.nio.ByteBuffer.wrap(b).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        assertEquals("FRONDSP1", String(b, 0, 8, Charsets.US_ASCII))
        buf.position(8)
        val out = ByteArray(buf.long.toInt())
        while (buf.remaining() >= 520) {
            val at = buf.long.toInt()
            val block = ByteArray(512).also { buf.get(it) }
            block.copyInto(out, at, 0, minOf(512, out.size - at))
        }
        to.writeBytes(out)
    }

    // The generation, then each path with "dir" or its size and CRC-32, by path.
    private fun expected(file: File): String = CardFile.open(file, false).use { c ->
        FatImage.over(c).use { fat ->
            assertEquals(emptyList<String>(), fat.check())
            val out = sortedMapOf<String, String>()
            fun walk(path: String) {
                for (e in fat.list(path)) {
                    val full = SdCard.joined(path, e.name)
                    if (e.directory) { out[full] = "dir"; walk(full) } else {
                        val bytes = ByteArrayOutputStream().also { fat.read(e, it) }.toByteArray()
                        out[full] = "${bytes.size}\t%08x".format(java.util.zip.CRC32().also { it.update(bytes) }.value)
                    }
                }
            }
            walk("/")
            "generation ${CardFile.verify(file)}\n" + out.entries.joinToString("") { "${it.key}\t${it.value}\n" }
        }
    }

    private val leftoverCases = listOf("commit" to "journal", "intent" to "data")

    // The suffixes of the files beside `file` whose names start with its name and a dot.
    private fun besides(file: File): List<String> =
        file.parentFile!!.list().orEmpty().filter { it.startsWith(file.name + ".") }.map { it.substring(file.name.length) }.sorted()

    @Test fun writeLeftoverFixtures() {
        val out = System.getenv("FROND_FIXTURE_OUT")?.let { File(it) } ?: return
        out.deleteRecursively(); out.mkdirs()
        for ((name, step) in leftoverCases) {
            val file = card("fixture-$name")
            visit(file) { it.makeFolder("/", "PALM") }
            CardFile.open(file, true).use { c ->
                c.crashHook = { if (it == step) { c.dead = true; throw Crash() } }
                FatImage.over(c).use { fat ->
                    try { fat.put("/PALM", "\u00dcn\u00efcode long name for the card.prc", Random(41).nextBytes(5_000)); fail() } catch (_: Crash) { }
                }
            }
            File(out, "$name.sparse").writeBytes(sparse(file))
            for (suffix in besides(file)) File(file.path + suffix).copyTo(File(out, name + suffix))
            assertTrue(CardFile.settle(file))
            File(out, "$name.expect").writeText(expected(file))
        }
    }

    @Test fun leftoversFromEitherAppSettle() {
        val root = leftovers ?: return fail("no tests/fixtures/card-leftovers")
        for (side in listOf("android", "desktop")) for ((name, _) in leftoverCases) {
            val from = File(root, side)
            val file = File(dir("leftover-$side-$name"), "sd.frondsd")
            unsparse(File(from, "$name.sparse").readBytes(), file)
            for (suffix in besides(File(from, name))) if (suffix != ".sparse" && suffix != ".expect") File(from, name + suffix).copyTo(File(file.path + suffix))
            assertTrue("$side $name", CardFile.unsettled(file))
            assertTrue(Cards.ready(file))
            assertFalse(CardFile.unsettled(file))
            assertEquals("$side $name", File(from, "$name.expect").readText(), expected(file))
        }
    }

    // With FROND_CARD_OUT set, the edited cards are kept there (with the files they should hold in
    // <name>.files/) so the core can be made to load them.
    @Test fun writeCardsForTheCore() {
        val out = System.getenv("FROND_CARD_OUT")?.let { File(it) } ?: return
        out.deleteRecursively(); out.mkdirs()
        val long = "A rather long file name for a card, with spaces and dots.v2.prc"
        val files = mapOf(
            "/PALM/$long" to Random(11).nextBytes(70_000),
            "/PALM/Launcher/h\u00e9llo \u00fcber \u65e5\u672c.txt" to "caf\u00e9".toByteArray(),
            "/PALM/Launcher/Deep/x.bin" to Random(12).nextBytes(5_000),
            // The files the SD probe on the Palm looks for.
            "/HOST.TXT" to "Hello from host\n".toByteArray(),
            "/DELETE.TXT" to "Delete me\n".toByteArray(),
        )
        fun folders(fat: FatImage) {
            fat.makeFolder("/", "PALM"); fat.makeFolder("/PALM", "Launcher"); fat.makeFolder("/PALM/Launcher", "Deep")
        }
        fun fill(fat: FatImage) {
            folders(fat)
            for ((path, bytes) in files) fat.put(path.substringBeforeLast('/'), path.substringAfterLast('/'), bytes)
        }
        fun keep(name: String, file: File) {
            file.copyTo(File(out, name), true)
            for ((path, bytes) in files) File(out, "$name.files$path").also { it.parentFile?.mkdirs() }.writeBytes(bytes)
        }
        val v2 = card("core-v2")
        visit(v2, ::fill)
        keep("v2.frondsd", v2)
        val v1 = card("core-v1", v1 = true)
        visit(v1, ::fill)
        assertEquals(1L, CardFile.verify(v1))
        keep("v1-to-v2.frondsd", v1)
        // Crashed with the commit file written, then settled.
        val crashed = card("core-crash")
        visit(crashed, ::folders)
        CardFile.open(crashed, true).use { c ->
            c.crashHook = { if (it == "journal") { c.dead = true; throw Crash() } }
            FatImage.over(c).use { fat ->
                try {
                    for ((path, bytes) in files) fat.put(path.substringBeforeLast('/'), path.substringAfterLast('/'), bytes)
                    fail("no crash")
                } catch (_: Crash) { }
            }
        }
        assertTrue(CardFile.settle(crashed))
        keep("crash-settled.frondsd", crashed)
        File(out, "crash-settled.frondsd.list").writeText(snapshot(crashed).first.keys.joinToString("\n") + "\n")
    }
}
