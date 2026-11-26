package com.rm.frond

import android.content.pm.ActivityInfo
import android.view.KeyEvent
import java.nio.file.Files
import com.rm.frond.ZodiacPad.Event.Axis
import com.rm.frond.ZodiacPad.Event.Button
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ZodiacTest {
    private val zodiac = Device.byId(Device.ZODIAC_ID)!!
    private val zodiac1 = Device.byId("tapwave-zodiac-1")!!

    @Test fun listedWithItsScreenAndNoFirmwareVariant() {
        for (model in listOf(zodiac1, zodiac)) {
            assertTrue(model in Device.shown)
            assertTrue(model.zodiac)
            assertEquals(480 to 320, model.width to model.height)
            assertEquals(8192000L, model.norSize)
            assertTrue(model.sound)
            assertTrue(model.sdWriteProtection)
            assertFalse(model.lowLatencyAudio)
            assertFalse(model.runs68k)
            assertEquals(Engine.FAST, MachineSettings.defaults(model).engine)
        }
    }

    @Test fun newMachinesRunFastAtOriginalSpeed() {
        assertEquals(Engine.FAST, MachineSettings.defaults(zodiac).engine)
        assertEquals(CpuSpeed.ORIGINAL, MachineSettings.defaults(zodiac).speed)
        assertEquals(Engine.FAST, MachineSettings.defaults(Device.TX).engine)
        assertEquals(Engine.FAST, MachineSettings.defaults(Device.ZIRE72).engine)
        assertEquals(Engine.EXACT, MachineSettings.defaults(Device.E2).engine)
        assertTrue(MachineSettings.defaults(zodiac).arguments().containsAll(listOf("--engine", "fast")))
        assertFalse("--mips" in MachineSettings.defaults(zodiac).arguments())
        assertFalse("--no-sound" in MachineSettings.defaults(zodiac).arguments(soundSupported = zodiac.sound))
        val lowLatency = MachineSettings(fastAudio = true).audioArguments(zodiac.lowLatencyAudio, 256) { true }
        assertFalse("--fast-audio" in lowLatency || "--low-latency-queue" in lowLatency)
    }

    @Test fun directStickIsOnAndGoesToTheCoreInFastOnly() {
        for (model in listOf(zodiac1, zodiac)) {
            val fresh = MachineSettings.defaults(model)
            assertTrue(fresh.directStick)
            assertEquals(listOf("--direct-stick"), fresh.directStickArguments())
            assertEquals(emptyList<String>(), fresh.copy(engine = Engine.EXACT).directStickArguments())
            assertEquals(emptyList<String>(), fresh.copy(directStick = false).directStickArguments())
        }
        // Settings saved before it read as on; off is saved and read back, on isn't written.
        val old = JSONObject().put("engine", "fast")
        assertTrue(MachineSettings.from(old).directStick)
        assertFalse(MachineSettings().json().has("directStick"))
        val off = MachineSettings(engine = Engine.FAST, directStick = false)
        assertFalse(MachineSettings.from(JSONObject(off.json().toString())).directStick)
        assertFalse("--direct-stick" in off.arguments())
    }

    @Test fun choosingAnotherDeviceKeepsAChosenEngine() {
        // Left at the default: it follows the device.
        assertEquals(Engine.FAST, MachineSettings.defaults(Device.E2).forDevice(Device.E2, zodiac).engine)
        assertEquals(Engine.EXACT, MachineSettings.defaults(zodiac).forDevice(zodiac, Device.E2).engine)
        // Chosen: it stays.
        assertEquals(Engine.EXACT, MachineSettings(engine = Engine.EXACT).forDevice(zodiac, Device.E2).engine)
        val tx = MachineSettings(engine = Engine.EXACT)
        assertEquals(Engine.EXACT, tx.forDevice(Device.TX, Device.E2).engine)
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
        val rom = Files.createTempFile("zodiac-nor", ".rom").toFile()
        try {
            rom.writeBytes(image)
            assertEquals(zodiac, Firmware.detect(rom))
            assertNull(Firmware.check(zodiac, rom, null))
            assertNull(Firmware.check(zodiac1, rom, null))
        } finally {
            rom.delete()
        }
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

    @Test fun controlsListEveryZodiacInputWithTodaysDefaults() {
        val on = PalmButton.on(zodiac)
        assertEquals(13, on.size)
        val defaults = ControllerMap.defaults(zodiac)
        assertTrue(defaults.keys.values.all { it in on })
        assertTrue(on.all { defaults.keysFor(it).isNotEmpty() })
        assertEquals(PalmButton.APP1, defaults.keys[KeyEvent.KEYCODE_BUTTON_A])
        assertEquals(PalmButton.APP1, defaults.keys[KeyEvent.KEYCODE_DPAD_CENTER])
        assertEquals(PalmButton.APP4, defaults.keys[KeyEvent.KEYCODE_BUTTON_Y])
        assertEquals(PalmButton.HOME, defaults.keys[KeyEvent.KEYCODE_BUTTON_START])
        assertEquals(PalmButton.HOME, defaults.keys[KeyEvent.KEYCODE_BUTTON_MODE])
        assertEquals(PalmButton.FUNCTION, defaults.keys[KeyEvent.KEYCODE_BUTTON_SELECT])
        assertEquals(PalmButton.STICK_PRESS, defaults.keys[KeyEvent.KEYCODE_BUTTON_THUMBL])
        assertEquals(PalmButton.TRIGGER_LEFT, defaults.keys[KeyEvent.KEYCODE_BUTTON_L2])
        assertEquals(PalmButton.RIGHT, defaults.keys[KeyEvent.KEYCODE_DPAD_RIGHT])
        assertEquals(defaults, ControllerMap.decode(null, zodiac))
        assertEquals("Left trigger", PalmButton.TRIGGER_LEFT.label(zodiac))
        assertEquals("Stick up", PalmButton.UP.label(zodiac))
        assertEquals("Up", PalmButton.UP.label(Device.TX))
    }

    @Test fun controlsSaveAndLoadAsText() {
        val map = ControllerMap.defaults(zodiac).assign(PalmButton.HOME, KeyEvent.KEYCODE_BUTTON_C).clear(PalmButton.FUNCTION)
        assertEquals(map, ControllerMap.decode(map.encode(), zodiac))
        assertTrue(map.keysFor(PalmButton.FUNCTION).isEmpty())
        // A button the Zodiac doesn't list is skipped.
        assertEquals(emptyMap<Int, PalmButton>(), ControllerMap.decode("99:power,98:jog-up", zodiac).keys)
    }

    @Test fun aRemappedKeyReachesTheRightInput() {
        val events = mutableListOf<ZodiacPad.Event>()
        val map = ControllerMap.defaults(zodiac)
            .clear(PalmButton.APP2)
            .assign(PalmButton.APP2, KeyEvent.KEYCODE_BUTTON_C)
            .assign(PalmButton.TRIGGER_RIGHT, KeyEvent.KEYCODE_BUTTON_A)
            .clear(PalmButton.HOME)
        val pad = ZodiacPad(map) { events += it }
        assertTrue(pad.key(1, KeyEvent.KEYCODE_BUTTON_C, true))
        assertTrue(pad.key(1, KeyEvent.KEYCODE_BUTTON_A, true))
        assertFalse(pad.key(1, KeyEvent.KEYCODE_BUTTON_START, true))
        // B was Action 2 and was cleared off it.
        assertFalse(pad.key(1, KeyEvent.KEYCODE_BUTTON_B, true))
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_B, true), Button(1, ZodiacPad.RIGHT_SHOULDER, true)), events)
    }

    @Test fun replacingTheMapReleasesHeldButtons() {
        val events = mutableListOf<ZodiacPad.Event>()
        val pad = ZodiacPad(ControllerMap.defaults(zodiac)) { events += it }
        pad.key(1, KeyEvent.KEYCODE_BUTTON_B, true)
        pad.replaceMap(ControllerMap.defaults(zodiac).clear(PalmButton.APP2))
        assertEquals(Button(1, ZodiacPad.BUTTON_B, false), events.last())
        assertFalse(pad.key(1, KeyEvent.KEYCODE_BUTTON_B, true))
    }

    @Test fun theMapChoosesTheStickThatMovesTheZodiacStick() {
        assertEquals(ZodiacStick.LEFT, ControllerMap.defaults(zodiac).stick)
        assertFalse(ControllerMap.defaults(zodiac).encode().contains("stick:"))
        assertEquals("Stick press (centre)", PalmButton.STICK_PRESS.label(zodiac))
        val rightUp = ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.UP)
        val right = ControllerMap.defaults(zodiac).assign(PalmButton.APP4, rightUp).withStick(ZodiacStick.RIGHT)
        // The chosen stick's own direction keys come off; the saved text round-trips, as the desktop's.
        assertFalse(rightUp in right.keys)
        assertTrue(right.encode().endsWith(",stick:right"))
        assertEquals(right, ControllerMap.decode(right.encode(), zodiac))
        assertEquals(ZodiacStick.NONE, ControllerMap.decode("stick:none", zodiac).stick)
        assertEquals(ZodiacStick.LEFT, ControllerMap.decode("96:app1,stick:sideways", zodiac).stick)

        val events = mutableListOf<ZodiacPad.Event>()
        val pad = ZodiacPad(right) { events += it }
        pad.axes(1, 1f, 0f, 0f, 0f, 0f, 0f, rightX = 0f, rightY = -1f)
        assertEquals(listOf(Axis(1, ZodiacPad.AXIS_LEFT_Y, -32768)), events)
        events.clear()
        pad.replaceMap(right.withStick(ZodiacStick.NONE))
        events.clear()
        pad.axes(1, 1f, 1f, 0f, 0f, 0f, 0f, rightX = 1f, rightY = 1f)
        assertEquals(emptyList<ZodiacPad.Event>(), events)
        pad.key(1, KeyEvent.KEYCODE_DPAD_LEFT, true)
        assertEquals(listOf(Axis(1, ZodiacPad.AXIS_LEFT_X, -32768)), events)
    }

    @Test fun eachStickAxisTakesAnyControllerAxisAndCanBeInverted() {
        // Old saved values are the presets they were.
        assertEquals(ZodiacStick.LEFT, ControllerMap.decode("96:app1", zodiac).stick)
        val oldRight = ControllerMap.decode("96:app1,stick:right", zodiac)
        assertEquals(StickAxis(PadAxis.RIGHT_X), oldRight.stickX)
        assertEquals(StickAxis(PadAxis.RIGHT_Y), oldRight.stickY)
        assertEquals(PadAxis.NONE, ControllerMap.decode("stick:none", zodiac).stickX.axis)
        // Right stick X, left stick Y upside down; the keys at those axes' ends come off.
        val rightLeft = ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.LEFT)
        val leftUp = ControllerMap.stickKey(ControllerMap.STICK_LEFT, StickDirection.UP)
        val custom = ControllerMap.defaults(zodiac).assign(PalmButton.APP1, rightLeft).assign(PalmButton.APP2, leftUp)
            .withStickAxis(false, PadAxis.RIGHT_X, false).withStickAxis(true, PadAxis.LEFT_Y, true)
        assertEquals(ZodiacStick.CUSTOM, custom.stick)
        assertFalse(rightLeft in custom.keys)
        assertFalse(leftUp in custom.keys)
        assertTrue(custom.encode().endsWith(",stickx:rightx,sticky:-lefty"))
        assertEquals(custom, ControllerMap.decode(custom.encode(), zodiac))
        assertEquals(ZodiacStick.LEFT, ControllerMap.decode("stickx:sideways,sticky:-", zodiac).stick)

        val events = mutableListOf<ZodiacPad.Event>()
        val pad = ZodiacPad(custom) { events += it }
        pad.axes(1, 1f, 0.5f, 0f, 0f, 0f, 0f, rightX = -1f, rightY = 0f)
        assertEquals(listOf(Axis(1, ZodiacPad.AXIS_LEFT_X, -32768), Axis(1, ZodiacPad.AXIS_LEFT_Y, -16383)), events)
        // A trigger can be an axis.
        pad.replaceMap(custom.withStickAxis(false, PadAxis.NONE, true).withStickAxis(true, PadAxis.RIGHT_TRIGGER, false))
        events.clear()
        pad.axes(2, 1f, 1f, 0f, 0f, 0f, 1f)
        assertEquals(listOf(Axis(2, ZodiacPad.AXIS_LEFT_Y, 32767)), events)
        assertEquals(ZodiacStick.LEFT, custom.withStick(ZodiacStick.LEFT).stick)
    }

    @Test fun aReboundDpadDirectionIsSavedAndUsed() {
        // A hat d-pad is assigned while a button waits, as the d-pad key.
        val assigned = mutableListOf<Pair<Int, Boolean>>()
        val hat = HatKeys()
        hat.update(1, 0f, -1f) { key, down -> assigned += key to down }
        hat.update(1, 0f, -1f) { key, down -> assigned += key to down }
        hat.update(1, 0f, 0f) { key, down -> assigned += key to down }
        assertEquals(listOf(KeyEvent.KEYCODE_DPAD_UP to true, KeyEvent.KEYCODE_DPAD_UP to false), assigned)

        // Up goes to Action 2, the key comes off Stick up, and the saved map keeps it.
        val map = ControllerMap.defaults(zodiac).clear(PalmButton.UP).assign(PalmButton.APP2, KeyEvent.KEYCODE_DPAD_UP)
        assertEquals(map, ControllerMap.decode(map.encode(), zodiac))
        assertEquals(listOf(KeyEvent.KEYCODE_DPAD_UP), map.keysFor(PalmButton.APP2).filter { it == KeyEvent.KEYCODE_DPAD_UP })
        assertTrue(map.keysFor(PalmButton.UP).isEmpty())

        val events = mutableListOf<ZodiacPad.Event>()
        val pad = ZodiacPad(map) { events += it }
        pad.axes(1, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_B, true)), events)
        events.clear()
        pad.axes(1, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_B, false)), events)
    }

    @Test fun theRightStickPressesWhatTheMapGivesIt() {
        val events = mutableListOf<ZodiacPad.Event>()
        val map = ControllerMap.defaults(zodiac)
            .assign(PalmButton.APP4, ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.UP))
            .assign(PalmButton.APP1, ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.DOWN))
        val pad = ZodiacPad(map) { events += it }
        pad.axes(1, 0f, 0f, 0f, 0f, 0f, 0f, 0f, -0.9f)
        pad.axes(1, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0.9f)
        pad.axes(1, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_Y, true), Button(1, ZodiacPad.BUTTON_Y, false),
            Button(1, ZodiacPad.BUTTON_A, true), Button(1, ZodiacPad.BUTTON_A, false)), events.filterIsInstance<Button>())
        // It never moves the analogue stick.
        assertTrue(events.filterIsInstance<Axis>().all { it.value == 0 })
    }

    @Test fun stickKeysAndTheHatFollowTheMap() {
        val events = mutableListOf<ZodiacPad.Event>()
        val map = ControllerMap.defaults(zodiac)
            .assign(PalmButton.UP, KeyEvent.KEYCODE_BUTTON_Y)
            .assign(PalmButton.APP3, KeyEvent.KEYCODE_DPAD_LEFT)
        val pad = ZodiacPad(map) { events += it }
        pad.key(1, KeyEvent.KEYCODE_BUTTON_Y, true)
        assertEquals(listOf(Axis(1, ZodiacPad.AXIS_LEFT_Y, -32768)), events)
        events.clear()
        // The hat's left is now Action 3, and its right still pushes the stick.
        pad.axes(1, 0f, 0f, -1f, 0f, 0f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_X, true)), events)
        events.clear()
        pad.axes(1, 0f, 0f, 1f, 0f, 0f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_X, false), Axis(1, ZodiacPad.AXIS_LEFT_X, 32767)), events)
    }

    @Test fun analogueTriggersFollowTheMapsL2AndR2() {
        val events = mutableListOf<ZodiacPad.Event>()
        val pad = ZodiacPad(ControllerMap.defaults(zodiac).assign(PalmButton.APP1, KeyEvent.KEYCODE_BUTTON_L2)) { events += it }
        pad.axes(1, 0f, 0f, 0f, 0f, 0.9f, 0f)
        assertEquals(listOf(Button(1, ZodiacPad.BUTTON_A, true)), events)
    }
}
