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

    @Test fun otherFilesAreNotDetected() {
        assertNull(Firmware.detect(nor(8L shl 20)))
        assertNull(Firmware.detect(nor(8L shl 20, 0x20000, palm = false)))
        assertNull(Firmware.detect(nor(0)))
        val text = tmp.newFile().apply { writeText("not a Palm") }
        assertNull(Firmware.detect(text))
        assertNull(Firmware.detect(File(tmp.root, "missing.bin")))
    }
}
