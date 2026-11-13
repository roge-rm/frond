package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertNull
import org.junit.Test

class DeviceTest {
    @Test
    fun namesMatchTheCoreTable() {
        assertEquals("palm-tx", Device.TX.id)
        assertEquals("palm-e2", Device.E2.id)
        assertEquals("palm-zire-72", Device.ZIRE72.id)
        for (device in Device.entries) assertEquals(device, Device.byId(device.id))
    }

    @Test
    fun onlyTheZodiacIsTaggedWorkInProgress() {
        for (device in Device.entries) {
            val tagged = device.id == Device.ZODIAC_ID
            assertEquals(device.id, tagged, device.experimental)
            assertEquals(device.id, if (tagged) "${device.label} · Work in progress" else device.label, device.listLabel)
        }
        assertEquals("Tapwave Zodiac 2 · Work in progress", Device.byId(Device.ZODIAC_ID)!!.listLabel)
    }

    @Test
    fun unknownNamesAreRefused() {
        for (name in listOf(null, "", "3", "0", "tungsten-e2", "zire-72", "palm-zire72", "Palm-TX")) assertNull(Device.byId(name))
    }

    @Test
    fun coreGetsTheName() {
        assertEquals(listOf("--device", "palm-tx"), Device.TX.coreFlags)
        assertEquals(listOf("--device", "palm-e2"), Device.E2.coreFlags)
        assertEquals(listOf("--device", "palm-zire-72"), Device.ZIRE72.coreFlags)
    }

    @Test fun e2DenvfsKeepsRetailDefaultsAndDeviceIdentity() {
        assertEquals(8L shl 20, Device.E2.norSize)
        assertEquals(34_603_008L, Device.E2.nandSize)
        assertFalse(Device.E2.nandRequired)
        val variant = Device.E2.firmwareVariants.single()
        assertEquals("e2-denvfs", variant.id)
        assertEquals(6L shl 20, variant.norSize)
        assertEquals(0x20000L, variant.headerOffset)
        assertEquals("rePalm", variant.cardName)
        assertEquals("Dmitry.GR", variant.manufacturer)
        assertEquals(Device.E2.companyId, variant.companyId)
        assertEquals(Device.E2.halId, variant.halId)
        assertTrue(variant.cardEndsAtEof)
        assertEquals("c2d90f775b1737de5bcd11634bf51d7d", variant.fastAudioNorMd5)
        for (device in Device.entries.filter { it != Device.E2 }) assertTrue(device.firmwareVariants.isEmpty())
        assertEquals(listOf("--device", "palm-e2"), Device.E2.coreFlags)
    }

    @Test fun e3KeepsTheExistingIdentityAndUsesTheWholeScreen() {
        val e3 = Device.byId("palm-e2-franken")!!
        assertEquals("Tungsten E3", e3.label)
        assertEquals(listOf("--device", "palm-e2-franken"), e3.coreFlags)
        assertEquals(8L shl 20, e3.norSize)
        assertEquals(34_603_008L, e3.nandSize)
        assertFalse(e3.nandRequired)
        assertEquals(320, e3.width)
        assertEquals(480, e3.height)
        assertEquals(0, e3.silkscreenHeight)
        assertEquals(Device.E2.sdSizes, e3.sdSizes)
        assertEquals(Device.E2.buttons.filter { it != "power" }, e3.buttons)
        assertFalse("power" in e3.buttons)
        assertFalse(PalmButton.POWER in PalmButton.on(e3))
        assertTrue(e3.sound && e3.sdSlot && e3.serialPort && e3.hotSyncButton)
        assertTrue(e3.keyboard && e3.usb && e3.infrared)
        assertFalse(e3.bluetooth || e3.irPhone || e3.network || e3.microphone || e3.camera)
        assertTrue(e3.fastAudio)
        assertEquals("a79f98bddd50c61e7eb89eee46eb81bd", e3.fastAudioNorMd5)
        assertFalse(e3.fastAudioNorMd5 == Device.E2.fastAudioNorMd5)
        assertTrue(e3.lowLatencyAudio)
        assertFalse(Device.byId("palm-iiix")!!.lowLatencyAudio)
        assertFalse(e3.serialAbsent || e3.runs68k)
        assertTrue(e3 in Device.shown)
    }

