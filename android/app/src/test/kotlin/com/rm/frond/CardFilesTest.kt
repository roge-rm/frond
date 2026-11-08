package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.OutputStream
import kotlin.random.Random

// The card file manager's operations on a FAT16 image: copy, move, delete, rename, clash naming,
// folder import and export, non-ASCII names.
class CardFilesTest {
    private fun card(): File = FatImageTest().blank(40)

    private fun FatImage.put(folder: String, name: String, data: ByteArray) = add(folder, name, data.inputStream(), data.size.toLong())

    private fun FatImage.get(path: String): ByteArray {
        val out = ByteArrayOutputStream()
        read(find(path)!!, out)
        return out.toByteArray()
    }

    private fun FatImage.names(path: String) = list(path).map { it.name }.sorted()

    private val ask = CardFiles.Resolver { CardFiles.Decision(CardFiles.Clash.KEEP_BOTH) }
    private fun always(c: CardFiles.Clash) = CardFiles.Resolver { CardFiles.Decision(c) }

    private val big = Random(7).nextBytes(150_000)

    private fun seeded(file: File) = FatImage.open(file, true).also { fat ->
        fat.makeFolder("/", "Music")
        fat.makeFolder("/Music", "Album")
        fat.put("/Music/Album", "track one.sunvox", big)
        fat.put("/Music", "notes.txt", "hello".toByteArray())
        fat.makeFolder("/", "Other")
        fat.put("/", "root.bin", byteArrayOf(1, 2, 3))
    }

    @Test fun copyFileAndFolderKeepsContent() {
        seeded(card()).use { fat ->
            val used = fat.freeBytes
            CardFiles.paste(fat, listOf("/Music", "/root.bin"), "/Other", false, ask)
            assertEquals(listOf("Music", "root.bin"), fat.names("/Other"))
            assertArrayEquals(big, fat.get("/Other/Music/Album/track one.sunvox"))
            assertArrayEquals("hello".toByteArray(), fat.get("/Other/Music/notes.txt"))
            // The originals are still there and the copies took room.
            assertArrayEquals(big, fat.get("/Music/Album/track one.sunvox"))
            assertTrue(fat.freeBytes < used)
            assertEquals(fat.find("/Music/notes.txt")!!.modified, fat.find("/Other/Music/notes.txt")!!.modified)
            assertTrue(fat.fatCopiesMatch())
        }
    }

    @Test fun moveGoesAcrossFoldersAndFixesParent() {
        val file = card()
        seeded(file).use { fat ->
            val used = fat.freeBytes
            CardFiles.paste(fat, listOf("/Music/Album"), "/Other", true, ask)
            assertEquals(listOf("notes.txt"), fat.names("/Music"))
            assertArrayEquals(big, fat.get("/Other/Album/track one.sunvox"))
            assertEquals(used, fat.freeBytes)
            // The moved folder's ".." points at its new parent: a folder made in it still lists, and a second move works.
            fat.makeFolder("/Other/Album", "Deeper")
            CardFiles.paste(fat, listOf("/Other/Album"), "/", true, ask)
            assertEquals(listOf("Album", "Music", "Other", "root.bin"), fat.names("/"))
            assertEquals(listOf("Deeper", "track one.sunvox"), fat.names("/Album"))
        }
        FatImage.open(file, false).use { assertTrue(it.fatCopiesMatch()); assertEquals(listOf("Deeper", "track one.sunvox"), it.names("/Album")) }
    }

    @Test fun deleteTreeFreesEverything() {
        val file = card()
        FatImage.open(file, true).use { fat ->
            val empty = fat.freeBytes
            fat.makeFolder("/", "Tree")
            fat.makeFolder("/Tree", "A")
            fat.makeFolder("/Tree/A", "B")
            fat.put("/Tree/A/B", "deep.bin", big)
            fat.put("/Tree/A", "mid.bin", big)
            fat.put("/Tree", "top.bin", big)
            fat.put("/", "keep.bin", byteArrayOf(9))
            val before = fat.freeBytes
            assertTrue(before < empty)
            CardFiles.delete(fat, listOf("/Tree"))
            assertNull(fat.find("/Tree"))
            assertEquals(listOf("keep.bin"), fat.names("/"))
            // Only keep.bin's cluster is still used.
            assertEquals(empty - fat.clusterBytes, fat.freeBytes)
        }
        FatImage.open(file, false).use { assertEquals(listOf("keep.bin"), it.names("/")); assertTrue(it.fatCopiesMatch()) }
    }

