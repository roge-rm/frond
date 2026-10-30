package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import kotlin.random.Random

class FatImageTest {
    // The same layout as the core's frondSd::create (native/SdImage.h).
    private fun blank(mib: Int): File {
        val image = ByteArray(mib shl 20)
        fun put16(at: Int, v: Int) { image[at] = v.toByte(); image[at + 1] = (v shr 8).toByte() }
        fun put32(at: Int, v: Int) { put16(at, v and 0xffff); put16(at + 2, v ushr 16) }
        val start = 63
        val sectors = image.size / 512 - start
        val cluster = if (mib > 64) 8 else 4
        var fatSectors = 1
        while (true) {
            val clusters = (sectors - 1 - 32 - 2 * fatSectors) / cluster
            val required = ((clusters + 2) * 2 + 511) / 512
            if (fatSectors >= required) break
            fatSectors = required
        }
        image[446 + 1] = 1; image[446 + 2] = 1; image[446 + 4] = 6
        put32(446 + 8, start); put32(446 + 12, sectors)
        image[510] = 0x55; image[511] = 0xaa.toByte()
        val b = start * 512
        image[b] = 0xeb.toByte(); image[b + 1] = 0x3c; image[b + 2] = 0x90.toByte()
        "FROND001".toByteArray().copyInto(image, b + 3)
        put16(b + 11, 512); image[b + 13] = cluster.toByte(); put16(b + 14, 1)
        image[b + 16] = 2; put16(b + 17, 512)
        if (sectors < 65536) put16(b + 19, sectors) else put32(b + 32, sectors)
        image[b + 21] = 0xf8.toByte(); put16(b + 22, fatSectors); put16(b + 24, 63); put16(b + 26, 255)
        put32(b + 28, start); image[b + 36] = 0x80.toByte(); image[b + 38] = 0x29
        "FROND      ".toByteArray().copyInto(image, b + 43)
        "FAT16   ".toByteArray().copyInto(image, b + 54)
        image[b + 510] = 0x55; image[b + 511] = 0xaa.toByte()
        for (f in 0 until 2) {
            val d = (start + 1 + f * fatSectors) * 512
            put16(d, 0xfff8); put16(d + 2, 0xffff)
        }
        val root = (start + 1 + 2 * fatSectors) * 512
        "FROND      ".toByteArray().copyInto(image, root)
        image[root + 11] = 8
        val out = File(System.getProperty("frond.fatTestDir") ?: "build/fat-test", "card-$mib.img")
        out.parentFile!!.mkdirs()
        out.writeBytes(image)
        return out
    }

    private fun FatImage.put(folder: String, name: String, data: ByteArray) = add(folder, name, data.inputStream(), data.size.toLong())

    private fun FatImage.get(path: String): ByteArray {
        val out = ByteArrayOutputStream()
        read(find(path)!!, out)
        return out.toByteArray()
    }

    @Test
    fun roundTrip() {
        val file = blank(32)
        val random = Random(1)
        val big = random.nextBytes(3_000_001)
        val small = random.nextBytes(100)
        FatImage.open(file, true).use { fat ->
            assertTrue(fat.list("/").isEmpty())
            fat.makeFolder("/", "PALM")
            fat.makeFolder("/PALM", "Launcher")
            fat.put("/PALM/Launcher", "SunVox.prc", big)
            fat.put("/", "README.TXT", small)
            fat.put("/", "a song with spaces.sunvox", small)
            fat.put("/", "empty", ByteArray(0))
            // Enough files to grow a subfolder past one cluster (2 KiB, 64 slots).
            for (i in 0 until 60) fat.put("/PALM", "Long file name number $i.pdb", byteArrayOf(i.toByte()))
        }
        FatImage.open(file, false).use { fat ->
            assertEquals(listOf("PALM", "a song with spaces.sunvox", "empty", "README.TXT"), fat.list("/").map { it.name })
            assertArrayEquals(big, fat.get("/palm/launcher/sunvox.prc"))
            assertArrayEquals(small, fat.get("/README.TXT"))
            assertArrayEquals(ByteArray(0), fat.get("/empty"))
            assertEquals(61, fat.list("/PALM").size)
            for (i in 0 until 60) assertArrayEquals(byteArrayOf(i.toByte()), fat.get("/PALM/Long file name number $i.pdb"))
            assertEquals("SUNVOX.PRC", fat.find("/PALM/Launcher/SunVox.prc")!!.shortName)
            assertEquals("ASONGW~1.SUN", fat.find("/a song with spaces.sunvox")!!.shortName)
        }
        FatImage.open(file, true).use { fat ->
            for (name in listOf("readme.txt", "ASONGW~1.SUN", "palm")) {
                try { fat.put("/", name, small); fail("$name taken") } catch (e: FatImage.FatException) {}
            }
            for (name in listOf("a:b", "x.", " x", "")) {
                try { fat.put("/", name, small); fail("$name allowed") } catch (e: FatImage.FatException) {}
            }
            try { fat.put("/", "huge", ByteArray(40 shl 20)); fail("fits") } catch (e: FatImage.FatException) {}
        }
    }

    @Test
    fun fillRoot() {
        val file = blank(128)
        FatImage.open(file, true).use { fat ->
            try {
                for (i in 0 until 600) fat.put("/", "F$i", byteArrayOf(1))
                fail("root never filled")
            } catch (e: FatImage.FatException) {
                assertTrue(e.message!!.contains("full"))
            }
            assertEquals(511, fat.list("/").size)
        }
    }
}