    // Sizes and capabilities from the core's deviceProfiles5 entry for the Zire 72.
    @Test
    fun zireIsNorOnlyWithNoNetworkOrSerial() {
        assertEquals(14_942_208L, Device.ZIRE72.norSize)
        assertFalse(Device.ZIRE72.hasNand)
        assertFalse(Device.ZIRE72.nandRequired)
        assertFalse(Device.ZIRE72.network)
        assertFalse(Device.ZIRE72.serialPort)
        assertTrue(Device.ZIRE72.serialAbsent && !Device.ZIRE72.hotSyncButton)
        assertTrue(Device.E2.serialPort && Device.E2.hotSyncButton && !Device.E2.serialAbsent)
        assertTrue(Device.TX.hotSyncButton && !Device.byId("sony-clie-tg50")!!.hotSyncButton)
        assertEquals("0f3e0bf04ceaa5ad73a1768b7d9ede98", Device.ZIRE72.fastAudioNorMd5)
        assertTrue(Device.ZIRE72.fastAudio)
        assertTrue(Device.TX.fastAudio)
        assertTrue(Device.E2.fastAudio)
        // Low-latency audio: TX, E2 and Zire 72 (preview or queue only) and the TG50 (queue only).
        assertTrue(Device.TX.lowLatencyAudio && Device.E2.lowLatencyAudio)
        assertTrue(Device.byId("sony-clie-tg50")!!.lowLatencyAudio)
        assertTrue(Device.ZIRE72.lowLatencyAudio)
        assertTrue(Device.ZIRE72.camera)
        assertFalse(Device.E2.camera)
        assertTrue(Device.ZIRE72 in Device.shown)
    }

    // The microphone row, permission request and lifetime rules key on this flag.
    @Test
    fun microphoneOnTheZireAndTg50Only() {
        assertTrue(Device.ZIRE72.microphone)
        assertTrue(Device.byId("sony-clie-tg50")!!.microphone)
        assertFalse(Device.TX.microphone)
        assertFalse(Device.E2.microphone)
    }
}

class SdBoundsTest {
    // From the core's profiles: TX and Zire 72 take 32 MB to 2 GB (new cards 1886 MiB), the E2 up to 1 GB, the TG50 no SD.
    @Test fun sizesFollowTheProfile() {
        assertEquals(listOf(32, 64, 128, 256, 512, 1024, 1886), Device.TX.sdSizes)
        assertEquals(listOf(32, 64, 128, 256, 512, 1024, 1886), Device.ZIRE72.sdSizes)
        assertEquals(listOf(32, 64, 128, 256, 512, 1024), Device.E2.sdSizes)
        assertEquals(emptyList<Int>(), Device.byId("sony-clie-tg50")!!.sdSizes)
    }

    @Test fun labels() {
        assertEquals("32 MB", Device.sizeLabel(32))
        assertEquals("512 MB", Device.sizeLabel(512))
        assertEquals("1 GB", Device.sizeLabel(1024))
        assertEquals("2 GB", Device.sizeLabel(1886))
        assertEquals("1 GB", Device.sizeLabel(1L shl 30))
    }
}

class MemoryStickSizesTest {
    // The TG50 takes the six classic sizes, offered 128 MB first; SD devices have none.
    @Test fun sizesFollowTheProfile() {
        val tg50 = Device.byId("sony-clie-tg50")!!
        assertEquals(listOf(4, 8, 16, 32, 64, 128), tg50.msSizes)
        assertEquals(listOf("128 MB", "4 MB"), listOf(128, 4).map { Device.sizeLabel(it) })
        assertEquals(4_268_032L, Device.memoryStickPhysicalBytes(4))
        assertEquals(136_577_024L, Device.memoryStickPhysicalBytes(128))
    }
}

class FastAudioQualifiedTest {
    private fun nor(content: String) = java.io.File.createTempFile("nor", ".bin").apply { writeText(content); deleteOnExit() }

