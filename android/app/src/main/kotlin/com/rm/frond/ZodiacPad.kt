package com.rm.frond

import android.view.KeyEvent

// Game controllers on the Zodiac. Their buttons and sticks go to the core as SDL game controller
// events, which the core maps as the desktop does (native/Gamepad.h): the left stick is the analogue
// stick, A, B, X and Y the Action keys by place, the shoulders the triggers, Start and Mode Home,
// Select Function and the left stick's click the stick's press. The map says which controller key
// presses which of those; its defaults are that layout. A key mapped to a stick direction pushes the
// stick all the way. The hat counts as the d-pad keys and the analogue triggers as L2 and R2, so they
// follow the map too. The map's stick (left by default, right or none) drives the analogue stick. Either
// stick pushed past half travel also presses what the map gives that direction (ControllerMap.stickKey),
// so the right stick can be the Action keys.
// Not thread-safe: call it from the UI thread.
class ZodiacPad(var map: ControllerMap = ControllerMap.zodiacDefaults(), private val send: (Event) -> Unit) {
    sealed class Event {
        data class Button(val device: Int, val button: Int, val down: Boolean) : Event()
        data class Axis(val device: Int, val axis: Int, val value: Int) : Event()
    }

    // What each device holds: SDL buttons with the keys or axes holding them, stick keys, and the stick's parts.
    private class Pad {
        val holds = mutableMapOf<Int, MutableSet<String>>()
        var dpadX = 0
        var dpadY = 0
        var hatX = 0f
        var hatY = 0f
        var hatKeys = setOf<Int>()
        var stickX = 0f
        var stickY = 0f
        var sentX = 0
        var sentY = 0
    }
    private val pads = mutableMapOf<Int, Pad>()
    private val stickKeys = StickKeys()

    // Returns whether the key is one the map uses.
    fun key(device: Int, key: Int, down: Boolean): Boolean {
        val pad = pads.getOrPut(device) { Pad() }
        val target = map.keys[key] ?: return false
        when (target) {
            PalmButton.LEFT -> pad.dpadX = if (down) -1 else if (pad.dpadX == -1) 0 else pad.dpadX
            PalmButton.RIGHT -> pad.dpadX = if (down) 1 else if (pad.dpadX == 1) 0 else pad.dpadX
            PalmButton.UP -> pad.dpadY = if (down) -1 else if (pad.dpadY == -1) 0 else pad.dpadY
            PalmButton.DOWN -> pad.dpadY = if (down) 1 else if (pad.dpadY == 1) 0 else pad.dpadY
            else -> {
                val button = sdlButton(target) ?: return true
                hold(device, pad, button, "k$key", down)
                return true
            }
        }
        updateStick(device, pad)
        return true
    }

    // Stick, hat and analogue trigger positions for one device: sticks and hats -1 to 1, triggers 0 to 1.
    fun axes(device: Int, stickX: Float, stickY: Float, hatX: Float, hatY: Float, leftTrigger: Float, rightTrigger: Float,
             rightX: Float = 0f, rightY: Float = 0f) {
        val pad = pads.getOrPut(device) { Pad() }
        val values = { axis: PadAxis -> when (axis) {
            PadAxis.NONE -> 0f
            PadAxis.LEFT_X -> stickX
            PadAxis.LEFT_Y -> stickY
            PadAxis.RIGHT_X -> rightX
            PadAxis.RIGHT_Y -> rightY
            PadAxis.LEFT_TRIGGER -> leftTrigger
            PadAxis.RIGHT_TRIGGER -> rightTrigger
        } }
        pad.stickX = values(map.stickX.axis).let { if (map.stickX.invert) -it else it }
        pad.stickY = values(map.stickY.axis).let { if (map.stickY.invert) -it else it }
        stickKeys.update(device, ControllerMap.STICK_LEFT, stickX, stickY) { key, down -> key(device, key, down) }
        stickKeys.update(device, ControllerMap.STICK_RIGHT, rightX, rightY) { key, down -> key(device, key, down) }
        hat(device, pad, hatX, hatY)
        trigger(device, pad, KeyEvent.KEYCODE_BUTTON_L2, leftTrigger)
        trigger(device, pad, KeyEvent.KEYCODE_BUTTON_R2, rightTrigger)
        updateStick(device, pad)
    }

    // The hat as the d-pad keys: a direction mapped to the stick pushes it, one mapped to a button holds it.
    private fun hat(device: Int, pad: Pad, x: Float, y: Float) {
        pad.hatX = 0f
        pad.hatY = 0f
        val now = mutableSetOf<Int>()
        if (x < -ControllerInput.RELEASE) now += KeyEvent.KEYCODE_DPAD_LEFT
        if (x > ControllerInput.RELEASE) now += KeyEvent.KEYCODE_DPAD_RIGHT
        if (y < -ControllerInput.RELEASE) now += KeyEvent.KEYCODE_DPAD_UP
        if (y > ControllerInput.RELEASE) now += KeyEvent.KEYCODE_DPAD_DOWN
        for (key in pad.hatKeys - now) hatKey(device, pad, key, false)
        for (key in now - pad.hatKeys) hatKey(device, pad, key, true)
        pad.hatKeys = now
        for (key in now) when (map.keys[key]) {
            PalmButton.LEFT -> pad.hatX = -1f
            PalmButton.RIGHT -> pad.hatX = 1f
            PalmButton.UP -> pad.hatY = -1f
            PalmButton.DOWN -> pad.hatY = 1f
            else -> {}
        }
    }

