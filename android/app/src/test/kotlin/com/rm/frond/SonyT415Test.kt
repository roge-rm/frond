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

// The Sony CLIE T415 on Android: ROM detection from synthetic headers (no firmware), the inventory,
// its Memory Stick, its keys and the settings its 68k core takes.
class SonyT415Test {
    @get:Rule val tmp = TemporaryFolder()

    private val t415 get() = Device.byId("sony-clie-t415")!!

    // A VZ ROM with a big-endian card header at 0, like tests/firmware-input.cpp's rom68k.
    private fun rom(size: Long, company: Long = 0x736f6e79L, hal: Long = 0x766e6365L, base: Long = 0x10000000L,
                    flags: Int = 0x0040): File {
        val f = tmp.newFile()
        RandomAccessFile(f, "rw").use { file ->
            file.setLength(size)
            fun be(at: Long, value: Long, bytes: Int) {
                file.seek(at)
                for (i in 0 until bytes) file.write((value shr (8 * (bytes - 1 - i))).toInt() and 0xff)
            }
            be(0, 0x3000, 4); be(4, base + 0x2a2, 4); be(8, 0xfeedbeefL, 4)
            be(0x0c, 5, 2); be(0x0e, flags.toLong(), 2)
            file.seek(0x10); file.write("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
            be(0x6c, size, 4)
            be(0x7e, company, 4); be(0x82, hal, 4)
        }
        return f
    }

    @Test fun t415IsInTheInventory() {
        val d = t415
        assertTrue(d in Device.shown)
        assertEquals("Sony CLIÉ T415", d.label)
        assertTrue(d.runs68k && d.colourLcd.not() && d.lcdContrast)
        assertEquals("ROM image", d.firmwareLabel)
        assertEquals(listOf("--device", "sony-clie-t415"), d.coreFlags)
        assertEquals(listOf("Date Book", "Address", "To Do", "Memo Pad"), d.appNames)
        assertEquals(listOf("up", "down", "app1", "app2", "app3", "app4", "jog-up", "jog-down", "jog-press", "back"), d.buttons)
        assertEquals(320, d.width)
        assertEquals(320, d.height)
        assertEquals(38, d.sideTabWidth)
        assertTrue(d.jogControls && d.jogBesideButtons)
        // The beeper, IrDA, serial cable and USB cradle.
        assertTrue(d.sound && d.infrared && d.irPhone && d.usb && d.serialPort)
        assertFalse(d.modem || d.network || d.keyboard || d.bluetooth || d.microphone || d.camera || d.fastAudio)
        assertEquals("", d.pendingUsb)
        assertEquals(Frond68kActivity::class.java, Emulators.activity(d))
        assertEquals(":emulator68k", Emulators.process(d))
        assertEquals(MachineRows(contrast = true, speed = true, engine = false, audioPreview = false), MachineRows.of(d))
    }

    @Test fun theMemoryStickIsOneLinkedSlot() {
        val d = t415
        assertTrue(d.memoryStick && d.linkedCards && d.cardSupported)
        assertEquals(1, d.storageSlots.size)
        val slot = d.storageSlots[0]
        assertTrue(slot.stick)
        assertEquals("ms-", slot.commandPrefix)
        assertEquals("Memory Stick", slot.title)
        val family = d.familiesOf(slot).single()
        assertEquals(listOf(4, 8, 16, 32, 64, 128), family.sizes)
        assertTrue(family.accepts(8L shl 20))
        assertFalse(family.accepts(256L shl 20))
        assertFalse(family.accepts(6L shl 20))
        val id = "1".repeat(32)
        val state = LinkedCards.state("inserted $id writable")!!
        assertTrue(state.inserted && !state.writeProtected)
        assertNull(LinkedCards.sessionCopyError(listOf("none"), 1))
        assertNotNull(LinkedCards.sessionCopyError(listOf("inserted $id writable"), 1))
        // The library takes classic sticks in its slot, nothing else.
        assertTrue(Cards.deviceTakes(d, "ms", 8))
        assertFalse(Cards.deviceTakes(d, "ms", 256))
        assertFalse(Cards.deviceTakes(d, "sd", 32))
        assertFalse(Cards.hasSlot(d, 1))
        assertEquals(listOf("ms"), CardChoices.kindsFor(d))
    }

    @Test fun theRomIsToldByItsSonyHeader() {
        val vnce = rom(0x320000)
        assertEquals(t415, Firmware.detect(vnce))
        assertNull(Firmware.check(t415, vnce, null))
        assertTrue(Firmware.check(t415, vnce, tmp.newFile())!!.contains("no NAND"))
        // Another VZ Palm's ROM, or a ROM with the wrong base, isn't one.
        val m125 = rom(0x300000, company = 0x70616c6dL, hal = 0x76737475L)
        assertEquals(Device.byId("palm-m125"), Firmware.detect(m125))
        assertNotNull(Firmware.check(t415, m125, null))
        assertNull(Firmware.detect(rom(0x320000, base = 0x10c00000L)))
        assertNotNull(Firmware.check(t415, rom(0x320000, base = 0x10c00000L), null))
        assertNotNull(Firmware.check(Device.byId("palm-iiix")!!, vnce, null))
        // An EZ ROM isn't for the VZ T415.
        assertNotNull(Firmware.check(t415, rom(0x320000, flags = 0x0020), null))
    }

    @Test fun controllerKeysReachTheJogDial() {
        val map = ControllerMap.defaults(t415)
        assertEquals(PalmButton.JOG_UP, map.keys[android.view.KeyEvent.KEYCODE_BUTTON_L2])
        assertEquals(PalmButton.JOG_PRESS, map.keys[android.view.KeyEvent.KEYCODE_BUTTON_THUMBR])
        assertEquals(PalmButton.BACK, map.keys[android.view.KeyEvent.KEYCODE_BUTTON_THUMBL])
        // The IIIx has none of these buttons.
        assertFalse(PalmButton.JOG_UP in PalmButton.on(Device.byId("palm-iiix")!!))
        assertEquals("Jog press", PalmButton.JOG_PRESS.label(t415))
        assertEquals(0x4000004B, PalmButtons.sdlKey(PalmButton.JOG_UP))
        assertEquals(0x4000004E, PalmButtons.sdlKey(PalmButton.JOG_DOWN))
        assertEquals(0x4000004D, PalmButtons.sdlKey(PalmButton.JOG_PRESS))
        assertEquals(0x4000004A, PalmButtons.sdlKey(PalmButton.BACK))
    }

    @Test fun jogControlsAndContrastGoToTheCore() {
        val s = MachineSettings(contrast = 40, contrastSet = true)
        val args = s.arguments(m68k = true, jogControls = t415.jogControls)
        assertEquals(listOf("--window-fit", "fit", "--jog-controls", "keyboard", "--contrast", "40"), args)
        assertEquals("fit=fit smooth=off jog=keyboard contrast=40 buttons=on",
            s.displayArgument(m68k = true, jogControls = true))
        assertEquals(listOf("--window-fit", "fit", "--jog-controls", "hidden", "--hide-buttons", "--contrast", "40"),
            s.arguments(m68k = true, jogControls = true, touchHidden = true))
        assertEquals("fit=fit smooth=off jog=hidden contrast=40 buttons=off",
            s.displayArgument(m68k = true, jogControls = true, touchHidden = true))
    }
}
