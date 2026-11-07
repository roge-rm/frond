package com.rm.frond

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.DataOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.util.zip.DeflaterOutputStream

class FirmwarePowerTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun nor(variant: Boolean): File = tmp.newFile().also { file ->
        RandomAccessFile(file, "rw").use { out ->
            out.setLength((if (variant) 6L else 8L) shl 20)
            fun le32(at: Long, n: Long) { out.seek(at); out.writeInt(Integer.reverseBytes(n.toInt())) }
            le32(0x20008, 0xfeedbeefL)
            le32(0x20080, 0x50616c6dL); le32(0x20084, 0x68737072L)
            if (variant) {
                out.seek(0x20010); out.write("rePalm\u0000".toByteArray())
                out.seek(0x20030); out.write("Dmitry.GR\u0000".toByteArray())
                le32(0x2006c, file.length() - 0x20000)
            }
        }
    }

    private fun session(nor: File, version: Int, media: Boolean = false, name: String = "palm-e2", metadataBytes: Int = 16): File = tmp.newFile().also { file ->
        DataOutputStream(file.outputStream()).use { out ->
            fun le32(n: Int) = out.writeInt(Integer.reverseBytes(n))
            fun le64(n: Long) { le32(n.toInt()); le32((n shr 32).toInt()) }
            val metadata = ByteArray(metadataBytes)
            le32(0x19800819); le32(version); out.write(name.toByteArray().copyOf(32))
            le32(5); le32(32 shl 20)
            if (version >= 7) {
                for (n in listOf(metadata.size.toLong(), nor.length(), 0L, 0L, 0L)) le64(n)
                le32(if (media) 1 else 0)
                if (media) out.write(ByteArray(if (version == 7) 92 else 100))
            } else {
                for (n in listOf(metadata.size, nor.length().toInt(), 0, 0, 0, 0)) le32(n)
            }
            DeflaterOutputStream(out).use { zipped ->
                zipped.write(metadata)
                nor.inputStream().use { it.copyTo(zipped) }
            }
        }
    }

    private fun assertNoPower(device: Device) {
        assertEquals("palm-e2", device.id)
        assertEquals(Device.E2.label, device.label)
        assertFalse("power" in device.buttons)
        assertFalse(PalmButton.POWER in PalmButton.on(device))
        assertEquals(mapOf(19 to PalmButton.UP), ControllerMap.decode("97:power,19:up", device).keys)
    }

    @Test fun rawVariantDisablesPowerAndRetailKeepsIt() {
        val variant = nor(true)
        assertNoPower(Firmware.runningDevice(Device.E2, null, variant))
        assertTrue("power" in Firmware.runningDevice(Device.E2, null, nor(false)).buttons)
        assertTrue("power" in Device.E2.buttons)
        assertEquals(PalmButton.POWER, ControllerMap.decode("97:power", Device.E2).keys[97])
        RandomAccessFile(variant, "rw").use { it.seek(0x20030); it.writeByte(120) }
        assertNoPower(Firmware.runningDevice(Device.E2, null, variant))
    }

    @Test fun savedNorOverridesInstalledFirmwareForAllNamedVersions() {
        val variant = nor(true)
        val retail = nor(false)
        for (version in 6..8) for (media in if (version >= 7) listOf(false, true) else listOf(false)) {
            val saved = session(variant, version, media)
            assertNoPower(Firmware.runningDevice(Device.E2, saved, retail))
            assertEquals(Firmware.md5(variant), Sessions.norMd5(saved))
            assertTrue("power" in Firmware.runningDevice(Device.E2, session(retail, version, media), variant).buttons)
        }
        val folder = tmp.newFolder()
        session(variant, 6).copyTo(File(folder, "old-machine-name.frond"))
        assertNoPower(Firmware.runningDevice(Device.E2, folder, retail))
    }

    @Test fun unsupportedGeometryAndOversizedMetadataStayConservative() {
        val retail = nor(false)
        for (version in 6..8) {
            // A valid payload with metadata just over the frontend's limit must be refused.
            val oversized = session(retail, version, metadataBytes = (1 shl 20) + 1)
            assertNoPower(Firmware.runningDevice(Device.E2, oversized, retail))
            assertNull(Sessions.norMd5(oversized))
            for ((offset, value) in if (version == 6) listOf(48L to 0x7fffffffL, 52L to 7L) else
                listOf(48L to (2L shl 30), 56L to 7L, 64L to Long.MAX_VALUE, 72L to Long.MIN_VALUE, 88L to 0xffffffffL)) {
                val bad = session(retail, version)
                RandomAccessFile(bad, "rw").use { out ->
                    out.seek(offset)
                    if (version == 6 || offset == 88L) out.writeInt(Integer.reverseBytes(value.toInt()))
                    else out.writeLong(java.lang.Long.reverseBytes(value))
                }
                assertNoPower(Firmware.runningDevice(Device.E2, bad, retail))
            }
        }
        assertNoPower(Firmware.runningDevice(Device.E2, null, File(tmp.root, "missing")))
    }

    @Test fun emptyDeflateBlocksCannotExceedTheCompressedInputBudget() {
        val retail = nor(false)
        val valid = session(retail, 8)
        val bytes = valid.readBytes()
        val bad = tmp.newFile()
        val emptyBlocks = ByteArray(2 * ((retail.length() + 16).toInt()) + 65536 + 10)
        for (i in emptyBlocks.indices) emptyBlocks[i] = if (i % 5 >= 3) 0xff.toByte() else 0
        bad.outputStream().use { out ->
            out.write(bytes, 0, 94) // v8 header and zlib header.
            out.write(emptyBlocks, 0, emptyBlocks.size - emptyBlocks.size % 5)
            out.write(bytes, 94, bytes.size - 94)
        }
        assertNoPower(Firmware.runningDevice(Device.E2, bad, retail))
        assertNull(Sessions.norMd5(bad))
        assertTrue("power" in Firmware.runningDevice(Device.E2, valid, retail).buttons)
    }

    @Test fun wrongSessionIdentityAndUnrelatedDevicesKeepTheirCapabilities() {
        val variant = nor(true)
        assertNoPower(Firmware.runningDevice(Device.E2, session(variant, 8, name = "palm-tx"), variant))
        assertEquals(Device.TX, Firmware.runningDevice(Device.TX, null, variant))
        val e3 = Device.byId("palm-e2-franken")!!
        assertEquals(e3, Firmware.runningDevice(e3, null, variant))
        assertFalse("power" in e3.buttons)
    }
}
