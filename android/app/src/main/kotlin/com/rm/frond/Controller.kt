package com.rm.frond

import android.content.Context
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyEvent

// A Palm button a controller can press. The app buttons are named per device.
enum class PalmButton(val id: String) {
    UP("up"), DOWN("down"), LEFT("left"), RIGHT("right"), CENTRE("centre"),
    APP1("app1"), APP2("app2"), APP3("app3"), APP4("app4"),
    // Silkscreen Home on the E2. The TX's Home is its first app button.
    HOME("home"),
    // Silkscreen Menu on the E2, the status bar's Menu on the TX.
    MENU("menu"),
    POWER("power");

    fun label(device: Device): String = when (this) {
        UP -> "Up"
        DOWN -> "Down"
        LEFT -> "Left"
        RIGHT -> "Right"
        CENTRE -> "Centre"
        APP1, APP2, APP3, APP4 -> appNames(device)[ordinal - APP1.ordinal]
        HOME -> "Home"
        MENU -> "Menu"
        POWER -> "Power"
    }

    companion object {
        fun byId(id: String) = entries.firstOrNull { it.id == id }

        // The four app buttons left to right, as the core's DeviceControls.h names them.
        fun appNames(device: Device) = when (device) {
            Device.TX -> listOf("Home", "Calendar", "Contacts", "Web")
            Device.E2 -> listOf("Calendar", "Contacts", "Tasks", "Note Pad")
        }

        // The buttons a device has, in the order the settings list them.
        fun on(device: Device): List<PalmButton> = when (device) {
            Device.TX -> listOf(UP, DOWN, LEFT, RIGHT, CENTRE, APP1, APP2, APP3, APP4, MENU, POWER)
            Device.E2 -> listOf(UP, DOWN, LEFT, RIGHT, CENTRE, APP1, APP2, APP3, APP4, HOME, MENU, POWER)
        }
    }
}

// Which controller key (an Android keycode) presses which Palm button. One key presses one button;
// a button can have more than one key. The d-pad hat and left stick always drive the 5-way.
data class ControllerMap(val keys: Map<Int, PalmButton>) {
    fun keysFor(button: PalmButton): List<Int> = keys.filterValues { it == button }.keys.sorted()

    // Makes key the only key for button, taking it from whatever it pressed before.
    fun assign(button: PalmButton, key: Int): ControllerMap =
        ControllerMap(keys.filterValues { it != button } + (key to button))

    fun clear(button: PalmButton): ControllerMap = ControllerMap(keys.filterValues { it != button })

    fun encode(): String = keys.entries.sortedBy { it.key }.joinToString(",") { "${it.key}:${it.value.id}" }

