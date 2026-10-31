package com.rm.frond

import android.util.Log
import java.io.File
import java.util.concurrent.Executors

// Presses Palm buttons for a controller. Hardware buttons go to the core as SDL key events
// (android_main.cpp), the same keys the desktop maps in SdlEventHandler. Home and Menu, which are
// silkscreen or status bar icons, are a pen tap sent as a one-off input replay on the control link.
class PalmButtons(private val device: Device, private val link: ControlLink, private val dir: File) {
    private val taps = Executors.newSingleThreadExecutor { Thread(it, "FrondTap").apply { isDaemon = true } }

    fun press(button: PalmButton, down: Boolean) {
        val key = sdlKey(button)
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
        if (device != Device.TX) return PORTRAIT
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

        fun sdlKey(button: PalmButton): Int = when (button) {
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
            PalmButton.HOME, PalmButton.MENU -> 0
        }

        // Where Home and Menu are in the core's pen coordinates (screen pixels, the silkscreen below
        // the screen), for the guest's orientation. E2: the left column of its 320x120 silkscreen.
        // TX: Menu is the second icon of the status bar, along the bottom of its 320x480 screen in
        // portrait. The status bar's rotate icon turns it to reverse landscape, which puts the bar
        // along the top, its icons in reverse. Other orientations aren't measured, so no tap.
        fun tapPoint(device: Device, button: PalmButton, orientation: String): Pair<Int, Int>? = when (device) {
            Device.E2 -> when (button) {
                PalmButton.HOME -> 27 to 351
                PalmButton.MENU -> 27 to 405
                else -> null
            }
            Device.TX -> when {
                button != PalmButton.MENU -> null
                orientation == PORTRAIT -> 54 to 466
                orientation == REVERSE_LANDSCAPE -> 266 to 14
                else -> null
            }
        }
    }
}
