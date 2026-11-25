package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DevicePickerTest {
    private fun ids(group: String) = DevicePicker.groups().first { it.first == group }.second.map { it.id }

    @Test fun groupsInOrder() {
        assertEquals(listOf("Palm OS 5", "Palm classic", "Sony CLIÉ", "Tapwave", "HP", "AlphaSmart", "IBM"), DevicePicker.groups().map { it.first })
    }

    @Test fun everyDeviceHasAGroupAndADate() {
        for (d in Device.entries) {
            assertTrue("${d.id} has no group", d.group in deviceGroups)
            assertTrue("${d.id} has no release date", d.released / 100 in 1998..2010 && d.released % 100 in 1..12)
        }
        assertEquals(Device.entries.size, DevicePicker.groups().sumOf { it.second.size })
    }

    @Test fun oldestFirst() {
        assertEquals(listOf("palm-zire-72", "palm-e2", "palm-e2-franken", "palm-tx"), ids("Palm OS 5"))
        assertEquals(listOf("tapwave-zodiac-1", "tapwave-zodiac-2"), ids("Tapwave"))
        assertEquals(listOf("sony-clie-t415", "sony-clie-tg50"), ids("Sony CLIÉ"))
        assertEquals(listOf("alphasmart-dana", "alphasmart-dana-wireless"), ids("AlphaSmart"))
        assertEquals(
            listOf("palm-iiix", "palm-v", "palm-iiie", "palm-vx", "palm-iiixe", "palm-iiic", "palm-m100", "palm-m105", "palm-m500",
                "palm-m505", "palm-m125", "palm-m515", "palm-m130", "palm-m520"),
            ids("Palm classic"),
        )
    }

    @Test fun recentKeepsThreeNewestFirst() {
        var recent = emptyList<String>()
        for (id in listOf("palm-tx", "palm-e2", "palm-v")) recent = DevicePicker.pushRecent(recent, id)
        assertEquals(listOf("palm-v", "palm-e2", "palm-tx"), recent)
        recent = DevicePicker.pushRecent(recent, "palm-tx")
        assertEquals(listOf("palm-tx", "palm-v", "palm-e2"), recent)
        recent = DevicePicker.pushRecent(recent, "palm-m515")
        assertEquals(listOf("palm-m515", "palm-tx", "palm-v"), recent)
    }

    @Test fun savedRecentSkipsUnknownIds() {
        assertEquals(listOf("palm-tx", "palm-e2"), DevicePicker.parse("palm-tx,gone,palm-e2", Device.shown).map { it.id })
        assertTrue(DevicePicker.parse(null, Device.shown).isEmpty())
        assertTrue(DevicePicker.parse("", Device.shown).isEmpty())
    }
}
