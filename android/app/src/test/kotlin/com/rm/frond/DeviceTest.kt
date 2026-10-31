package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DeviceTest {
    @Test
    fun namesMatchTheCoreTable() {
        assertEquals("palm-tx", Device.TX.id)
        assertEquals("palm-e2", Device.E2.id)
        for (device in Device.entries) assertEquals(device, Device.byId(device.id))
    }

    @Test
    fun unknownNamesAreRefused() {
        for (name in listOf(null, "", "3", "0", "tungsten-e2", "palm-zire-72", "Palm-TX")) assertNull(Device.byId(name))
    }

    @Test
    fun coreGetsTheName() {
        assertEquals(listOf("--device", "palm-tx"), Device.TX.coreFlags)
        assertEquals(listOf("--device", "palm-e2"), Device.E2.coreFlags)
    }
}
