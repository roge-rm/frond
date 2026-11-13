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
    private val ezIds = listOf("palm-iiie", "palm-iiixe", "palm-v", "palm-vx", "palm-m100", "palm-m105")
    private val vzIds = listOf("palm-m125", "palm-m500", "ibm-workpad-c500")
    private val colourIds = listOf("palm-iiic", "palm-m505", "palm-m515", "palm-m130", "palm-m520")
    private val greyVzIds = listOf("sony-clie-t415")

    // A 68k ROM with a big-endian card header, like tests/firmware-input.cpp's rom68k.
    private fun rom(size: Long, version: Int, flags: Int = 0x0020, company: Long = 0, hal: Long = 0,
                    resetOffset: Long = 0x822a, base: Long = 0x10c00000L): File {
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            fun be(at: Long, value: Long, bytes: Int) {
                file.seek(at)
                for (i in 0 until bytes) file.write((value shr (8 * (bytes - 1 - i))).toInt() and 0xff)
            }
            be(0, 0x3000, 4); be(4, base + resetOffset, 4); be(8, 0xfeedbeefL, 4)
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
        // 68328 only, header version 2, an unknown HAL: the header is there but it can't run.
        for (f in listOf(rom(0x128000, 3, flags = 0x0010), rom(0x128000, 2),
                         rom(0x1a8000, 5, company = 0x70616c6dL, hal = 0x12345678L))) {
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
        // The Danas run on the 68k core too; every other device is ARM.
        for (arm in Device.entries.filter { it != d && !it.isDana() && it.id !in ezIds && it.id !in vzIds && it.id !in colourIds && it.id !in greyVzIds }) assertFalse(arm.id, arm.runs68k)
    }

    @Test fun otherEzDevicesAreInTheInventory() {
        val expect = mapOf("palm-iiie" to (2 to true), "palm-iiixe" to (8 to true), "palm-v" to (2 to false),
            "palm-vx" to (8 to false), "palm-m100" to (2 to false), "palm-m105" to (8 to false))
        for ((id, ram) in expect) {
            val d = Device.byId(id)!!
            assertTrue(id, d in Device.shown && d.runs68k && d != iiix)
            assertEquals("ROM image", d.firmwareLabel)
            assertTrue(id, d.sound && d.serialPort && d.infrared)
            assertFalse(id, d.network || d.keyboard || d.usb || d.bluetooth || d.microphone || d.camera || d.fastAudio)
            assertFalse(id, d.cardSupported || d.hasNand)
            assertEquals(id, ram.second, "power" in d.buttons)
            assertEquals(listOf("--device", id), d.coreFlags)
        }
    }

    @Test fun anyEzDeviceTakesAnEzRomAndTheM100FamilyIsToldByItsClockDatabase() {
        for (id in listOf("palm-iiie", "palm-iiixe", "palm-v", "palm-vx", "palm-m100", "palm-m105")) {
            val d = Device.byId(id)!!
            for (version in 3..5) {
                val f = rom(0x178000, version, company = 0x70616c6dL, hal = 0x73756d6fL)
                assertNull(id, Firmware.check(d, f, null))
                assertEquals(iiix, Firmware.detect(f))
            }
            assertEquals("The ${d.label} has no NAND. Leave the NAND image empty.", Firmware.check(d, rom(0x178000, 3), tmp.newFile()))
            assertEquals("This ROM isn't one the ${d.label} can run.", Firmware.check(d, rom(0x178000, 3, flags = 0x0010), null))
        }
        val clock = "cclkPalmClock\u0000".toByteArray(Charsets.US_ASCII)
        for (at in listOf(0x150000L, 0x1fffffL - 13, 0x1fffcL)) {
            val f = rom(0x200000, 4, resetOffset = 0xa06a)
            assertEquals(iiix, Firmware.detect(f))
            RandomAccessFile(f, "rw").use { it.seek(at); it.write(clock) }
            val m100 = Device.byId("palm-m100")!!
            assertEquals(m100, Firmware.detect(f))
            assertNull(Firmware.check(m100, f, null))
            assertNull(Firmware.check(Device.byId("palm-m105")!!, f, null))
        }
    }

    @Test fun vzPalmsTakeTheirOwnRomsAndHaveOneCardSlot() {
        val m125 = Device.byId("palm-m125")!!
        val m500 = Device.byId("palm-m500")!!
        val c500 = Device.byId("ibm-workpad-c500")!!
        for (d in listOf(m125, m500, c500)) {
            assertTrue(d.id, d in Device.shown && d.runs68k && d.linkedCards)
            assertEquals(d.id, 1, d.storageSlots.size)
            assertEquals("USB HotSync", d.pendingUsb)
            assertTrue(d.id, d.serialPort && d.infrared)
            assertEquals(listOf("--device", d.id), d.coreFlags)
        }
        assertEquals("", iiix.pendingUsb)
        assertEquals("USB computer", Device.byId("alphasmart-dana")!!.pendingUsb)
        fun vz(hal: Long) = rom(0x300000, 5, flags = 0x0040, company = 0x70616c6dL, hal = hal, resetOffset = 0x2a2,
            base = 0x10000000L)
        val vstu = vz(0x76737475L)
        val vtrn = vz(0x7674726eL)
        assertEquals(m125, Firmware.detect(vstu))
        assertEquals(m500, Firmware.detect(vtrn))
        assertNull(Firmware.check(m125, vstu, null))
        assertNull(Firmware.check(m500, vtrn, null))
        // The c500 runs the m500's ROM, whose header can't tell them apart.
        assertNull(Firmware.check(c500, vtrn, null))
        assertEquals("This is a Palm m500 ROM. Choose Palm m500.", Firmware.check(m125, vtrn, null))
        assertEquals("This is a Palm m125 ROM. Choose Palm m125.", Firmware.check(c500, vstu, null))
        assertNotNull(Firmware.check(iiix, vstu, null))
        // Past 4 MiB, or a reset vector outside the ROM.
        assertNull(Firmware.detect(rom((4L shl 20) + 0x1000, 5, flags = 0x0040, company = 0x70616c6dL,
            hal = 0x76737475L, base = 0x10000000L)))
        assertNull(Firmware.detect(rom(0x300000, 5, flags = 0x0040, company = 0x70616c6dL, hal = 0x76737475L)))
    }

    @Test fun colourPalmsHaveNoContrastAndTheM520IsToldByItsBigCard() {
        for (id in colourIds) {
            val d = Device.byId(id)!!
            assertTrue(id, d in Device.shown && d.runs68k && d.colourLcd && !d.lcdContrast)
            assertEquals(MachineRows(contrast = false, speed = true, engine = false, audioPreview = false), MachineRows.of(d))
        }
        assertTrue(iiix.lcdContrast && !iiix.colourLcd)
        for (id in colourIds - "palm-iiic") assertEquals(id, 1, Device.byId(id)!!.storageSlots.size)
        val iiic = Device.byId("palm-iiic")!!
        assertEquals(iiic, Firmware.detect(rom(0x1a8000, 5, company = 0x70616c6dL, hal = 0x6173746eL, resetOffset = 0xc6e6)))
        // The IIIc takes only its own ROM, and the IIIx and the other EZ devices take no IIIc ROM.
        val iiicRom = rom(0x1a8000, 5, company = 0x70616c6dL, hal = 0x6173746eL, resetOffset = 0xc6e6)
        assertNull(Firmware.check(iiic, iiicRom, null))
        assertEquals("This is a Palm IIIc ROM. Choose Palm IIIc.", Firmware.check(iiix, iiicRom, null))
        assertEquals("This is a Palm IIIc ROM. Choose Palm IIIc.", Firmware.check(Device.byId("palm-v")!!, iiicRom, null))
        assertEquals("This is a Palm IIIx ROM. Choose Palm IIIx.", Firmware.check(iiic, rom(0x178000, 3), null))
        assertNull(Firmware.check(Device.byId("palm-iiie")!!, rom(0x178000, 3), null))
        val m505 = Device.byId("palm-m505")!!
        val m520 = Device.byId("palm-m520")!!
        // The m520's small card has the m505's IDs; the big card at 64 KiB has its own.
        val split = rom(0x400000, 5, flags = 0x0040, company = 0x70616c6dL, hal = 0x65637479L, resetOffset = 0x2a2,
            base = 0x10000000L)
        assertEquals(m505, Firmware.detect(split))
        RandomAccessFile(split, "rw").use { file ->
            fun be(at: Long, value: Long) {
                file.seek(at)
                for (i in 0 until 4) file.write((value shr (8 * (3 - i))).toInt() and 0xff)
            }
            be(0x6c, 0x8000); be(0x68, 0x10010000L); be(0x10008, 0xfeedbeefL)
            file.seek(0x1000c); file.write(byteArrayOf(0, 5))
            file.seek(0x10010); file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
            be(0x1007e, 0x70616c6dL); be(0x10082, 0x7366676fL)
        }
        assertEquals(m520, Firmware.detect(split))
        assertNull(Firmware.check(m520, split, null))
        assertEquals("This is a Palm m520 ROM. Choose Palm m520.", Firmware.check(m505, split, null))
    }

    @Test fun settingsRowsFollowTheDevice() {
        assertEquals(MachineRows(contrast = true, speed = true, engine = false, audioPreview = false), MachineRows.of(iiix))
        assertEquals(MachineRows(contrast = false, speed = true, engine = true, audioPreview = true), MachineRows.of(Device.TX))
    }

    @Test fun argumentsCarryContrastAndSpeed() {
        val s = MachineSettings(contrast = 30, contrastSet = true, speed = CpuSpeed.UNCAPPED, engine = Engine.FAST)
        val args = s.arguments(soundSupported = true, m68k = true)
        assertEquals(listOf("--window-fit", "fit", "--contrast", "30", "--speed", "uncapped"), args)
        assertFalse("--engine" in s.arguments(m68k = true))
        // A colour LCD has no contrast, so none is passed.
        assertFalse("--contrast" in s.arguments(m68k = true, colourLcd = true))
        assertEquals("fit=fit smooth=off", s.displayArgument(m68k = true, colourLcd = true))
        // Without a chosen contrast the session's own stays.
        assertEquals(listOf("--window-fit", "fit"), MachineSettings().arguments(m68k = true))
        assertFalse("contrast" in MachineSettings().json().keys().asSequence().toList())
        assertEquals(listOf("--window-fit", "fit", "--engine", "fast", "--mips", "1000"), s.arguments())
        // Original passes no --mips, so the core runs the device at its own.
        assertEquals(listOf("--window-fit", "fit"), MachineSettings().arguments())
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
