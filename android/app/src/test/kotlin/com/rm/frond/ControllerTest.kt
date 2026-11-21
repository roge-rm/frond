package com.rm.frond

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerTest {
    @Test
    fun defaultsForTheTx() {
        val map = ControllerMap.defaults(Device.TX)
        assertEquals(PalmButton.UP, map.keys[KeyEvent.KEYCODE_DPAD_UP])
        assertEquals(PalmButton.CENTRE, map.keys[KeyEvent.KEYCODE_BUTTON_A])
        assertEquals(listOf(KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_START), map.keysFor(PalmButton.APP1))
        assertEquals(PalmButton.APP4, map.keys[KeyEvent.KEYCODE_BUTTON_R1])
        assertEquals(PalmButton.MENU, map.keys[KeyEvent.KEYCODE_BUTTON_SELECT])
        assertFalse(map.keys.containsKey(KeyEvent.KEYCODE_BUTTON_B))
        // Every default is a button the TX has.
        assertTrue(map.keys.values.all { it in PalmButton.on(Device.TX) })
    }

    @Test
    fun defaultsForTheE2() {
        val map = ControllerMap.defaults(Device.E2)
        assertEquals(PalmButton.HOME, map.keys[KeyEvent.KEYCODE_BUTTON_START])
        assertEquals(PalmButton.APP1, map.keys[KeyEvent.KEYCODE_BUTTON_L1])
        assertTrue(map.keys.values.all { it in PalmButton.on(Device.E2) })
        assertEquals("Note Pad", PalmButton.APP4.label(Device.E2))
        assertEquals("Web", PalmButton.APP4.label(Device.TX))
    }

    @Test
    fun defaultsForTheZire() {
        val map = ControllerMap.defaults(Device.ZIRE72)
        assertEquals(PalmButton.HOME, map.keys[KeyEvent.KEYCODE_BUTTON_START])
        assertEquals(PalmButton.MENU, map.keys[KeyEvent.KEYCODE_BUTTON_SELECT])
        assertTrue(map.keys.values.all { it in PalmButton.on(Device.ZIRE72) })
        assertEquals(listOf("Calendar", "Contacts", "Music", "Camera"),
            listOf(PalmButton.APP1, PalmButton.APP2, PalmButton.APP3, PalmButton.APP4).map { it.label(Device.ZIRE72) })
        // Only the Zire has the voice button.
        assertTrue(PalmButton.VOICE in PalmButton.on(Device.ZIRE72))
        assertFalse(PalmButton.VOICE in PalmButton.on(Device.E2))
        assertFalse(PalmButton.VOICE in PalmButton.on(Device.TX))
        val voice = map.assign(PalmButton.VOICE, KeyEvent.KEYCODE_BUTTON_L2)
        assertEquals(voice, ControllerMap.decode(voice.encode(), Device.ZIRE72))
        assertEquals(emptyMap<Int, PalmButton>(), ControllerMap.decode("104:voice", Device.E2).keys)
    }

    @Test
    fun encodeAndDecode() {
        val map = ControllerMap.defaults(Device.E2).assign(PalmButton.POWER, KeyEvent.KEYCODE_BUTTON_B)
        assertEquals(map, ControllerMap.decode(map.encode(), Device.E2))
        assertEquals(ControllerMap.defaults(Device.TX), ControllerMap.decode(null, Device.TX))
        // Saved with nothing assigned stays empty.
        assertEquals(ControllerMap(emptyMap()), ControllerMap.decode("", Device.TX))
    }

    @Test
    fun decodeSkipsBadEntries() {
        val map = ControllerMap.decode("96:centre,junk,x:up,97:nothing,0:up,3:home,19:up", Device.TX)
        // Home isn't a TX button; it's the first app button there.
        assertEquals(mapOf(96 to PalmButton.CENTRE, 19 to PalmButton.UP), map.keys)
    }

    @Test
    fun assignAddsTheKeyAndTakesItFromItsOldButton() {
        val map = ControllerMap.defaults(Device.TX).assign(PalmButton.APP2, KeyEvent.KEYCODE_BUTTON_R1)
        assertEquals(listOf(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_R1), map.keysFor(PalmButton.APP2))
        assertEquals(emptyList<Int>(), map.keysFor(PalmButton.APP4))
        // Assigning a key a button already has changes nothing.
        assertEquals(map, map.assign(PalmButton.APP2, KeyEvent.KEYCODE_BUTTON_X))
        assertEquals(emptyList<Int>(), map.clear(PalmButton.CENTRE).keysFor(PalmButton.CENTRE))
    }

    @Test
    fun removeTakesOffOneKey() {
        val map = ControllerMap.defaults(Device.TX)
        val removed = map.remove(KeyEvent.KEYCODE_BUTTON_L1)
        assertEquals(listOf(KeyEvent.KEYCODE_BUTTON_START), removed.keysFor(PalmButton.APP1))
        assertEquals(map.keys - KeyEvent.KEYCODE_BUTTON_L1, removed.keys)
        assertEquals(removed, removed.remove(KeyEvent.KEYCODE_BUTTON_L1))
    }

    @Test
    fun stickKeysSaveAndName() {
        val up = ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.UP)
        val left = ControllerMap.stickKey(ControllerMap.STICK_LEFT, StickDirection.LEFT)
        assertTrue(ControllerMap.isStickKey(up) && ControllerMap.isStickKey(left))
        assertFalse(ControllerMap.isStickKey(KeyEvent.KEYCODE_BUTTON_A) || ControllerMap.isStickKey(KeyEvent.KEYCODE_BUTTON_16))
        assertEquals("Right stick up", ControllerMap.keyName(up))
        assertEquals("Left stick left", ControllerMap.keyName(left))
        val map = ControllerMap.defaults(Device.TX).assign(PalmButton.APP3, up).assign(PalmButton.APP4, left)
        assertEquals(map, ControllerMap.decode(map.encode(), Device.TX))
        // A map saved before stick keys reads the same.
        val old = ControllerMap.defaults(Device.TX)
        assertEquals(old, ControllerMap.decode(old.encode(), Device.TX))
        // Eight stick keys, all different.
        val all = (0..1).flatMap { s -> StickDirection.entries.map { ControllerMap.stickKey(s, it) } }
        assertEquals(8, all.toSet().size)
    }

    @Test
    fun stickDirectionsPressWhatTheMapGivesThem() {
        val rightUp = ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.UP)
        val rightLeft = ControllerMap.stickKey(ControllerMap.STICK_RIGHT, StickDirection.LEFT)
        val leftDown = ControllerMap.stickKey(ControllerMap.STICK_LEFT, StickDirection.DOWN)
        val (c, sent) = input(ControllerMap(mapOf(rightUp to PalmButton.APP1, rightLeft to PalmButton.APP2, leftDown to PalmButton.MENU)))
        c.axes(1, 0f, 0f, 0f, 0f, 0f, -0.4f)
        assertEquals(emptyList<String>(), sent)
        c.axes(1, 0f, 0f, 0f, 0f, 0f, -0.6f)
        assertEquals(listOf("+app1"), sent)
        // Held between the release and press points stays down.
        c.axes(1, 0f, 0f, 0f, 0f, -0.7f, -0.4f)
        assertEquals(listOf("+app1", "+app2"), sent)
        c.axes(1, 0f, 0f, 0f, 0f, 0f, -0.2f)
        assertEquals(listOf("+app1", "+app2", "-app2", "-app1"), sent)
        // The left stick's direction presses its key as well as the 5-way.
        sent.clear()
        c.axes(1, 0f, 0.9f, 0f, 0f)
        assertEquals(setOf("+down", "+menu"), sent.toSet())
        c.release(1)
        assertTrue(c.held.isEmpty())
    }

    @Test
    fun keyNames() {
        assertEquals("A", ControllerMap.keyName(KeyEvent.KEYCODE_BUTTON_A))
        assertEquals("Start", ControllerMap.keyName(KeyEvent.KEYCODE_BUTTON_START))
        assertEquals("D-pad up", ControllerMap.keyName(KeyEvent.KEYCODE_DPAD_UP))
        assertEquals("Button 3", ControllerMap.keyName(KeyEvent.KEYCODE_BUTTON_3))
    }

    private fun input(map: ControllerMap = ControllerMap.defaults(Device.TX)): Pair<ControllerInput, MutableList<String>> {
        val sent = mutableListOf<String>()
        return ControllerInput(map) { b, down -> sent += (if (down) "+" else "-") + b.id } to sent
    }

    @Test
    fun keysPressAndRelease() {
        val (c, sent) = input()
        assertTrue(c.key(1, KeyEvent.KEYCODE_BUTTON_A, true))
        assertTrue(c.key(1, KeyEvent.KEYCODE_BUTTON_A, false))
        assertFalse(c.key(1, KeyEvent.KEYCODE_BUTTON_B, true))
        assertEquals(listOf("+centre", "-centre"), sent)
    }

    @Test
    fun twoKeysOnOneButtonReleaseAfterTheLast() {
        val (c, sent) = input()
        c.key(1, KeyEvent.KEYCODE_BUTTON_A, true)
        c.key(1, KeyEvent.KEYCODE_DPAD_CENTER, true)
        c.key(1, KeyEvent.KEYCODE_BUTTON_A, false)
        assertEquals(listOf("+centre"), sent)
        c.key(1, KeyEvent.KEYCODE_DPAD_CENTER, false)
        assertEquals(listOf("+centre", "-centre"), sent)
    }

    @Test
    fun releaseKeepsWorkingAfterTheMapChanges() {
        val (c, sent) = input()
        c.key(1, KeyEvent.KEYCODE_BUTTON_X, true)
        c.map = ControllerMap(emptyMap())
        c.key(1, KeyEvent.KEYCODE_BUTTON_X, false)
        assertEquals(listOf("+app2", "-app2"), sent)
    }

    @Test
    fun stickWithHysteresis() {
        val (c, sent) = input()
        c.axes(1, 0.6f, 0f, 0f, 0f)
        c.axes(1, 0.4f, 0f, 0f, 0f)
        assertEquals(listOf("+right"), sent)
        c.axes(1, 0.3f, 0f, 0f, 0f)
        c.axes(1, 0f, -0.9f, 0f, 0f)
        assertEquals(listOf("+right", "-right", "+up"), sent)
        // The hat wins over the stick.
        c.axes(1, 0f, -0.9f, 0f, 1f)
        assertEquals(listOf("+right", "-right", "+up", "-up", "+down"), sent)
    }

    @Test
    fun releaseAllAndPerDevice() {
        val (c, sent) = input()
        c.key(1, KeyEvent.KEYCODE_BUTTON_A, true)
        c.axes(2, -1f, 0f, 0f, 0f)
        c.key(12, KeyEvent.KEYCODE_BUTTON_Y, true)
        c.release(1)
        assertEquals(listOf("+centre", "+left", "+app3", "-centre"), sent)
        c.releaseAll()
        assertEquals(setOf<PalmButton>(), c.held)
        assertEquals(setOf("-left", "-app3"), sent.drop(4).toSet())
    }

    @Test
    fun sdlKeysMatchTheCoreMap() {
        assertEquals(0x4000003A, PalmButtons.sdlKey(PalmButton.APP1))
        assertEquals(0x40000052, PalmButtons.sdlKey(PalmButton.UP))
        assertEquals(0x40000077, PalmButtons.sdlKey(PalmButton.CENTRE))
        assertEquals(27, PalmButtons.sdlKey(PalmButton.POWER))
        // F5, the core's keyIdVoice.
        assertEquals(0x4000003E, PalmButtons.sdlKey(PalmButton.VOICE))
        assertEquals(0, PalmButtons.sdlKey(PalmButton.MENU))
    }
}
