package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile

class Rx1950FirmwareTest {
    @get:Rule val tmp = TemporaryFolder()

    private val rx1950 get() = Device.byId(Device.RX1950_ID)!!

    private fun sized(size: Long): File = tmp.newFile().also { f -> RandomAccessFile(f, "rw").use { it.setLength(size) } }

    @Test fun listedOnFast() {
        assertTrue(rx1950 in Device.shown)
        assertTrue(rx1950.rx1950 && rx1950.nandRequired && !rx1950.runs68k)
        assertEquals(Engine.FAST, rx1950.defaultEngine)
        assertEquals(Engine.FAST, MachineSettings.defaults(rx1950).engine)
        // The five-way's centre is a controller button like on the Palms.
        assertTrue(PalmButton.CENTRE in PalmButton.on(rx1950))
    }

    @Test fun detectedByTheBootloaderSize() {
        assertEquals(rx1950, Firmware.detect(sized(Firmware.RX1950_CEBL.bytes)))
    }

    @Test fun eachFileCheckedWithItsOwnMessage() {
        val cebl = sized(Firmware.RX1950_CEBL.bytes)
        val ceos = sized(Firmware.RX1950_CEOS.bytes)
        assertEquals("Choose CEOS.nbf as the NAND image for the HP iPAQ rx1950.", Firmware.check(rx1950, cebl, null))
        // Right sizes, wrong bytes.
        assertEquals("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.", Firmware.check(rx1950, cebl, ceos))
        assertEquals("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.", Firmware.check(rx1950, ceos, cebl))
    }

    @Test fun sha256MatchesByContent() {
        val f = tmp.newFile().also { it.writeText("abc") }
        val abc = Firmware.KnownFile("abc", 3L, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        assertTrue(Firmware.matches(f, abc))
        assertFalse(Firmware.matches(f, abc.copy(bytes = 4L)))
        assertFalse(Firmware.matches(f, abc.copy(sha256 = "0".repeat(64))))
        assertNull(Firmware.sha256(File(tmp.root, "missing")))
    }
}
