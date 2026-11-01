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
        assertEquals("This is a Zire 72 NOR image. Choose a Palm TX one.", Firmware.check(Device.TX, zire(), nand))
        assertEquals("This is a Zire 72 NOR image. Choose a Tungsten E2 one.", Firmware.check(Device.E2, zire(), null))
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
}