    companion object {
        private const val FILE = "settings"
        private fun prefKey(device: Device) = "controller.${device.id}"

        // Suits a standard gamepad and handhelds like the Retroid Pocket: d-pad and A for the 5-way,
        // L1, X, Y and R1 for the four app buttons left to right, Start for Home and Select for Menu.
        // B does nothing; Palm OS has no back, and B would otherwise fall back to Android's Back.
        fun defaults(device: Device): ControllerMap {
            val keys = mutableMapOf(
                KeyEvent.KEYCODE_DPAD_UP to PalmButton.UP,
                KeyEvent.KEYCODE_DPAD_DOWN to PalmButton.DOWN,
                KeyEvent.KEYCODE_DPAD_LEFT to PalmButton.LEFT,
                KeyEvent.KEYCODE_DPAD_RIGHT to PalmButton.RIGHT,
                KeyEvent.KEYCODE_DPAD_CENTER to PalmButton.CENTRE,
                KeyEvent.KEYCODE_BUTTON_A to PalmButton.CENTRE,
                KeyEvent.KEYCODE_BUTTON_L1 to PalmButton.APP1,
                KeyEvent.KEYCODE_BUTTON_X to PalmButton.APP2,
                KeyEvent.KEYCODE_BUTTON_Y to PalmButton.APP3,
                KeyEvent.KEYCODE_BUTTON_R1 to PalmButton.APP4,
                KeyEvent.KEYCODE_BUTTON_SELECT to PalmButton.MENU,
            )
            // The TX's Home is a hardware button, its first app button.
            keys[KeyEvent.KEYCODE_BUTTON_START] = if (device == Device.TX) PalmButton.APP1 else PalmButton.HOME
            return ControllerMap(keys)
        }

        // Unknown or malformed entries are skipped; null or blank gives the defaults.
        fun decode(text: String?, device: Device): ControllerMap {
            if (text == null) return defaults(device)
            val allowed = PalmButton.on(device)
            val keys = mutableMapOf<Int, PalmButton>()
            for (entry in text.split(',')) {
                val parts = entry.split(':')
                if (parts.size != 2) continue
                val key = parts[0].toIntOrNull() ?: continue
                val button = PalmButton.byId(parts[1])?.takeIf { it in allowed } ?: continue
                if (key > 0) keys[key] = button
            }
            return ControllerMap(keys)
        }

        fun load(context: Context, device: Device): ControllerMap =
            decode(context.getSharedPreferences(FILE, Context.MODE_PRIVATE).getString(prefKey(device), null), device)

        fun save(context: Context, device: Device, map: ControllerMap) {
            context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit().putString(prefKey(device), map.encode()).apply()
        }

        fun reset(context: Context, device: Device) {
            context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit().remove(prefKey(device)).apply()
        }

        // A short name for a controller key, for the settings list.
        fun keyName(key: Int): String = when (key) {
            KeyEvent.KEYCODE_BUTTON_A -> "A"
            KeyEvent.KEYCODE_BUTTON_B -> "B"
            KeyEvent.KEYCODE_BUTTON_C -> "C"
            KeyEvent.KEYCODE_BUTTON_X -> "X"
            KeyEvent.KEYCODE_BUTTON_Y -> "Y"
            KeyEvent.KEYCODE_BUTTON_Z -> "Z"
            KeyEvent.KEYCODE_BUTTON_L1 -> "L1"
            KeyEvent.KEYCODE_BUTTON_R1 -> "R1"
            KeyEvent.KEYCODE_BUTTON_L2 -> "L2"
            KeyEvent.KEYCODE_BUTTON_R2 -> "R2"
            KeyEvent.KEYCODE_BUTTON_THUMBL -> "L3"
            KeyEvent.KEYCODE_BUTTON_THUMBR -> "R3"
            KeyEvent.KEYCODE_BUTTON_START -> "Start"
            KeyEvent.KEYCODE_BUTTON_SELECT -> "Select"
            KeyEvent.KEYCODE_BUTTON_MODE -> "Mode"
            KeyEvent.KEYCODE_DPAD_UP -> "D-pad up"
            KeyEvent.KEYCODE_DPAD_DOWN -> "D-pad down"
            KeyEvent.KEYCODE_DPAD_LEFT -> "D-pad left"
            KeyEvent.KEYCODE_DPAD_RIGHT -> "D-pad right"
            KeyEvent.KEYCODE_DPAD_CENTER -> "D-pad centre"
            in KeyEvent.KEYCODE_BUTTON_1..KeyEvent.KEYCODE_BUTTON_16 -> "Button ${key - KeyEvent.KEYCODE_BUTTON_1 + 1}"
            else -> "Key $key"
        }

        // Events from a gamepad, joystick or d-pad device, as opposed to a keyboard. A keyboard's
        // arrows often say they come from a d-pad too, so a device with letter keys counts only
        // for gamepad buttons. Back and the system keys stay with Android.
        fun fromController(event: InputEvent): Boolean {
            if (event is KeyEvent) {
                when (event.keyCode) {
                    KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_APP_SWITCH,
                    KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE,
                    KeyEvent.KEYCODE_POWER -> return false
                }
                if (KeyEvent.isGamepadButton(event.keyCode)) return true
            }
            val source = event.source
            if (source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD) return true
            if (source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) return true
            if (source and InputDevice.SOURCE_DPAD == InputDevice.SOURCE_DPAD) {
                return event.device?.keyboardType != InputDevice.KEYBOARD_TYPE_ALPHABETIC
            }
            return false
        }
    }
}

// Turns controller keys and axes into Palm button presses and releases. Each held key or axis
// direction counts as one hold, so a button pressed from two places lets go after the last.
// Not thread-safe: call it from the UI thread.
class ControllerInput(var map: ControllerMap, private val send: (PalmButton, Boolean) -> Unit) {
    // What holds each button: "k<device>:<keycode>" or "a<device>:<button>".
    private val holds = mutableMapOf<String, PalmButton>()

    // Returns whether the key is a controller key this map uses.
    fun key(device: Int, key: Int, down: Boolean): Boolean {
        val tag = "k$device:$key"
        if (down) {
            val button = map.keys[key] ?: return false
            hold(tag, button)
        } else {
            unhold(tag) ?: return map.keys.containsKey(key)
        }
        return true
    }

    // Stick and hat positions from -1 to 1 for one device; the hat wins where it's pressed.
    fun axes(device: Int, stickX: Float, stickY: Float, hatX: Float, hatY: Float) {
        val x = if (hatX != 0f) hatX else stickX
        val y = if (hatY != 0f) hatY else stickY
        direction(device, PalmButton.LEFT, x, -1)
        direction(device, PalmButton.RIGHT, x, 1)
        direction(device, PalmButton.UP, y, -1)
        direction(device, PalmButton.DOWN, y, 1)
    }

    // Presses past PRESS and lets go under RELEASE, so a stick resting near the edge doesn't chatter.
    private fun direction(device: Int, button: PalmButton, value: Float, sign: Int) {
        val tag = "a$device:${button.id}"
        val v = value * sign
        if (tag in holds) {
            if (v < RELEASE) unhold(tag)
        } else if (v > PRESS) {
            hold(tag, button)
        }
    }

    private fun hold(tag: String, button: PalmButton) {
        if (tag in holds) return
        val first = holds.values.none { it == button }
        holds[tag] = button
        if (first) send(button, true)
    }

    private fun unhold(tag: String): PalmButton? {
        val button = holds.remove(tag) ?: return null
        if (holds.values.none { it == button }) send(button, false)
        return button
    }

    // Lets go of everything one device holds, as when it's unplugged.
    fun release(device: Int) {
        holds.keys.filter { it.substring(1).substringBefore(':') == device.toString() }.forEach { unhold(it) }
    }

    fun releaseAll() {
        holds.keys.toList().forEach { unhold(it) }
    }

    val held: Set<PalmButton> get() = holds.values.toSet()

    companion object {
        const val PRESS = 0.5f
        const val RELEASE = 0.35f
    }
}
