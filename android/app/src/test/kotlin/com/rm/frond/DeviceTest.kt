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
        assertNull(Device.ZIRE72.fastAudioNorMd5)
        assertFalse(Device.ZIRE72.fastAudio)
        assertTrue(Device.TX.fastAudio)
        assertTrue(Device.E2.fastAudio)
        assertTrue(Device.ZIRE72.camera)
        assertFalse(Device.E2.camera)
        assertTrue(Device.ZIRE72 in Device.shown)
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