    @Test fun keepBothNames() {
        assertEquals("a (2).txt", CardFiles.keepBothName("a.txt") { it == "a.txt" })
        assertEquals("a (3).txt", CardFiles.keepBothName("a.txt") { it == "a (2).txt".lowercase() || it == "a.txt" || it == "a (2).txt" })
        assertEquals("folder (2)", CardFiles.keepBothName("folder") { false })
        assertEquals(".hidden (2)", CardFiles.keepBothName(".hidden") { false })
        assertEquals("archive.tar (2).gz", CardFiles.keepBothName("archive.tar.gz") { false })
        val long = "x".repeat(250) + ".dat"
        val name = CardFiles.keepBothName(long) { false }
        assertTrue(name.length <= 255)
        assertTrue(name.endsWith(" (2).dat"))
        FatImage.checkName(name)
        // A pair split by the cut doesn't leave half a character.
        val cut = CardFiles.keepBothName("a" + "😀".repeat(130)) { false }
        assertTrue(cut.length <= 255)
        var i = 0
        while (i < cut.length) {
            if (cut[i].isHighSurrogate()) { assertTrue(cut[i + 1].isLowSurrogate()); i += 2 } else { assertTrue(!cut[i].isLowSurrogate()); i++ }
        }
        assertTrue(cut.endsWith(" (2)"))
    }

    @Test fun clashesFollowTheChoice() {
        seeded(card()).use { fat ->
            fat.put("/Other", "root.bin", byteArrayOf(7, 7))
            // Keep both
            CardFiles.paste(fat, listOf("/root.bin"), "/Other", false, always(CardFiles.Clash.KEEP_BOTH))
            assertEquals(listOf("root (2).bin", "root.bin"), fat.names("/Other"))
            assertArrayEquals(byteArrayOf(1, 2, 3), fat.get("/Other/root (2).bin"))
            // Skip
            val skip = CardFiles.paste(fat, listOf("/root.bin"), "/Other", false, always(CardFiles.Clash.SKIP))
            assertEquals(1, skip.skipped)
            assertEquals(listOf("root (2).bin", "root.bin"), fat.names("/Other"))
            assertArrayEquals(byteArrayOf(7, 7), fat.get("/Other/root.bin"))
            // Replace, differently cased
            fat.put("/Other", "ROOT.BIN2", byteArrayOf(0))
            CardFiles.paste(fat, listOf("/root.bin"), "/Other", true, always(CardFiles.Clash.REPLACE))
            assertArrayEquals(byteArrayOf(1, 2, 3), fat.get("/Other/root.bin"))
            assertNull(fat.find("/root.bin"))
            // Copy next to the original needs no question.
            val none = CardFiles.Resolver { fail("asked"); CardFiles.Decision(CardFiles.Clash.SKIP) }
            CardFiles.paste(fat, listOf("/Other/root.bin"), "/Other", false, none)
            assertNotNull(fat.find("/Other/root (3).bin"))
        }
    }

    @Test fun forAllAsksOnce() {
        seeded(card()).use { fat ->
            fat.put("/Other", "root.bin", byteArrayOf(5))
            fat.put("/Other", "notes.txt", byteArrayOf(5))
            fat.put("/", "notes.txt", byteArrayOf(6))
            var asked = 0
            val resolver = CardFiles.Resolver { asked++; CardFiles.Decision(CardFiles.Clash.SKIP, all = true) }
            val report = CardFiles.paste(fat, listOf("/root.bin", "/notes.txt"), "/Other", false, resolver)
            assertEquals(1, asked)
            assertEquals(2, report.skipped)
            assertEquals(0, report.done)
        }
    }

    @Test fun cancelStopsAndKeepsEarlierWork() {
        seeded(card()).use { fat ->
            val resolver = CardFiles.Resolver { CardFiles.Decision(CardFiles.Clash.CANCEL) }
            fat.put("/Other", "root.bin", byteArrayOf(5))
            try { CardFiles.paste(fat, listOf("/root.bin"), "/Other", false, resolver); fail("not cancelled") } catch (e: FatImage.Cancelled) {}
            assertArrayEquals(byteArrayOf(5), fat.get("/Other/root.bin"))
        }
    }

