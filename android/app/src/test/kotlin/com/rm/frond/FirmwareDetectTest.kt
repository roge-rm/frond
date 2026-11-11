package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile

class FirmwareDetectTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun le32(file: RandomAccessFile, at: Long, value: Long) {
        file.seek(at)
        file.write(byteArrayOf(value.toByte(), (value shr 8).toByte(), (value shr 16).toByte(), (value shr 24).toByte()))
    }

    // A NOR of the given size with a Palm card header at headerAt, or none.
    private fun nor(size: Long, headerAt: Long? = null, palm: Boolean = true): File {
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            if (headerAt != null) {
                le32(file, headerAt + 8, 0xfeedbeefL)
                le32(file, headerAt + 0x80, if (palm) 0x50616c6dL else 0x48616e64L)
                le32(file, headerAt + 0x84, 0x68737072L)
            }
        }
        return f
    }

    @Test fun e2HeaderIsE2() {
        val f = nor(8L shl 20, 0x20000)
        assertEquals(Firmware.Header.E2, Firmware.header(f))
        assertEquals(Device.E2, Firmware.detect(f))
    }

    @Test fun e2HeaderInTheWrongPlaceIsStillE2() {
        val f = nor(8L shl 20, 0x40000)
        assertEquals(Firmware.Header.E2_WRONG_PLACE, Firmware.header(f))
        assertEquals(Device.E2, Firmware.detect(f))
        // The check then says what's wrong with it.
        assertEquals("This E2 NOR image doesn't have its header at 0x20000.", Firmware.check(Device.E2, f, null))
    }

    @Test fun sixteenMibWithoutE2HeaderIsTx() {
        assertEquals(Device.TX, Firmware.detect(nor(16L shl 20)))
        assertEquals(Device.TX, Firmware.detect(nor(16L shl 20, 0, palm = false)))
    }

    @Test fun e2HeaderWinsOverTxSize() {
        assertEquals(Device.E2, Firmware.detect(nor(16L shl 20, 0x20000)))
    }

    @Test fun e2DenvfsRequiresFullIdentity() {
        fun denvfs(size: Long = 6L shl 20, at: Long = 0x20000): File = nor(size, at).also { f ->
            RandomAccessFile(f, "rw").use { file ->
                file.seek(at + 0x10); file.write("rePalm\u0000".toByteArray(Charsets.US_ASCII))
                file.seek(at + 0x30); file.write("Dmitry.GR\u0000".toByteArray(Charsets.US_ASCII))
                le32(file, at + 0x6c, size - at)
            }
        }
        val f = denvfs()
        assertEquals(Device.E2, Firmware.detect(f))
        assertEquals("e2-denvfs", Firmware.inspect(f).variant?.id)
        assertNull(Firmware.check(Device.E2, f, null))
        org.junit.Assert.assertNotNull(Firmware.check(Device.TX, f, null))
        org.junit.Assert.assertNotNull(Firmware.check(Device.byId("palm-e2-franken")!!, f, null))
        val nand = tmp.newFile()
        RandomAccessFile(nand, "rw").use { it.setLength(Device.E2.nandSize) }
        assertNull(Firmware.check(Device.E2, f, nand))
        RandomAccessFile(nand, "rw").use { it.setLength(Device.E2.nandSize - 528) }
        org.junit.Assert.assertNotNull(Firmware.check(Device.E2, f, nand))
        for (offset in listOf(0x10L, 0x16L, 0x30L, 0x39L, 0x6cL, 0x80L, 0x84L)) {
            val wrong = denvfs()
            RandomAccessFile(wrong, "rw").use { it.seek(0x20000 + offset); it.writeByte(120) }
            assertNull(Firmware.inspect(wrong).variant)
            org.junit.Assert.assertNotNull(Firmware.check(Device.E2, wrong, null))
        }
        val e3 = Device.byId("palm-e2-franken")!!
        assertEquals("This is a Tungsten E2 NOR image. Choose Tungsten E2.", Firmware.check(e3, f, null))
        for (wrong in listOf(denvfs((6L shl 20) - 1), denvfs(at = 0x40000), nor(6L shl 20, 0x20000))) {
            assertNull(Firmware.inspect(wrong).variant)
            org.junit.Assert.assertNotNull(Firmware.check(Device.E2, wrong, null))
        }
    }

    @Test fun e3DetectionAndImportChecks() {
        val e3 = Device.byId("palm-e2-franken")!!
        fun e3Nor(size: Long = e3.norSize, at: Long = e3.headerOffset): File = nor(size, at).also { f ->
            RandomAccessFile(f, "rw").use { le32(it, at + 0x84, e3.halId) }
        }
        val f = e3Nor()
        assertEquals(e3, Firmware.detect(f))
        assertNull(Firmware.check(e3, f, null))
        val nand = tmp.newFile()
        RandomAccessFile(nand, "rw").use { it.setLength(e3.nandSize) }
        assertNull(Firmware.check(e3, f, nand))
        RandomAccessFile(nand, "rw").use { it.setLength(e3.nandSize - 528) }
        org.junit.Assert.assertNotNull(Firmware.check(e3, f, nand))
        org.junit.Assert.assertNotNull(Firmware.check(e3, e3Nor(16L shl 20), null))
        org.junit.Assert.assertNotNull(Firmware.check(e3, e3Nor(at = 0x40000), null))
        org.junit.Assert.assertNotNull(Firmware.check(e3, nor(e3.norSize), null))
        org.junit.Assert.assertNotNull(Firmware.check(e3, nor(e3.norSize, 0x20000), null))
        assertEquals("This is a Tungsten E3 NOR image. Choose Tungsten E3.", Firmware.check(Device.E2, f, null))
        assertEquals("This is a Tungsten E3 NOR image. Choose Tungsten E3.", Firmware.check(Device.TX, f, null))
        assertEquals("This is a Tungsten E2 NOR image. Choose Tungsten E2.", Firmware.check(e3, nor(16L shl 20, 0x20000), null))
    }

    private fun card(file: RandomAccessFile, at: Long, length: Long) {
        le32(file, at + 8, 0xfeedbeefL)
        file.seek(at + 0x10)
        file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
        le32(file, at + 0x6c, length)
        le32(file, at + 0x80, 0x50616c6dL)
        le32(file, at + 0x84, 0x615a3732L)
    }

    // A Zire 72 NOR: a small card at 0 and a big card at 0x20000 to the end.
    private fun zire(size: Long = Device.ZIRE72.norSize, big: Boolean = true): File {
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            card(file, 0, 0x20000)
            if (big) card(file, 0x20000, size - 0x20000)
        }
        return f
    }

    @Test fun zireCardsAreZire() {
        val f = zire()
        assertEquals(Firmware.Header.ZIRE72, Firmware.header(f))
        assertEquals(Device.ZIRE72, Firmware.detect(f))
        assertNull(Firmware.check(Device.ZIRE72, f, null))
    }

    @Test fun zireWithoutItsBigCardIsRefused() {
        val f = zire(big = false)
        assertEquals(Firmware.Header.ZIRE72_BAD_LAYOUT, Firmware.header(f))
        assertEquals(Device.ZIRE72, Firmware.detect(f))
        assertEquals("This Zire 72 NOR image doesn't have a small card at 0x0 and a big card at 0x20000.",
            Firmware.check(Device.ZIRE72, f, null))
    }

    @Test fun zireChecks() {
        val nand = tmp.newFile().apply { writeBytes(ByteArray(16)) }
        assertEquals("The Zire 72 has no NAND. Leave the NAND image empty.", Firmware.check(Device.ZIRE72, zire(), nand))
        assertEquals("This is a Zire 72 NOR image. Choose Zire 72.", Firmware.check(Device.TX, zire(), nand))
        assertEquals("This is a Zire 72 NOR image. Choose Zire 72.", Firmware.check(Device.E2, zire(), null))
        assertEquals("Choose a 14,942,208-byte NOR image for the Zire 72.", Firmware.check(Device.ZIRE72, nor(8L shl 20, 0x20000), null))
        assertEquals("Choose a 14,942,208-byte NOR image for the Zire 72.", Firmware.check(Device.ZIRE72, nor(16L shl 20), null))
        // The big card runs to the end, so a wrong size is reported before the header.
        assertEquals("Choose a 14,942,208-byte NOR image for the Zire 72.",
            Firmware.check(Device.ZIRE72, zire(16L shl 20, big = false), null))
    }

    @Test fun otherFilesAreNotDetected() {
        assertNull(Firmware.detect(nor(8L shl 20)))
        assertNull(Firmware.detect(nor(8L shl 20, 0x20000, palm = false)))
        assertNull(Firmware.detect(nor(0)))
        val text = tmp.newFile().apply { writeText("not a Palm") }
        assertNull(Firmware.detect(text))
        assertNull(Firmware.detect(File(tmp.root, "missing.bin")))
    }
    @Test fun profileHeaderAndLayoutDoNotRequireADeviceBranch() {
        val fixture = Device.E2.copy(id = "test-fixture", label = "Fixture", halId = 0x12345678L, headerOffset = 0x40000)
        val f = nor(fixture.norSize, 0x40000)
        RandomAccessFile(f, "rw").use { le32(it, 0x40084, fixture.halId) }
        val info = Firmware.inspect(f, listOf(fixture))
        assertEquals(fixture, info.device)
        assertEquals(true, info.validLayout)
        assertNull(Firmware.check(fixture, f, null))
        val wrongOffset = fixture.copy(headerOffset = 0x20000)
        assertEquals(false, Firmware.inspect(f, listOf(wrongOffset)).validLayout)
        assertEquals("This Fixture NOR image has an invalid card layout.", Firmware.check(wrongOffset, f, null))
    }


    // A Dana ROM's layout: a VZ small card at 0, a second card at 32 KiB, padding to 6 MiB.
    private fun dana(size: Long = 6L shl 20, mutate: (java.io.RandomAccessFile) -> Unit = {}): File {
        val dana = Device.byId("alphasmart-dana")!!
        fun be(file: RandomAccessFile, at: Long, value: Long, bytes: Int) {
            file.seek(at)
            for (i in bytes - 1 downTo 0) file.write(((value shr (8 * i)) and 0xff).toInt())
        }
        fun card(file: RandomAccessFile, at: Long, length: Long) {
            file.seek(at + 0x10)
            file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
            be(file, at + 4, 0x10000000L + at + 0x2a2, 4)
            be(file, at + 8, 0xfeedbeefL, 4)
            be(file, at + 0x0c, 5, 2)
            be(file, at + 0x0e, 0x0040, 2)
            be(file, at + 0x6c, length, 4)
            be(file, at + 0x7e, dana.companyId, 4)
            be(file, at + 0x82, dana.halId, 4)
        }
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            card(file, 0, 0x8000)
            card(file, 0x8000, 0x200000)
            mutate(file)
        }
        return f
    }

    @Test fun danaRomIsAcceptedForTheDanaAndTheWireless() {
        val rom = dana()
        assertEquals(Device.byId("alphasmart-dana"), Firmware.detect(rom))
        assertNull(Firmware.check(Device.byId("alphasmart-dana")!!, rom, null))
        assertNull(Firmware.check(Device.byId("alphasmart-dana-wireless")!!, rom, null))
    }

    @Test fun danaRomWithABrokenSecondCardIsRefused() {
        val wrongSize = dana(5L shl 20)
        assertNull(Firmware.inspectRom68k(wrongSize).device)
        val noSecond = dana { it.seek(0x8000 + 8); it.write(ByteArray(4)) }
        assertNull(Firmware.inspectRom68k(noSecond).device)
        assertEquals("This ROM isn't one the AlphaSmart Dana can run.", Firmware.check(Device.byId("alphasmart-dana")!!, noSecond, null))
    }
}