    private fun hatKey(device: Int, pad: Pad, key: Int, down: Boolean) {
        val button = sdlButton(map.keys[key] ?: return) ?: return
        hold(device, pad, button, "h$key", down)
    }

    // Lets go of everything one device holds, as when it's unplugged.
    fun release(device: Int) {
        stickKeys.forget(device)
        val pad = pads.remove(device) ?: return
        for (button in pad.holds.keys.sorted()) send(Event.Button(device, button, false))
        if (pad.sentX != 0) send(Event.Axis(device, AXIS_LEFT_X, 0))
        if (pad.sentY != 0) send(Event.Axis(device, AXIS_LEFT_Y, 0))
    }

    fun releaseAll() {
        pads.keys.toList().forEach { release(it) }
    }

    // Swaps the map after letting go of everything held under the old one.
    fun replaceMap(newMap: ControllerMap) {
        releaseAll()
        map = newMap
    }

    // Presses past PRESS and lets go under RELEASE, so a trigger resting near the middle doesn't chatter.
    private fun trigger(device: Int, pad: Pad, key: Int, value: Float) {
        val tag = "t$key"
        val button = sdlButton(map.keys[key] ?: return) ?: return
        val held = pad.holds[button]?.contains(tag) == true
        if (held && value < ControllerInput.RELEASE) hold(device, pad, button, tag, false)
        else if (!held && value > ControllerInput.PRESS) hold(device, pad, button, tag, true)
    }

    private fun hold(device: Int, pad: Pad, button: Int, tag: String, down: Boolean) {
        if (down) {
            val tags = pad.holds.getOrPut(button) { mutableSetOf() }
            if (tags.add(tag) && tags.size == 1) send(Event.Button(device, button, true))
        } else {
            val tags = pad.holds[button] ?: return
            if (!tags.remove(tag)) return
            if (tags.isEmpty()) {
                pad.holds.remove(button)
                send(Event.Button(device, button, false))
            }
        }
    }

    // The d-pad wins over the hat, which wins over the stick.
    private fun updateStick(device: Int, pad: Pad) {
        val x = if (pad.dpadX != 0) pad.dpadX.toFloat() else if (pad.hatX != 0f) pad.hatX else pad.stickX
        val y = if (pad.dpadY != 0) pad.dpadY.toFloat() else if (pad.hatY != 0f) pad.hatY else pad.stickY
        val sx = axisValue(x)
        val sy = axisValue(y)
        if (sx != pad.sentX) send(Event.Axis(device, AXIS_LEFT_X, sx))
        if (sy != pad.sentY) send(Event.Axis(device, AXIS_LEFT_Y, sy))
        pad.sentX = sx
        pad.sentY = sy
    }

    companion object {
        // SDL_GameControllerButton and SDL_GameControllerAxis values.
        const val BUTTON_A = 0
        const val BUTTON_B = 1
        const val BUTTON_X = 2
        const val BUTTON_Y = 3
        const val BUTTON_BACK = 4
        const val BUTTON_GUIDE = 5
        const val BUTTON_START = 6
        const val LEFT_STICK = 7
        const val LEFT_SHOULDER = 9
        const val RIGHT_SHOULDER = 10
        const val AXIS_LEFT_X = 0
        const val AXIS_LEFT_Y = 1

        // The SDL button the core reads for a Zodiac input, or null for the stick's directions.
        fun sdlButton(button: PalmButton): Int? = when (button) {
            PalmButton.APP1 -> BUTTON_A
            PalmButton.APP2 -> BUTTON_B
            PalmButton.APP3 -> BUTTON_X
            PalmButton.APP4 -> BUTTON_Y
            PalmButton.FUNCTION -> BUTTON_BACK
            PalmButton.HOME -> BUTTON_START
            PalmButton.STICK_PRESS -> LEFT_STICK
            PalmButton.TRIGGER_LEFT -> LEFT_SHOULDER
            PalmButton.TRIGGER_RIGHT -> RIGHT_SHOULDER
            else -> null
        }

        // -1 to 1 as SDL's -32768 to 32767.
        fun axisValue(value: Float): Int = if (value <= -1f) -32768 else (value.coerceAtMost(1f) * 32767f).toInt()

        // In libmain.so, which SDLActivity loads. They queue SDL game controller events.
        @JvmStatic external fun pushButton(device: Int, button: Int, down: Boolean)
        @JvmStatic external fun pushAxis(device: Int, axis: Int, value: Int)

        fun push(event: Event) = when (event) {
            is Event.Button -> pushButton(event.device, event.button, event.down)
            is Event.Axis -> pushAxis(event.device, event.axis, event.value)
        }
    }
}