    @Test fun aFolderCantGoIntoItself() {
        seeded(card()).use { fat ->
            for (move in listOf(true, false)) {
                for (target in listOf("/Music", "/Music/Album")) {
                    try {
                        CardFiles.paste(fat, listOf("/root.bin", "/Music"), target, move, ask)
                        fail("moved into itself")
                    } catch (e: FatImage.FatException) {
                        assertTrue(e.message!!.contains("can't go into itself"))
                    }
                }
            }
            // Nothing happened, not even for the file listed before the folder.
            assertEquals(listOf("Music", "Other", "root.bin"), fat.names("/"))
            assertEquals(listOf("Album", "notes.txt"), fat.names("/Music"))
            try { fat.move("/Music", "/music/album"); fail("moved") } catch (e: FatImage.FatException) {}
            try { fat.copy("/Music", "/Music/Album"); fail("copied") } catch (e: FatImage.FatException) {}
            assertEquals(listOf("track one.sunvox"), fat.names("/Music/Album"))
            assertTrue(fat.fatCopiesMatch())
        }
    }

    @Test fun replacingAnAncestorIsRefused() {
        seeded(card()).use { fat ->
            fat.makeFolder("/Music/Album", "Music")
            fat.put("/Music/Album/Music", "inner.txt", byteArrayOf(1))
            try {
                CardFiles.paste(fat, listOf("/Music/Album/Music"), "/", true, always(CardFiles.Clash.REPLACE))
                fail("replaced the folder holding the source")
            } catch (e: FatImage.FatException) {}
            assertNotNull(fat.find("/Music/Album/Music/inner.txt"))
        }
    }

    @Test fun nonAsciiNamesRoundTrip() {
        val names = listOf("日本語のファイル.txt", "naïve café ☃.txt", "Ünïcödé", "смайлик 😀.bin")
        val file = card()
        FatImage.open(file, true).use { fat ->
            for (n in names) if (n == "Ünïcödé") fat.makeFolder("/", n) else fat.put("/", n, n.toByteArray())
            fat.makeFolder("/", "Dest")
            CardFiles.paste(fat, names.map { "/$it" }, "/Dest", false, ask)
            CardFiles.paste(fat, names.map { "/$it" }, "/Dest", true, always(CardFiles.Clash.KEEP_BOTH))
            CardFiles.rename(fat, "/Dest/日本語のファイル.txt", "ファイル名.txt")
        }
        FatImage.open(file, false).use { fat ->
            assertTrue(fat.names("/").none { it != "Dest" })
            val dest = fat.names("/Dest")
            assertTrue("ファイル名.txt" in dest)
            assertTrue("naïve café ☃ (2).txt" in dest)
            assertTrue("смайлик 😀 (2).bin" in dest)
            assertArrayEquals("naïve café ☃.txt".toByteArray(), fat.get("/Dest/naïve café ☃.txt"))
            assertArrayEquals("naïve café ☃.txt".toByteArray(), fat.get("/Dest/naïve café ☃ (2).txt"))
            assertTrue(fat.find("/Dest/Ünïcödé")!!.directory)
        }
    }

    @Test fun renameChangesCaseAndRefusesTaken() {
        seeded(card()).use { fat ->
            CardFiles.rename(fat, "/root.bin", "ROOT.bin")
            assertTrue("ROOT.bin" in fat.names("/"))
            CardFiles.rename(fat, "/Music", "Music Library")
            assertEquals(listOf("Album", "notes.txt"), fat.names("/Music Library"))
            try { CardFiles.rename(fat, "/ROOT.bin", "other"); fail("taken") } catch (e: FatImage.FatException) {}
            try { CardFiles.rename(fat, "/ROOT.bin", "a:b"); fail("bad name") } catch (e: FatImage.FatException) {}
            assertTrue("ROOT.bin" in fat.names("/"))
        }
    }

    // ---- Folder import and export ----

    private fun dir(name: String, vararg kids: FolderCardTest.Fake) = FolderCardTest.Fake(name, kids = kids.toList())
    private fun file(name: String, content: ByteArray) = FolderCardTest.Fake(name, bytes = content)

