package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.security.MessageDigest

// Import folder as card: the size choice, the fresh card layout, and the copy onto a new card file
// from a fake tree.
class FolderCardTest {
    @get:Rule val tmp = TemporaryFolder()

    class Fake(
        override val name: String,
        private val bytes: ByteArray? = null,
        private val kids: List<Fake>? = null,
        private val declared: Long? = null,
    ) : FolderCard.Node {
        override val isDirectory get() = kids != null
        override val size get() = declared ?: bytes?.size?.toLong() ?: 0L
        override var modified = 0L
        override fun children(): List<FolderCard.Node> = kids.orEmpty()
        override fun open(): InputStream = ByteArrayInputStream(bytes ?: ByteArray(0))
    }

    private fun dir(name: String, vararg kids: Fake) = Fake(name, kids = kids.toList())
    private fun file(name: String, content: ByteArray) = Fake(name, bytes = content)
    private fun pattern(n: Int) = ByteArray(n) { (it * 31 + it / 4096).toByte() }

    private fun tree() = dir("My Music Collection",
        file("A rather long file name for a card.txt", "hello".toByteArray()),
        file("empty", ByteArray(0)),
        dir("Songs",
            dir("Album One",
                file("01 - A very long track title indeed.sunvox", pattern(3 shl 20)),
                file("notes.txt", "x".toByteArray())),
            dir("Album Two", dir("Deeper", dir("Deepest", file("readme.md", "nested".toByteArray()))))))

    @Test fun blankImageMatchesTheCoresLayout() {
        // SHA-256 of each sector's number (4 bytes, little endian) and bytes from frondSd::layout().
        val expected = mapOf(
            32 to "f2cdc40a28684c0dccbcb3962ecdf4947f9597d4f1c76b550a3849d9e71449d8",
            64 to "b67c48ef6490e30b06de729ea70f7437520bdce927fc5fc3eec099bf93714772",
            128 to "9beb67ba09f1fd4db6841123859758e683ce01a137f4c9c6498149cb90ae7f56",
            256 to "fe6913aa6729544301bc2423f2a5d74d01afe3bdb995af2189bc5f75f048b8d3",
            512 to "372d58c9beca4f4a31ceab0db91f7f3c837071b289c876bc48563b8dea4fdd4f",
            1024 to "b450f1910a76573622acdda8685dfbb097d78a59865233b41280a096d84b13b3",
            1886 to "0f1b4af41bed75f5e614bc2249f326722804a707eee9cb2661af70a7e43c11fa",
        )
        for ((mib, hash) in expected) {
            val out = ByteArrayOutputStream()
            for ((sector, bytes) in CoreLayout.layout(mib)) {
                out.write(ByteArray(4) { (sector shr (8 * it)).toByte() })
                out.write(bytes)
            }
            assertEquals("$mib MiB", hash, MessageDigest.getInstance("SHA-256").digest(out.toByteArray()).joinToString("") { "%02x".format(it) })
        }
    }

    @Test fun geometryMatchesWhatFatImageReads() {
        for (mib in listOf(32, 128, 1886)) {
            val image = tmp.newFile("g$mib.img")
            CoreLayout.blank(image, mib)
            assertEquals(mib.toLong() shl 20, image.length())
            FatImage.open(image, false).use {
                val g = FolderCard.geometry(mib)
                assertEquals(g.clusterBytes, it.clusterBytes)
                assertEquals(g.clusters.toLong() * g.clusterBytes, it.totalBytes)
                assertTrue(it.list("/").isEmpty())
            }
            image.delete()
        }
    }

    // A new 32 MiB card file as `make` leaves it, filled with the tree.
    private fun fill(name: String, root: FolderCard.Node, plan: FolderCard.Plan = FolderCard.measure(root), cancelled: () -> Boolean = { false },
                     progress: ((Long, Long) -> Unit)? = null): Pair<java.io.File, String?> {
        val card = tmp.newFile(name)
        CoreLayout.card(card, 32)
        val result = CardFile.open(card, true).use { c -> FatImage.over(c).use { FolderCard.fill(it, root, plan, cancelled, progress) } }
        return card to result
    }

    private fun <T> reading(card: java.io.File, use: (FatImage) -> T): T = CardFile.open(card, false).use { c -> FatImage.over(c).use(use) }

    @Test fun theSmallestSizeThatFitsIsChosen() {
        val sizes = CardChoices.sizesFor("sd")
        val small = FolderCard.measure(tree())
        assertNull(small.problem)
        assertEquals(5, small.files)
        assertEquals(5, small.folders)
        assertEquals(32, FolderCard.smallestFit(small, sizes))
        // 40 MB of files needs the next size up, because of the clusters and the folders.
        val forty = FolderCard.measure(dir("Big", Fake("huge.bin", declared = 40L shl 20)))
        assertEquals(64, FolderCard.smallestFit(forty, sizes))
        assertFalse(FolderCard.fits(forty, 32))
        // A card's data area is a bit under its size: 31.9 MB of files does not fit on 32 MB.
        val edge = FolderCard.measure(dir("Edge", Fake("a.bin", declared = 32L shl 20)))
        assertEquals(64, FolderCard.smallestFit(edge, sizes))
        // Many small files take a cluster each.
        val many = FolderCard.measure(dir("Many", *Array(20000) { Fake("f$it.txt", bytes = ByteArray(1)) }.toList().chunked(500).mapIndexed { i, c -> dir("d$i", *c.toTypedArray()) }.toTypedArray()))
        assertNull(many.problem)
        assertEquals(64, FolderCard.smallestFit(many, sizes))
        // Nothing fits a folder larger than the biggest card.
        assertNull(FolderCard.smallestFit(FolderCard.measure(dir("Huge", Fake("x", declared = 2000L shl 20))), sizes))
    }