    private fun session(firmware: java.io.File, version: Int): java.io.File = nor("").also { session ->
        java.io.DataOutputStream(session.outputStream()).use { out ->
            fun le32(n: Int) = out.writeInt(Integer.reverseBytes(n))
            fun le64(n: Long) { le32(n.toInt()); le32((n shr 32).toInt()) }
            le32(0x19800819); le32(version)
            out.write("palm-e2".toByteArray().copyOf(32))
            le32(0); le32(0)
            if (version >= 7) {
                for (n in listOf(0L, firmware.length(), 0L, 0L, 0L)) le64(n)
                le32(0)
            } else {
                for (n in listOf(0, firmware.length().toInt(), 0, 0, 0, 0)) le32(n)
            }
            java.util.zip.DeflaterOutputStream(out).use { it.write(firmware.readBytes()) }
        }
    }

    private fun checkDenvfsAudioOptions(device: Device, firmware: java.io.File) {
        val settings = MachineSettings(fastAudio = true)
        fun arguments(session: java.io.File? = null) = settings.audioArguments(device.lowLatencyAudio, 512) {
            Firmware.fastAudioQualified(device, session, firmware)
        }
        val fast = listOf("--fast-audio", "--audio-period", "512")
        val queue = listOf("--low-latency-queue", "--audio-period", "512")
        assertEquals(fast, arguments())
        val sessions = (6..8).map { session(firmware, it) }
        for (saved in sessions) assertEquals(fast, arguments(saved))
        var original = 0
        java.io.RandomAccessFile(firmware, "rw").use {
            original = it.readUnsignedByte()
            it.seek(0); it.writeByte(original xor 1)
        }
        assertEquals(queue, arguments())
        for (saved in sessions) assertEquals(fast, arguments(saved))
        val changedSession = session(firmware, 8)
        java.io.RandomAccessFile(firmware, "rw").use { it.writeByte(original) }
        assertEquals(fast, arguments())
        assertEquals(queue, arguments(changedSession))
    }

    @Test fun variantFingerprintUsesSixMibRawOrEmbeddedNor() {
        val firmware = nor("synthetic de-NVFS NOR")
        java.io.RandomAccessFile(firmware, "rw").use { it.setLength(6L shl 20) }
        val variant = Device.E2.firmwareVariants.single().copy(fastAudioNorMd5 = Firmware.md5(firmware))
        val device = Device.E2.copy(firmwareVariants = listOf(variant))
        assertTrue(device.copy(fastAudioNorMd5 = null).fastAudio)
        checkDenvfsAudioOptions(device, firmware)
    }

    @Test fun qualifiedDenvfsFirmwareSelectsPreview() {
        val source = System.getenv("FROND_TEST_E2_DENVFS_NOR")
        org.junit.Assume.assumeTrue("FROND_TEST_E2_DENVFS_NOR not set", !source.isNullOrEmpty())
        val original = java.io.File(source!!)
        assertEquals(6L shl 20, original.length())
        assertEquals("c2d90f775b1737de5bcd11634bf51d7d", Firmware.md5(original))
        val copy = nor("")
        original.copyTo(copy, overwrite = true)
        checkDenvfsAudioOptions(Device.E2, copy)
        assertEquals("c2d90f775b1737de5bcd11634bf51d7d", Firmware.md5(original))
    }

    @Test fun e3FingerprintSelectsPreviewAndChangedNorUsesTheQueue() {
        val firmware = nor("synthetic E3 NOR")
        val e3 = Device.byId("palm-e2-franken")!!.copy(fastAudioNorMd5 = Firmware.md5(firmware))
        val settings = MachineSettings(fastAudio = true)
        fun arguments() = settings.audioArguments(e3.lowLatencyAudio, 1024) {
            Firmware.fastAudioQualified(e3, null, firmware)
        }
        assertTrue(Firmware.fastAudioQualified(e3, null, firmware))
        assertEquals(listOf("--fast-audio", "--audio-period", "1024"), arguments())
        firmware.appendText("changed")
        assertFalse(Firmware.fastAudioQualified(e3, null, firmware))
        assertEquals(listOf("--low-latency-queue", "--audio-period", "1024"), arguments())
    }

    @Test
    fun otherZireNorDoesNotQualify() {
        assertFalse(Firmware.fastAudioQualified(Device.ZIRE72, null, nor("x")))
    }

    @Test
    fun otherNorDoesNotQualify() {
        assertFalse(Firmware.fastAudioQualified(Device.TX, null, nor("not the qualified image")))
    }

    @Test
    fun missingNorDoesNotQualify() {
        assertFalse(Firmware.fastAudioQualified(Device.E2, null, java.io.File("/nonexistent/nor.bin")))
    }
}
