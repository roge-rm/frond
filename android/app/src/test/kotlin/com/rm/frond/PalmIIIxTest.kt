package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile
import javax.xml.parsers.DocumentBuilderFactory

// The Palm IIIx on Android: ROM detection from synthetic headers (no firmware), the inventory, its
// settings rows and arguments, and the activity and process its 68k core runs in.
class PalmIIIxTest {
    @get:Rule val tmp = TemporaryFolder()

    private val iiix get() = Device.byId("palm-iiix")!!

    // A 68k ROM with a big-endian card header, like tests/firmware-input.cpp's rom68k.
    private fun rom(size: Long, version: Int, flags: Int = 0x0020, company: Long = 0, hal: Long = 0,
                    resetOffset: Long = 0x822a): File {
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            fun be(at: Long, value: Long, bytes: Int) {
                file.seek(at)
                for (i in 0 until bytes) file.write((value shr (8 * (bytes - 1 - i))).toInt() and 0xff)
            }
            be(0, 0x3000, 4); be(4, 0x10c00000L + resetOffset, 4); be(8, 0xfeedbeefL, 4)
            be(0x0c, version.toLong(), 2); be(0x0e, flags.toLong(), 2)
            file.seek(0x10); file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
            be(0x6c, size, 4)
            if (version >= 5) { be(0x7e, company, 4); be(0x82, hal, 4) }
        }
        return f
    }

    @Test fun iiixRomsAreDetected() {
        val kinds = listOf(
            Triple("3.1 retail", 0x128000L, 3) to 0x822aL, Triple("3.1 debug", 0x128000L, 3) to 0x8236L,
            Triple("3.5 EZ", 0x178000L, 4) to 0x90e0L, Triple("3.5 EZ debug", 0x178000L, 4) to 0x9184L,
            Triple("4.1", 0x1a8000L, 5) to 0xc6eaL,
        )
        for ((kind, reset) in kinds) {
            val (name, size, version) = kind
            val f = rom(size, version, company = 0x70616c6dL, hal = 0x73756d6fL, resetOffset = reset)
            assertEquals(name, iiix, Firmware.detect(f))
            assertNull(name, Firmware.check(iiix, f, null))
            assertTrue(name, Firmware.check(iiix, f, tmp.newFile())!!.contains("no NAND"))
            for (other in listOf(Device.TX, Device.E2, Device.ZIRE72))
                assertEquals(name, "This is a Palm IIIx ROM. Choose Palm IIIx.", Firmware.check(other, f, null))
        }
    }

    @Test fun romsTheIiixCantRunAreRefused() {
        // 68328 only, header version 2, another 4.1 device's IDs: the header is there but it can't run.
        for (f in listOf(rom(0x128000, 3, flags = 0x0010), rom(0x128000, 2),
                         rom(0x1a8000, 5, company = 0x70616c6dL, hal = 0x6173746eL))) {
            assertNull(Firmware.detect(f))
            assertEquals("This ROM isn't one the Palm IIIx can run.", Firmware.check(iiix, f, null))
        }
        // Past the 2 MiB flash part, or truncated.
        assertNotNull(Firmware.check(iiix, rom((2L shl 20) + 0x1000, 3), null))
        val truncated = rom(0x128000, 3)
        RandomAccessFile(truncated, "rw").use { it.setLength(0x127000) }
        assertNotNull(Firmware.check(iiix, truncated, null))
        assertNull(Firmware.detect(truncated))
        // An ARM NOR's little-endian card header isn't a IIIx ROM.
        val arm = tmp.newFile()
        RandomAccessFile(arm, "rw").use { file ->
            file.setLength(0x128000)
            file.seek(8); file.write(byteArrayOf(0xef.toByte(), 0xbe.toByte(), 0xed.toByte(), 0xfe.toByte()))
            file.seek(0x10); file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
        }
        assertEquals("This file isn't a Palm IIIx ROM.", Firmware.check(iiix, arm, null))
        assertEquals("This file isn't a Palm IIIx ROM.", Firmware.check(iiix, tmp.newFile(), null))
    }

    @Test fun inventoryHasTheIiix() {
        val d = iiix
        assertTrue(d in Device.shown)
        assertEquals("Palm IIIx", d.label)
        assertTrue(d.runs68k)
        assertEquals("ROM image", d.firmwareLabel)
        assertFalse(d.hasNand)
        assertFalse(d.nandRequired)
        assertEquals(listOf("Date Book", "Address", "To Do List", "Memo Pad"), d.appNames)
        assertEquals(listOf("up", "down", "app1", "app2", "app3", "app4", "power"), d.buttons)
        // The piezo, the cradle serial port and IrDA; nothing else.
        assertTrue(d.sound && d.serialPort && d.infrared)
        assertFalse(d.network || d.keyboard || d.usb || d.bluetooth || d.microphone || d.camera || d.fastAudio)
        assertFalse(d.cardSupported || d.holdPresent)
        assertEquals(listOf("--device", "palm-iiix"), d.coreFlags)
        // The Dana runs on the 68k core too; every other device is ARM.
        for (arm in Device.entries.filter { it != d && it.id != "alphasmart-dana" }) assertFalse(arm.id, arm.runs68k)
    }

    @Test fun settingsRowsFollowTheDevice() {
        for (debug in listOf(false, true)) {
            assertEquals(MachineRows(contrast = true, speed = true, engine = false, audioPreview = false), MachineRows.of(iiix, debug))
            assertEquals(MachineRows(contrast = false, speed = false, engine = debug, audioPreview = true), MachineRows.of(Device.TX, debug))
        }
    }

    @Test fun argumentsCarryContrastAndSpeed() {
        val s = MachineSettings(contrast = 30, contrastSet = true, speed = CpuSpeed.UNCAPPED, engine = Engine.FAST)
        val args = s.arguments(soundSupported = true, m68k = true)
        assertEquals(listOf("--window-fit", "fit", "--contrast", "30", "--speed", "uncapped"), args)
        assertFalse("--engine" in s.arguments(m68k = true))
        // Without a chosen contrast the session's own stays.
        assertEquals(listOf("--window-fit", "fit"), MachineSettings().arguments(m68k = true))
        assertFalse("contrast" in MachineSettings().json().keys().asSequence().toList())
        assertEquals(listOf("--window-fit", "fit", "--engine", "fast"), s.arguments())
        assertEquals("fit=fit smooth=off contrast=30", s.displayArgument(m68k = true))
        assertEquals("fit=fit smooth=off", s.displayArgument())
        val back = MachineSettings.from(s.json())
        assertEquals(30, back.contrast)
        assertEquals(CpuSpeed.UNCAPPED, back.speed)
        // Older settings have neither; a contrast out of range is clamped.
        val old = MachineSettings.from(s.json().apply { remove("contrast"); remove("speed") })
        assertEquals(MachineSettings.DEFAULT_CONTRAST, old.contrast)
        assertFalse(old.contrastSet)
        assertEquals(CpuSpeed.ORIGINAL, old.speed)
        assertEquals(100, MachineSettings.from(s.json().put("contrast", 400)).contrast)
    }

    @Test fun the68kCoreRunsInItsOwnProcess() {
        assertEquals(Frond68kActivity::class.java, Emulators.activity(iiix))
        assertEquals("main68k", Emulators.library(iiix))
        assertEquals(":emulator68k", Emulators.process(iiix))
        for (arm in Device.entries.filter { !it.runs68k }) {
            assertEquals(FrondActivity::class.java, Emulators.activity(arm))
            assertEquals("main", Emulators.library(arm))
            assertEquals(":emulator", Emulators.process(arm))
        }
        // The manifest puts each activity in the process Emulators names.
        val manifest = File("src/main/AndroidManifest.xml")
        val doc = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }.newDocumentBuilder().parse(manifest)
        val android = "http://schemas.android.com/apk/res/android"
        val activities = doc.getElementsByTagName("activity")
        val processes = (0 until activities.length).associate {
            val e = activities.item(it) as org.w3c.dom.Element
            e.getAttributeNS(android, "name") to e.getAttributeNS(android, "process")
        }
        assertEquals(":emulator68k", processes[".Frond68kActivity"])
        assertEquals(":emulator", processes[".FrondActivity"])
        assertEquals(setOf(":emulator", ":emulator68k"), Emulators.processes.toSet())
    }
}
