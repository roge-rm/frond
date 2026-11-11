package com.rm.frond

import android.content.pm.ActivityInfo
import android.view.KeyEvent
import com.rm.frond.ZodiacPad.Event.Axis
import com.rm.frond.ZodiacPad.Event.Button
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ZodiacTest {
    private val zodiac = Device.byId(Device.ZODIAC_ID)!!

    @Test fun listedWithItsScreenAndNoFirmwareVariant() {
        assertTrue(zodiac in Device.shown)
        assertTrue(zodiac.zodiac)
        assertEquals(480 to 320, zodiac.width to zodiac.height)
        assertEquals(8192000L, zodiac.norSize)
        assertFalse(zodiac.sound)
        assertFalse(zodiac.runs68k)
        assertFalse(Device.entries.any { it.zodiac && it != zodiac })
    }

    @Test fun newMachinesRunFastAtOriginalSpeed() {
        assertEquals(Engine.FAST, MachineSettings.defaults(zodiac).engine)
        assertEquals(CpuSpeed.ORIGINAL, MachineSettings.defaults(zodiac).speed)
        assertEquals(Engine.EXACT, MachineSettings.defaults(Device.TX).engine)
        assertTrue(MachineSettings.defaults(zodiac).arguments().containsAll(listOf("--engine", "fast")))
        assertFalse("--mips" in MachineSettings.defaults(zodiac).arguments())
        assertTrue("--no-sound" in MachineSettings.defaults(zodiac).arguments(soundSupported = zodiac.sound))
    }

    @Test fun choosingAnotherDeviceKeepsAChosenEngine() {
        // Left at the default: it follows the device.
        assertEquals(Engine.FAST, MachineSettings.defaults(Device.TX).forDevice(Device.TX, zodiac).engine)
        assertEquals(Engine.EXACT, MachineSettings.defaults(zodiac).forDevice(zodiac, Device.TX).engine)
        // Chosen: it stays.
        assertEquals(Engine.EXACT, MachineSettings(engine = Engine.EXACT).forDevice(zodiac, Device.TX).engine)
        val tx = MachineSettings(engine = Engine.FAST)
        assertEquals(Engine.FAST, tx.forDevice(Device.TX, Device.E2).engine)
        assertEquals(tx, tx.forDevice(Device.TX, Device.E2))
    }

    @Test fun firmwareIsDetected() {
        // A small and a big card, each with the Zodiac's company and HAL ids, the big one ending at the end.
        val size = zodiac.norSize
        val image = ByteArray(size.toInt())
        fun put32(at: Long, value: Long) { for (i in 0 until 4) image[(at + i).toInt()] = (value shr (8 * i)).toByte() }
        for (at in listOf(0L, zodiac.bigCardOffset)) {
            "PalmCard\u0000".toByteArray().copyInto(image, (at + 0x10).toInt())
            put32(at + 8, 0xfeedbeefL)
            put32(at + 0x80, zodiac.companyId)
            put32(at + 0x84, zodiac.halId)
        }
        put32(zodiac.bigCardOffset + 0x6c, size - zodiac.bigCardOffset)
        val info = Firmware.inspect(size) { at, count -> image.copyOfRange(at.toInt(), (at + count).toInt()) }
        assertEquals(zodiac, info.device)
        assertTrue(info.validLayout)
    }

    private fun pad(): Pair<ZodiacPad, MutableList<ZodiacPad.Event>> {
        val events = mutableListOf<ZodiacPad.Event>()
        return ZodiacPad { events += it } to events
    }

    @Test fun buttonsGoAsControllerButtons() {
        val (pad, events) = pad()
        assertTrue(pad.key(3, KeyEvent.KEYCODE_BUTTON_A, true))
        assertTrue(pad.key(3, KeyEvent.KEYCODE_BUTTON_A, false))
        assertTrue(pad.key(3, KeyEvent.KEYCODE_BUTTON_SELECT, true))
        assertTrue(pad.key(3, KeyEvent.KEYCODE_BUTTON_START, true))
        assertTrue(pad.key(3, KeyEvent.KEYCODE_BUTTON_THUMBL, true))
        assertFalse(pad.key(3, KeyEvent.KEYCODE_BUTTON_THUMBR, true))
        assertEquals(listOf(
            Button(3, ZodiacPad.BUTTON_A, true), Button(3, ZodiacPad.BUTTON_A, false),
            Button(3, ZodiacPad.BUTTON_BACK, true), Button(3, ZodiacPad.BUTTON_START, true),
            Button(3, ZodiacPad.LEFT_STICK, true),
        ), events)
    }

    @Test fun shouldersFromKeysAndTriggersLetGoAfterTheLast() {
        val (pad, events) = pad()
        pad.key(1, KeyEvent.KEYCODE_BUTTON_L1, true)
        pad.axes(1, 0f, 0f, 0f, 0f, 0.9f, 0f)
        pad.key(1, KeyEvent.KEYCODE_BUTTON_L1, false)
        // Between release and press nothing changes.
        pad.axes(1, 0f, 0f, 0f, 0f, 0.4f, 0f)
        pad.axes(1, 0f, 0f, 0f, 0f, 0.2f, 0.6f)
        assertEquals(listOf(
            Button(1, ZodiacPad.LEFT_SHOULDER, true),
            Button(1, ZodiacPad.LEFT_SHOULDER, false), Button(1, ZodiacPad.RIGHT_SHOULDER, true),
        ), events)
    }

    @Test fun stickHatAndDpad() {
        val (pad, events) = pad()
        pad.axes(2, 0.5f, -1f, 0f, 0f, 0f, 0f)
        assertEquals(listOf(Axis(2, ZodiacPad.AXIS_LEFT_X, 16383), Axis(2, ZodiacPad.AXIS_LEFT_Y, -32768)), events)
        events.clear()
        // The hat wins over the stick, the d-pad over both.
        pad.axes(2, 0.5f, -1f, 1f, 0f, 0f, 0f)
        assertEquals(listOf(Axis(2, ZodiacPad.AXIS_LEFT_X, 32767)), events)
        events.clear()
        assertTrue(pad.key(2, KeyEvent.KEYCODE_DPAD_LEFT, true))
        pad.key(2, KeyEvent.KEYCODE_DPAD_DOWN, true)
        assertEquals(listOf(Axis(2, ZodiacPad.AXIS_LEFT_X, -32768), Axis(2, ZodiacPad.AXIS_LEFT_Y, 32767)), events)
        events.clear()
        pad.key(2, KeyEvent.KEYCODE_DPAD_LEFT, false)
        pad.key(2, KeyEvent.KEYCODE_DPAD_DOWN, false)
        pad.axes(2, 0f, 0f, 0f, 0f, 0f, 0f)
        assertEquals(listOf(Axis(2, ZodiacPad.AXIS_LEFT_X, 32767), Axis(2, ZodiacPad.AXIS_LEFT_Y, -32768),
            Axis(2, ZodiacPad.AXIS_LEFT_X, 0), Axis(2, ZodiacPad.AXIS_LEFT_Y, 0)), events)
    }

    @Test fun releaseLetsGoOfOneDevice() {
        val (pad, events) = pad()
        pad.key(1, KeyEvent.KEYCODE_BUTTON_Y, true)
        pad.axes(1, 1f, 0f, 0f, 0f, 0f, 0f)
        pad.key(2, KeyEvent.KEYCODE_BUTTON_B, true)
        events.clear()
        pad.release(1)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_Y, false), Axis(1, ZodiacPad.AXIS_LEFT_X, 0)), events)
        events.clear()
        pad.releaseAll()
        assertEquals(listOf(Button(2, ZodiacPad.BUTTON_B, false)), events)
        events.clear()
        pad.releaseAll()
        assertTrue(events.isEmpty())
    }

    @Test fun followTheProgramTurnsToTheGuestsShape() {
        assertEquals(Orientation.PROGRAM, Settings().orientation)
        assertEquals("Follow the program", Orientation.PROGRAM.label)
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT, Orientation.programRequest(1))
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE, Orientation.programRequest(2))
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT, Orientation.programRequest(3))
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE, Orientation.programRequest(4))
        assertNull(Orientation.programRequest(0))
        assertNull(Orientation.programRequest(5))
        // Other devices have no program orientation: it follows the phone.
        assertEquals(Orientation.PHONE.request, Orientation.PROGRAM.request)
    }
}
