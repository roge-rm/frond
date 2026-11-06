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
    fun unknownNamesAreRefused() {
        for (name in listOf(null, "", "3", "0", "tungsten-e2", "zire-72", "palm-zire72", "Palm-TX")) assertNull(Device.byId(name))
    }

    @Test
    fun coreGetsTheName() {
        assertEquals(listOf("--device", "palm-tx"), Device.TX.coreFlags)
        assertEquals(listOf("--device", "palm-e2"), Device.E2.coreFlags)
        assertEquals(listOf("--device", "palm-zire-72"), Device.ZIRE72.coreFlags)
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
        assertNull(Device.ZIRE72.fastAudioNorMd5)
        assertFalse(Device.ZIRE72.fastAudio)
        assertTrue(Device.TX.fastAudio)
        assertTrue(Device.E2.fastAudio)
        // Low-latency audio: TX and E2 (preview or queue only) and the TG50 (queue only).
        assertTrue(Device.TX.lowLatencyAudio && Device.E2.lowLatencyAudio)
        assertTrue(Device.byId("sony-clie-tg50")!!.lowLatencyAudio)
        assertFalse(Device.ZIRE72.lowLatencyAudio)
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
        assertEquals(2048L shl 20, Device.TX.sdImageMax)
        assertEquals(1024L shl 20, Device.E2.sdImageMax)
        assertEquals(2048L shl 20, Device.ZIRE72.sdImageMax)
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
        assertEquals(listOf(128, 64, 32, 16, 8, 4, 256, 512, 1024, 1886), tg50.memoryStickSizes)
        assertEquals(emptyList<Int>(), Device.TX.memoryStickSizes)
        assertEquals(listOf("128 MB", "4 MB"), listOf(128, 4).map { Device.sizeLabel(it) })
        assertEquals(4_268_032L, Device.memoryStickPhysicalBytes(4))
        assertEquals(136_577_024L, Device.memoryStickPhysicalBytes(128))
    }
}

class FastAudioQualifiedTest {
    private fun nor(content: String) = java.io.File.createTempFile("nor", ".bin").apply { writeText(content); deleteOnExit() }

    @Test
    fun zireNeverQualifies() {
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
