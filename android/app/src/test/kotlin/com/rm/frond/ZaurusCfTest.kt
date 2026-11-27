package com.rm.frond

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileDescriptor
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.file.Files
import java.util.zip.GZIPOutputStream
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ZaurusCfTest {
    @Test fun sizesRunFromTheStockFourToThirtyTwoGb() {
        assertEquals((4..32).toList(), ZaurusCf.sizes)
        assertEquals(4, ZaurusCf.DEFAULT_GB)
        assertEquals(4_095_737_856L, ZaurusCf.imageBytes(4))
        assertEquals(5_000_000_000L, ZaurusCf.imageBytes(5))
        assertEquals(32_000_000_000L, ZaurusCf.imageBytes(32))
        assertEquals(0L, ZaurusCf.imageBytes(3))
        assertEquals(0L, ZaurusCf.imageBytes(33))
        assertTrue(ZaurusCf.sizes.all { ZaurusCf.imageBytes(it) % 512 == 0L })
        assertEquals("16 GB", ZaurusCf.sizeName(16))
    }

    @Test fun picksAreReadyForTheirMode() {
        val nand = "content://n"
        assertFalse(ZaurusImages.Picks().ready)
        assertFalse(ZaurusImages.Picks(nand = nand).ready)
        assertTrue(ZaurusImages.Picks(nand = nand, cf = "content://c").ready)
        assertFalse(ZaurusImages.Picks(cf = "content://c").ready)
        // Creating needs the archive, not a CF image; the font source stays optional.
        assertFalse(ZaurusImages.Picks(nand = nand, cf = "content://c", create = true).ready)
        assertTrue(ZaurusImages.Picks(nand = nand, create = true, hdd1 = "content://h").ready)
        assertFalse(ZaurusImages.Picks(nand = nand, create = false, hdd1 = "content://h").ready)
        assertFalse(ZaurusImages.Picks(create = true, hdd1 = "content://h").ready)
    }

    @Test fun picksSurviveTheSavedDraft() {
        val picks = ZaurusImages.Picks("content://n", "content://c", "content://p", create = true, hdd1 = "content://h", sizeGb = 12)
        val json = picks.putInto(JSONObject().put("device", "sharp-zaurus-sl-c3000"))
        assertEquals("sharp-zaurus-sl-c3000", json.getString("device"))
        assertEquals(picks, ZaurusImages.Picks.from(JSONObject(json.toString())))
        assertEquals(ZaurusImages.Picks(), ZaurusImages.Picks.from(JSONObject()))
        // A size outside the choice comes back as the stock one.
        assertEquals(4, ZaurusImages.Picks.from(JSONObject().put("zaurusSizeGb", 64)).sizeGb)
        assertEquals(4, ZaurusImages.Picks.from(JSONObject().put("zaurusSizeGb", 2)).sizeGb)
    }

    @Test fun freeSpaceChecks() {
        val mb = 1L shl 20
        assertNull(ZaurusCf.shortOfSpace(1L shl 40, 200 * mb))
        assertNull(ZaurusCf.shortOfSpace(64 * mb, null))
        assertNotNull(ZaurusCf.shortOfSpace(63 * mb, null))
        val short = ZaurusCf.shortOfSpace(300 * mb, 100 * mb)
        assertEquals("Not enough free space to create the CF image. It needs about 381 MB.", short)
        assertNull(ZaurusCf.fillWarning(ZaurusCf.imageBytes(8), 8))
        assertEquals("Only 3.2 GB free here, so the card can't fill up.", ZaurusCf.fillWarning(3_200_000_000L, 4))
        assertNotNull(ZaurusCf.fillWarning(ZaurusCf.imageBytes(32) - 1, 32))
    }

    @Test fun buildsASparseImageFromAnArchive() = withDir { dir ->
        val archive = File(dir, "hdd1.tar.gz").apply { writeBytes(hdd1Archive(fonts = true)) }
        val image = File(dir, "cf.img")
        val seen = mutableListOf<Pair<Long, Long>>()
        build(archive, image, progress = { done, total -> seen += done to total })
        assertEquals(ZaurusCf.imageBytes(4), image.length())
        RandomAccessFile(image, "r").use { cf ->
            cf.seek(510)
            assertEquals(0x55, cf.read())
            assertEquals(0xaa, cf.read())
        }
        assertTrue(seen.isNotEmpty())
        assertEquals(archive.length() to archive.length(), seen.last())
    }

    @Test fun showsTheCoresMessage() = withDir { dir ->
        val archive = File(dir, "hdd1.tar.gz").apply { writeBytes(hdd1Archive(fonts = false)) }
        val error = assertThrows(IOException::class.java) { build(archive, File(dir, "cf.img")) }
        assertEquals("Direct boot needs the stock LC Fonts. Choose a CS0 dump or a complete HDD1 with LC Fonts.", error.message)
        // Without the font check the same archive builds.
        build(archive, File(dir, "cf2.img"), requireFonts = false)

        val junk = File(dir, "junk.bin").apply { writeBytes(ByteArray(4096) { 7 }) }
        assertTrue(assertThrows(IOException::class.java) { build(junk, File(dir, "cf3.img")) }.message!!.isNotEmpty())
    }

    @Test fun cancelStops() = withDir { dir ->
        val archive = File(dir, "hdd1.tar.gz").apply { writeBytes(hdd1Archive(fonts = true)) }
        assertThrows(kotlinx.coroutines.CancellationException::class.java) {
            build(archive, File(dir, "cf.img"), cancelled = { true })
        }
    }

    private fun build(archive: File, image: File, requireFonts: Boolean = true, cancelled: () -> Boolean = { false },
                      progress: (Long, Long) -> Unit = { _, _ -> }) {
        archive.inputStream().use { input ->
            RandomAccessFile(image, "rw").use { output ->
                ZaurusCf.buildFds(fd(input.fd), archive.length(), -1, 4, fd(output.fd), cancelled, progress, requireFonts)
            }
        }
    }

    private fun fd(descriptor: FileDescriptor): Int =
        FileDescriptor::class.java.getDeclaredField("fd").apply { isAccessible = true }.getInt(descriptor)

    private fun withDir(body: (File) -> Unit) {
        val dir = Files.createTempDirectory("zaurus-cf").toFile()
        try { body(dir) } finally { dir.deleteRecursively() }
    }

    // A small HDD1-shaped archive: hdd1 with its file list and a factory seed holding hdd2 and hdd3.
    private fun hdd1Archive(fonts: Boolean): ByteArray {
        val seed = gzip(tar {
            dir("hdd2"); dir("hdd2/etc"); file("hdd2/etc/passwd", "root:x:0:0::/home/root:/bin/sh\n".toByteArray())
            dir("hdd3"); dir("hdd3/Documents"); file("hdd3/Documents/readme.txt", "hello\r\n".toByteArray())
        })
        return gzip(tar {
            dir("hdd1"); dir("hdd1/.sys")
            file("hdd1/.sys/hdlist1.dat", "./usr\n".toByteArray())
            file("hdd1/.sys/hdimage2.tgz", seed)
            for (d in listOf("usr", "usr/QtPalmtop.rom", "usr/QtPalmtop.rom/lib", "usr/QtPalmtop.rom/lib/fonts")) dir("hdd1/$d")
            for (height in listOf(10, 12, 16, 18, 20, 24, 36))
                file("hdd1/usr/QtPalmtop.rom/lib/fonts/lcfont_${height * 10}_50_t10.qpf",
                    if (fonts) ByteArray(3000) { (it * height).toByte() } else ByteArray(0))
        })
    }

    private class Tar {
        val out = ByteArrayOutputStream()

        fun dir(name: String) = entry("$name/", '5', ByteArray(0), "755")
        fun file(name: String, data: ByteArray) = entry(name, '0', data, "644")

        private fun entry(name: String, type: Char, data: ByteArray, mode: String) {
            val h = ByteArray(512)
            fun put(at: Int, text: String) = text.toByteArray(Charsets.US_ASCII).copyInto(h, at)
            fun octal(at: Int, width: Int, value: Long) = put(at, value.toString(8).padStart(width - 1, '0'))
            put(0, name)
            put(100, mode.padStart(7, '0'))
            octal(108, 8, 0)
            octal(116, 8, 0)
            octal(124, 12, data.size.toLong())
            octal(136, 12, 1_100_000_000L)
            h[156] = type.code.toByte()
            put(257, "ustar\u000000")
            put(265, "root")
            put(297, "root")
            for (i in 148 until 156) h[i] = ' '.code.toByte()
            put(148, (h.sumOf { it.toInt() and 0xff }).toString(8).padStart(6, '0') + "\u0000 ")
            out.write(h)
            out.write(data)
            out.write(ByteArray((512 - data.size % 512) % 512))
        }

        fun bytes(): ByteArray {
            out.write(ByteArray(1024))
            return out.toByteArray()
        }
    }

    private fun tar(body: Tar.() -> Unit) = Tar().apply(body).bytes()

    private fun gzip(data: ByteArray) = ByteArrayOutputStream().also { GZIPOutputStream(it).use { z -> z.write(data) } }.toByteArray()
}
