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
    fun assignTakesTheKeyAndReplacesTheButtonsKeys() {
        val map = ControllerMap.defaults(Device.TX).assign(PalmButton.APP2, KeyEvent.KEYCODE_BUTTON_R1)
        assertEquals(listOf(KeyEvent.KEYCODE_BUTTON_R1), map.keysFor(PalmButton.APP2))
        assertEquals(emptyList<Int>(), map.keysFor(PalmButton.APP4))
        assertFalse(map.keys.containsKey(KeyEvent.KEYCODE_BUTTON_X))
        assertEquals(emptyList<Int>(), map.clear(PalmButton.CENTRE).keysFor(PalmButton.CENTRE))
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
        assertEquals(0, PalmButtons.sdlKey(PalmButton.MENU))
    }
}