    @Test fun importKeepsTheStructure() {
        val tree = dir("Pictures",
            file("a.jpg", big),
            dir("2024", dir("Trip", file("日本.jpg", big.copyOf(1000)), file("Long name with spaces and more words.png", byteArrayOf(4)))),
            dir("Empty"))
        val file = card()
        FatImage.open(file, true).use { fat ->
            fat.makeFolder("/", "DCIM")
            val report = CardFiles.importTree(fat, tree, "/DCIM", ask)
            assertEquals(3, report.done)
            assertEquals(listOf("Pictures"), fat.names("/DCIM"))
            assertEquals(listOf("2024", "Empty", "a.jpg"), fat.names("/DCIM/Pictures"))
            assertArrayEquals(big, fat.get("/DCIM/Pictures/a.jpg"))
            assertArrayEquals(big.copyOf(1000), fat.get("/DCIM/Pictures/2024/Trip/日本.jpg"))
            assertTrue(fat.find("/DCIM/Pictures/Empty")!!.directory)
            assertTrue(fat.list("/DCIM/Pictures/Empty").isEmpty())
            // A second import fills the same folder and follows the clash rule.
            val again = CardFiles.importTree(fat, tree, "/DCIM", always(CardFiles.Clash.KEEP_BOTH))
            assertEquals(3, again.done)
            assertEquals(listOf("Pictures"), fat.names("/DCIM"))
            assertNotNull(fat.find("/DCIM/Pictures/a (2).jpg"))
            assertNotNull(fat.find("/DCIM/Pictures/2024/Trip/日本 (2).jpg"))
            assertTrue(fat.fatCopiesMatch())
        }
    }

    @Test fun importRefusesNamesFatCantHold() {
        val tree = dir("Bad", file("what?.txt", byteArrayOf(1)))
        FatImage.open(card(), true).use { fat ->
            try { CardFiles.importTree(fat, tree, "/", ask); fail("imported") } catch (e: FatImage.FatException) {}
        }
    }

    private class Sink : CardFiles.HostFolder {
        val files = linkedMapOf<String, ByteArray>()
        val folders = linkedMapOf<String, Sink>()
        override fun folder(name: String) = Sink().also { folders[name] = it }
        override fun file(name: String): OutputStream = object : ByteArrayOutputStream() {
            override fun close() { super.close(); files[name] = toByteArray() }
        }
    }

    @Test fun exportCopiesFoldersOff() {
        seeded(card()).use { fat ->
            val sink = Sink()
            val n = CardFiles.exportTree(fat, listOf("/Music", "/root.bin"), sink)
            assertEquals(3, n)
            assertArrayEquals(byteArrayOf(1, 2, 3), sink.files["root.bin"])
            assertArrayEquals("hello".toByteArray(), sink.folders["Music"]!!.files["notes.txt"])
            assertArrayEquals(big, sink.folders["Music"]!!.folders["Album"]!!.files["track one.sunvox"])
        }
    }

    @Test fun addStreamReplaceChecksRoomFirst() {
        seeded(card()).use { fat ->
            fat.put("/", "small.bin", byteArrayOf(1))
            val huge = (fat.freeBytes + 1_000_000).toLong()
            try {
                CardFiles.addStream(fat, "/", "small.bin", huge, { java.io.InputStream.nullInputStream() }, always(CardFiles.Clash.REPLACE))
                fail("fit")
            } catch (e: FatImage.FatException) {}
            // The old file is still there.
            assertArrayEquals(byteArrayOf(1), fat.get("/small.bin"))
        }
    }

    @Test fun sortingByNameSizeAndDate() {
        fun e(name: String, dir: Boolean, size: Long, modified: Long) = FatImage.Entry(name, name, dir, size, 0, modified)
        val list = listOf(e("b.txt", false, 5, 100), e("A.txt", false, 50, 50), e("zdir", true, 0, 10), e("c.txt", false, 1, 200))
        assertEquals(listOf("zdir", "A.txt", "b.txt", "c.txt"), FileList.sorted(list, FileList.Sort.NAME).map { it.name })
        assertEquals(listOf("zdir", "A.txt", "b.txt", "c.txt"), FileList.sorted(list, FileList.Sort.SIZE).map { it.name })
        assertEquals(listOf("zdir", "c.txt", "b.txt", "A.txt"), FileList.sorted(list, FileList.Sort.DATE).map { it.name })
    }
}
