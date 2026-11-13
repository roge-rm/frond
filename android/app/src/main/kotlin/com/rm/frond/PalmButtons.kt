package com.rm.frond

import android.util.Log
import java.io.File
import java.util.concurrent.Executors

// Presses Palm buttons for a controller. Hardware buttons go to the core as SDL key events
// (android_main.cpp), the same keys the desktop maps in SdlEventHandler. Silkscreen and
// status bar icons use a one-off pen replay on the control link.
class PalmButtons(private val device: Device, private val link: ControlLink, private val dir: File) {
    private val taps = Executors.newSingleThreadExecutor { Thread(it, "FrondTap").apply { isDaemon = true } }

    fun press(button: PalmButton, down: Boolean) {
        if (button.id !in device.buttons) return
        val key = sdlKey(button, device)
        if (key != 0) {
            push(key, down)
            return
        }
        if (!down) return
        if (tapPoint(device, button, PORTRAIT) == null) return
        taps.execute {
            val orientation = orientation() ?: return@execute
            val (x, y) = tapPoint(device, button, orientation) ?: return@execute
            val file = File(dir, "controller-${button.id}.replay")
            file.writeText("0 pen-down $x $y\n$TAP_MS pen-up\n")
            val reply = link.request("input-replay", file.path, timeoutSeconds = 5)
            if (!reply.ok) Log.w(FrondActivity.TAG, "tap ${button.id}: ${reply.code} ${reply.message}")
        }
    }

    // The guest's own screen orientation, which moves the TX's status bar, or null when the core can't
    // read it. Then there's no tap, as a guess could land inside the running app.
    private fun orientation(): String? {
        if (!device.rotatingStatusBar) return PORTRAIT
        val reply = link.request("guest-orientation", timeoutSeconds = 5)
        if (reply.ok) return reply.message
        Log.w(FrondActivity.TAG, "guest-orientation: ${reply.code} ${reply.message}")
        return null
    }

    companion object {
        // guest-orientation replies.
        const val PORTRAIT = "portrait"
        const val REVERSE_LANDSCAPE = "reverse-landscape"

        // In libmain.so, which SDLActivity loads.
        @JvmStatic external fun push(keycode: Int, down: Boolean)

        // How long the pen stays down for a tap, in ms.
        private const val TAP_MS = 80

        // SDL keycodes (SDL_keycode.h) the core maps to each hardware button.
        private const val SDLK_ESCAPE = 27
        private fun scancodeKey(scancode: Int) = scancode or (1 shl 30)

        fun sdlKey(button: PalmButton, device: Device? = null): Int = when (button) {
            PalmButton.APP1 -> scancodeKey(58) // F1
            PalmButton.APP2 -> scancodeKey(59)
            PalmButton.APP3 -> scancodeKey(60)
            PalmButton.APP4 -> scancodeKey(61)
            PalmButton.RIGHT -> scancodeKey(79)
            PalmButton.LEFT -> scancodeKey(80)
            PalmButton.DOWN -> scancodeKey(81)
            PalmButton.UP -> scancodeKey(82)
            PalmButton.CENTRE -> scancodeKey(119) // SDLK_SELECT
            PalmButton.POWER -> SDLK_ESCAPE
            PalmButton.VOICE -> scancodeKey(62) // F5
            PalmButton.HOME -> device?.homeKey ?: 0
            PalmButton.MENU -> 0
            // Page Up, Page Down, End and Home, which frond-68k turns into the Jog Dial and Back.
            PalmButton.JOG_UP -> scancodeKey(75)
            PalmButton.JOG_DOWN -> scancodeKey(78)
            PalmButton.JOG_PRESS -> scancodeKey(77)
            PalmButton.BACK -> scancodeKey(74)
        }

        // Where Home and Menu are in the core's pen coordinates (screen pixels, the silkscreen below
        // the screen), for the guest's orientation. E2 and Zire 72: the left column of the 320x120
        // silkscreen, the middle of the ROM's Home and Menu areas.
        // TX: Menu is the second icon of the status bar, along the bottom of its 320x480 screen in
        // portrait. The status bar's rotate icon turns it to reverse landscape, which puts the bar
        // along the top, its icons in reverse. Other orientations aren't measured, so no tap.
        fun tapPoint(device: Device, button: PalmButton, orientation: String): Pair<Int, Int>? {
            if (button == PalmButton.HOME && device.homeKey != 0) return null
            if (device.rotatingStatusBar) return when {
                button != PalmButton.MENU -> null
                orientation == PORTRAIT -> device.menuX to device.menuY
                orientation == REVERSE_LANDSCAPE -> device.width - device.menuX to device.height - device.menuY
                else -> null
            }
            return when {
                button == PalmButton.HOME && device.homeX >= 0 -> device.homeX to device.homeY
                button == PalmButton.MENU && device.menuX >= 0 -> device.menuX to device.menuY
                else -> null
            }
        }
    }
}
