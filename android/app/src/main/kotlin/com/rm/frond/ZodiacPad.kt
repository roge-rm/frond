package com.rm.frond

import android.view.KeyEvent

// Game controllers on the Zodiac. Their buttons and sticks go to the core as SDL game controller
// events, which the core maps as the desktop does (native/Gamepad.h): the left stick is the analogue
// stick, A, B, X and Y the Action keys by place, the shoulders the triggers, Start and Mode Home,
// Select Function and the left stick's click the stick's press. The d-pad and the hat push the stick
// all the way, and the analogue triggers and L2 and R2 work as the shoulders.
// Not thread-safe: call it from the UI thread.
class ZodiacPad(private val send: (Event) -> Unit) {
    sealed class Event {
        data class Button(val device: Int, val button: Int, val down: Boolean) : Event()
        data class Axis(val device: Int, val axis: Int, val value: Int) : Event()
    }

    // What each device holds: SDL buttons with the keys or axes holding them, d-pad keys, and the stick's parts.
    private class Pad {
        val holds = mutableMapOf<Int, MutableSet<String>>()
        var dpadX = 0
        var dpadY = 0
        var hatX = 0f
        var hatY = 0f
        var stickX = 0f
        var stickY = 0f
        var sentX = 0
        var sentY = 0
    }
    private val pads = mutableMapOf<Int, Pad>()

    // Returns whether the key is one the Zodiac uses.
    fun key(device: Int, key: Int, down: Boolean): Boolean {
        val pad = pads.getOrPut(device) { Pad() }
        when (key) {
            KeyEvent.KEYCODE_DPAD_LEFT -> pad.dpadX = if (down) -1 else if (pad.dpadX == -1) 0 else pad.dpadX
            KeyEvent.KEYCODE_DPAD_RIGHT -> pad.dpadX = if (down) 1 else if (pad.dpadX == 1) 0 else pad.dpadX
            KeyEvent.KEYCODE_DPAD_UP -> pad.dpadY = if (down) -1 else if (pad.dpadY == -1) 0 else pad.dpadY
            KeyEvent.KEYCODE_DPAD_DOWN -> pad.dpadY = if (down) 1 else if (pad.dpadY == 1) 0 else pad.dpadY
            else -> {
                val button = BUTTONS[key] ?: return false
                hold(device, pad, button, "k$key", down)
                return true
            }
        }
        updateStick(device, pad)
        return true
    }

    // Stick, hat and analogue trigger positions for one device: sticks and hats -1 to 1, triggers 0 to 1.
    fun axes(device: Int, stickX: Float, stickY: Float, hatX: Float, hatY: Float, leftTrigger: Float, rightTrigger: Float) {
        val pad = pads.getOrPut(device) { Pad() }
        pad.stickX = stickX
        pad.stickY = stickY
        pad.hatX = hatX
        pad.hatY = hatY
        trigger(device, pad, LEFT_SHOULDER, "l", leftTrigger)
        trigger(device, pad, RIGHT_SHOULDER, "r", rightTrigger)
        updateStick(device, pad)
    }

    // Lets go of everything one device holds, as when it's unplugged.
    fun release(device: Int) {
        val pad = pads.remove(device) ?: return
        for (button in pad.holds.keys.sorted()) send(Event.Button(device, button, false))
        if (pad.sentX != 0) send(Event.Axis(device, AXIS_LEFT_X, 0))
        if (pad.sentY != 0) send(Event.Axis(device, AXIS_LEFT_Y, 0))
    }

    fun releaseAll() {
        pads.keys.toList().forEach { release(it) }
    }

    // Presses past PRESS and lets go under RELEASE, so a trigger resting near the middle doesn't chatter.
    private fun trigger(device: Int, pad: Pad, button: Int, tag: String, value: Float) {
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

        private val BUTTONS = mapOf(
            KeyEvent.KEYCODE_BUTTON_A to BUTTON_A,
            KeyEvent.KEYCODE_DPAD_CENTER to BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B to BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X to BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y to BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_SELECT to BUTTON_BACK,
            KeyEvent.KEYCODE_BUTTON_MODE to BUTTON_GUIDE,
            KeyEvent.KEYCODE_BUTTON_START to BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL to LEFT_STICK,
            KeyEvent.KEYCODE_BUTTON_L1 to LEFT_SHOULDER,
            KeyEvent.KEYCODE_BUTTON_L2 to LEFT_SHOULDER,
            KeyEvent.KEYCODE_BUTTON_R1 to RIGHT_SHOULDER,
            KeyEvent.KEYCODE_BUTTON_R2 to RIGHT_SHOULDER,
        )

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
