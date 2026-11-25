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

    // Low-latency audio is the shorter queue only: no qualified firmware, so never the preview.
    @Test fun lowLatencyIsQueueOnly() {
        assertTrue(rx1950.lowLatencyAudio)
        assertFalse(rx1950.fastAudio)
        assertFalse(Firmware.fastAudioQualified(rx1950, null, sized(rx1950.norSize)))
        val arguments = MachineSettings(fastAudio = true).audioArguments(rx1950.lowLatencyAudio, 512) {
            Firmware.fastAudioQualified(rx1950, null, sized(rx1950.norSize))
        }
        assertEquals(listOf("--low-latency-queue", "--audio-period", "512"), arguments)
    }

    @Test fun detectedByTheBootloaderSize() {
        assertEquals(rx1950, Firmware.detect(sized(Firmware.RX1950_CEBL.bytes)))
    }

    @Test fun eachFileCheckedWithItsOwnMessage() {
        val cebl = sized(Firmware.RX1950_CEBL.bytes)
        val ceos = sized(Firmware.RX1950_CEOS.bytes)
        assertEquals(Firmware.RX1950_OS_MISSING, Firmware.check(rx1950, cebl, null))
        // Right sizes, wrong bytes.
        assertEquals("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.", Firmware.check(rx1950, cebl, ceos))
        // The two swapped, or an OS image alone as the NOR.
        assertEquals(Firmware.RX1950_SWAPPED, Firmware.check(rx1950, ceos, cebl))
        assertEquals(Firmware.RX1950_SWAPPED, Firmware.check(rx1950, ceos, null))
    }

    // The right NOR (by content: the check hashes it); the OS image is HP's by hash, another NBF that lays out
    // (accepted, with a warning), or refused.
    @Test fun osImageAcceptedByLayout() {
        val bytes = File(tmp.root, "cebl").also { it.writeBytes(ByteArray(Firmware.RX1950_CEBL.bytes.toInt())) }
        val known = Firmware.RX1950_CEBL.copy(sha256 = Firmware.sha256(bytes)!!)
        assertTrue(Firmware.matches(bytes, known))
        val other = sized(Firmware.RX1950_CEOS.bytes)
        assertEquals(Firmware.Rx1950Os.OTHER, Firmware.rx1950Os(other))
        assertNull(Firmware.checkRx1950(bytes, other, known))
        // HP's, by a hash told to the check.
        val hp = Firmware.RX1950_CEOS.copy(sha256 = Firmware.sha256(other)!!)
        assertEquals(Firmware.Rx1950Os.HP, Firmware.rx1950Os(other, hp))
        assertNull(Firmware.checkRx1950(bytes, other, known, hp))
        // A raw dump (wrong size), and a right-sized file whose SectorInfo isn't one.
        assertEquals(Firmware.RX1950_OS_UNUSABLE, Firmware.checkRx1950(bytes, sized(100000), known))
        val junk = File(tmp.root, "junk.nbf").also { it.writeBytes(ByteArray(0xa5a + 520) { 0x12 }) }
        assertEquals(Firmware.Rx1950Os.UNUSABLE, Firmware.rx1950Os(junk))
        assertEquals(Firmware.RX1950_OS_UNUSABLE, Firmware.checkRx1950(bytes, junk, known))
        // An OS image as the NOR.
        assertEquals(Firmware.RX1950_SWAPPED, Firmware.checkRx1950(other, bytes, known))
    }

    // Picking CEOS.nbf as the NOR with the rx1950 chosen copies it (check() then names it); past that, and for
    // the other devices past 32 MiB, the message says what's wrong.
    @Test fun pickLimitTakesCeosForTheRx1950() {
        assertTrue(Firmware.pickLimit(rx1950) >= Firmware.RX1950_NBF_MAX)
        assertEquals(32L shl 20, Firmware.pickLimit(null))
        assertEquals(Firmware.NOT_DETECTED, Firmware.tooBig(null))
        assertEquals("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.", Firmware.tooBig(rx1950))
    }

    // Allow other firmware: HP's size is enough, and the off checks stay as they were.
    @Test fun otherFirmwareTakesHpsSize() {
        val other = sized(Firmware.RX1950_CEBL.bytes)
        val wrong = sized(Firmware.RX1950_CEBL.bytes - 1)
        val ceos = sized(Firmware.RX1950_CEOS.bytes)
        assertNull(Firmware.check(rx1950, other, ceos, true))
        assertEquals(Firmware.RX1950_ANY_SIZE, Firmware.check(rx1950, wrong, ceos, true))
        assertEquals("The NOR image must be a 264,794-byte CEBL.nbf.", Firmware.RX1950_ANY_SIZE)
        assertEquals(Firmware.RX1950_OS_MISSING, Firmware.check(rx1950, other, null, true))
        assertEquals(Firmware.RX1950_ANY_SIZE, Firmware.tooBig(rx1950, true))
        // Off: the same file fails as before.
        assertEquals("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.", Firmware.check(rx1950, other, ceos))
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
