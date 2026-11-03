package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import kotlin.random.Random

class FatImageTest {
    // The same layout as the core's frondSd::layout (native/SdImage.h), written sparse.
    private fun blank(mib: Int): File {
        val start = 63L
        val total = (mib.toLong() shl 20) / 512
        val sectors = total - start
        val cluster = when { mib <= 64 -> 4; mib <= 256 -> 8; mib == 512 -> 16; mib == 1024 -> 32; else -> 64 }
        var fatSectors = 1L
        var reserved = 1L
        var clusters: Long
        while (true) {
            clusters = minOf((sectors - reserved - 32 - 2 * fatSectors) / cluster, 65524L)
            val required = ((clusters + 2) * 2 + 511) / 512
            if (fatSectors >= required) break
            fatSectors = required
        }
        if (mib > 128) reserved = total - clusters * cluster - start - 32 - 2 * fatSectors
        fun sector() = ByteArray(512)
        fun put16(b: ByteArray, at: Int, v: Long) { b[at] = v.toByte(); b[at + 1] = (v shr 8).toByte() }
        fun put32(b: ByteArray, at: Int, v: Long) { put16(b, at, v and 0xffff); put16(b, at + 2, v ushr 16) }
        val mbr = sector()
        mbr[446 + 1] = 1; mbr[446 + 2] = 1; mbr[446 + 4] = 6
        put32(mbr, 446 + 8, start); put32(mbr, 446 + 12, sectors)
        mbr[510] = 0x55; mbr[511] = 0xaa.toByte()
        val boot = sector()
        boot[0] = 0xeb.toByte(); boot[1] = 0x3c; boot[2] = 0x90.toByte()
        "FROND001".toByteArray().copyInto(boot, 3)
        put16(boot, 11, 512); boot[13] = cluster.toByte(); put16(boot, 14, reserved)
        boot[16] = 2; put16(boot, 17, 512)
        if (sectors < 65536) put16(boot, 19, sectors) else put32(boot, 32, sectors)
        boot[21] = 0xf8.toByte(); put16(boot, 22, fatSectors); put16(boot, 24, 63); put16(boot, 26, 255)
        put32(boot, 28, start); boot[36] = 0x80.toByte(); boot[38] = 0x29
        "FROND      ".toByteArray().copyInto(boot, 43)
        "FAT16   ".toByteArray().copyInto(boot, 54)
        boot[510] = 0x55; boot[511] = 0xaa.toByte()
        val fat = sector().also { put16(it, 0, 0xfff8); put16(it, 2, 0xffff) }
        val root = sector().also { "FROND      ".toByteArray().copyInto(it); it[11] = 8 }
        val out = File(System.getProperty("frond.fatTestDir") ?: "build/fat-test", "card-$mib.img")
        out.parentFile!!.mkdirs()
        out.delete()
        RandomAccessFile(out, "rw").use { f ->
            f.setLength(mib.toLong() shl 20)
            for ((index, bytes) in listOf(0L to mbr, start to boot, start + reserved to fat, start + reserved + fatSectors to fat,
                    start + reserved + 2 * fatSectors to root)) {
                f.seek(index * 512); f.write(bytes)
            }
        }
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

    // Larger cards: bigger clusters, a data area that ends at the last sector, files at both ends.
    private fun largeCard(mib: Int, clusterBytes: Int) {
        val file = blank(mib)
        try {
            val random = Random(mib)
            val first = random.nextBytes(1_000_003)
            val small = random.nextBytes(77)
            FatImage.open(file, true).use { fat ->
                assertEquals(clusterBytes, fat.clusterBytes)
                assertTrue(fat.totalBytes in (mib.toLong() shl 20) * 95 / 100..(mib.toLong() shl 20))
                assertEquals(fat.totalBytes, fat.freeBytes)
                fat.makeFolder("/", "PALM")
                fat.put("/PALM", "first.bin", first)
                fat.put("/", "small.txt", small)
                for (i in 0 until 40) fat.put("/PALM", "File number $i.pdb", byteArrayOf(i.toByte()))
            }
            FatImage.open(file, false).use { fat ->
                assertArrayEquals(first, fat.get("/PALM/first.bin"))
                assertArrayEquals(small, fat.get("/small.txt"))
                assertEquals(41, fat.list("/PALM").size)
                for (i in 0 until 40) assertArrayEquals(byteArrayOf(i.toByte()), fat.get("/PALM/File number $i.pdb"))
                val clustersUsed = (1_000_003 + clusterBytes - 1) / clusterBytes + 1 + 40 + 1
                assertTrue(fat.totalBytes - fat.freeBytes >= clustersUsed.toLong() * clusterBytes - clusterBytes)
            }
            assertEquals(mib.toLong() shl 20, file.length())
        } finally {
            file.delete()
        }
    }

    @Test fun card256() = largeCard(256, 8 * 512)

    @Test fun card1886() = largeCard(1886, 64 * 512)

    @Test fun card2048() = largeCard(2048, 64 * 512)

    @Test fun tooLargeRefused() {
        val file = File(System.getProperty("frond.fatTestDir") ?: "build/fat-test", "card-4096.img")
        file.parentFile!!.mkdirs()
        RandomAccessFile(file, "rw").use { it.setLength(4096L shl 20) }
        try {
            FatImage.open(file, false).use { }
            fail("4 GB card opened")
        } catch (e: FatImage.FatException) {
            assertEquals("Use a 32 MB to 2 GB FAT16 card image.", e.message)
        } finally {
            file.delete()
        }
    }
}
