package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class FirmwareRecoverTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun file(name: String, text: String? = null): File {
        val f = File(tmp.root, name)
        if (text != null) f.writeText(text)
        return f
    }

    private fun targets() = listOf(file("nor.bin"), file("nand.bin"))

    @Test fun finishedImportDropsOldFiles() {
        file("nor.bin", "new"); file("nand.bin", "new")
        file("nor.bin.old", "old"); file("nand.bin.old", "old")
        Firmware.recover(targets())
        assertEquals("new", file("nor.bin").readText())
        assertEquals("new", file("nand.bin").readText())
        assertFalse(file("nor.bin.old").exists() || file("nand.bin.old").exists())
    }

    @Test fun stoppedImportRestoresTheWholePair() {
        file("nor.bin", "new")
        file("nor.bin.old", "old"); file("nand.bin.old", "old")
        Firmware.recover(targets())
        assertEquals("old", file("nor.bin").readText())
        assertEquals("old", file("nand.bin").readText())
        assertFalse(file("nor.bin.old").exists() || file("nand.bin.old").exists())
    }

    @Test fun droppedNandComesBackWithOldNor() {
        file("nor.bin", "new")
        file("nor.bin.old", "old"); file("nand.bin.old", "old")
        Firmware.recover(targets())
        assertTrue(file("nand.bin").exists())
    }

    @Test fun stopDuringFirstRenamesKeepsTheUnmovedFile() {
        file("nand.bin", "old"); file("nor.bin.old", "old")
        Firmware.recover(targets())
        assertEquals("old", file("nor.bin").readText())
        assertEquals("old", file("nand.bin").readText())
    }

    @Test fun nothingToDo() {
        file("nor.bin", "a")
        Firmware.recover(targets())
        assertEquals("a", file("nor.bin").readText())
    }
}