    @Test fun unusableFoldersAreRefusedWithAReason() {
        assertTrue(FolderCard.measure(dir("a", file("Readme.txt", ByteArray(1)), file("README.TXT", ByteArray(1)))).problem!!.contains("differ only in case"))
        assertTrue(FolderCard.measure(dir("a", file("what?.txt", ByteArray(1)))).problem!!.contains("can't be used"))
        assertTrue(FolderCard.measure(dir("a", file("name.", ByteArray(1)))).problem!!.contains("can't be used"))
        val wide = dir("a", *Array(520) { file("file-$it.txt", ByteArray(0)) })
        assertTrue(FolderCard.measure(wide).problem!!.contains("too many names"))
        assertEquals(COPY_CANCELLED, FolderCard.measure(tree()) { true }.problem)
    }

    @Test fun theTreeIsCopiedOntoACardWhoseFilesMatch() {
        val root = tree()
        val seen = mutableListOf<Long>()
        val (image, result) = fill("card.frondsd", root) { done, total -> seen += done * 1000 / total }
        assertNull(result)
        assertTrue(seen.isNotEmpty() && seen.last() == 1000L)
        assertNotNull(CardFile.verify(image))
        reading(image) { fat ->
            assertEquals(listOf("A rather long file name for a card.txt", "empty", "Songs"), fat.list("/").map { it.name }.sortedBy { it.lowercase() })
            val album = fat.list("/Songs/Album One").associateBy { it.name }
            assertEquals(setOf("01 - A very long track title indeed.sunvox", "notes.txt"), album.keys)
            val out = ByteArrayOutputStream()
            fat.read(album.getValue("01 - A very long track title indeed.sunvox"), out)
            assertArrayEquals(pattern(3 shl 20), out.toByteArray())
            val nested = ByteArrayOutputStream()
            fat.read(fat.find("/Songs/Album Two/Deeper/Deepest/readme.md")!!, nested)
            assertEquals("nested", nested.toString())
            assertEquals(0L, fat.find("/empty")!!.size)
            assertTrue(fat.fatCopiesMatch())
            assertTrue(fat.check().isEmpty())
        }
    }

    @Test fun datesComeFromTheTree() {
        val dated = file("old.txt", "x".toByteArray()).apply { modified = 1_000_000_000_000L }
        val folder = dir("Old", file("in.txt", ByteArray(1))).apply { modified = 1_100_000_000_000L }
        val (image, result) = fill("dated.frondsd", dir("Top", dated, folder))
        assertNull(result)
        reading(image) { fat ->
            // FAT keeps local time to two seconds.
            assertTrue(kotlin.math.abs(fat.find("/old.txt")!!.modified - 1_000_000_000_000L) < 2000)
            assertTrue(kotlin.math.abs(fat.find("/Old")!!.modified - 1_100_000_000_000L) < 2000)
            assertTrue(fat.find("/Old/in.txt")!!.modified > 1_600_000_000_000L)
        }
    }

    @Test fun namesPastAsciiComeBackExactly() {
        val names = listOf("äpfel.txt", "ÄB.txt", "naïve café.txt", "日本語.md", "README.TXT")
        val root = dir("Names", *names.map { file(it, it.toByteArray()) }.toTypedArray(), dir("Ölfarben", file("ñ", "n".toByteArray())))
        val (image, result) = fill("names.frondsd", root) { _, _ -> }
        assertNull(result)
        reading(image) { fat ->
            assertEquals((names + "Ölfarben").sorted(), fat.list("/").map { it.name }.sorted())
            assertEquals(listOf("ñ"), fat.list("/Ölfarben").map { it.name })
        }
    }

    @Test fun aFolderThatDoesNotFitStopsTheCopyAndCancelStopsIt() {
        // Declared small enough for the plan, but the card is too full for it: the copy says so,
        // and nothing of it stays.
        val root = dir("Over", *Array(40) { file("part-$it.bin", ByteArray(1 shl 20)) })
        val plan = FolderCard.measure(root)
        assertFalse(FolderCard.fits(plan, 32))
        val (full, result) = fill("small.frondsd", root, plan)
        assertEquals("The folder doesn't fit on a card that size.", result)
        reading(full) { assertTrue(it.list("/").isEmpty()) }
        // Cancel while a file is read: rolled back too.
        var reads = 0
        val (again, stopped) = fill("again.frondsd", tree(), cancelled = { ++reads > 3 })
        assertEquals(COPY_CANCELLED, stopped)
        assertNotNull(CardFile.verify(again))
        reading(again) { assertTrue(it.list("/").isEmpty()) }
    }
}
