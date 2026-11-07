package com.rm.frond

// The rows of the running bar's menu for a device, and their states. Greyed and hidden rows follow the
// Peripherals screen: a 68k device lists only the hardware it has, the others grey out what isn't done yet.
object RunningMenu {
    enum class Id { HOLD, KEYBOARD, MICROPHONE, NETWORK, DISPLAY, CONNECTIONS, SCREENSHOT, HIDE_BAR, SAVE_COPY, LOG, RESET, HARD_RESET }

    // value is the state shown at the row's end; group sets where the dividers go.
    data class Row(
        val id: Id,
        val label: String,
        val value: String? = null,
        val enabled: Boolean = true,
        val destructive: Boolean = false,
        val group: Int = 0,
    )

    // What the core and the model have said so far; null is "not known yet".
    data class State(
        val hold: Boolean? = null,
        val keyboard: Boolean? = null,
        val beamOff: Boolean = true,
        val microphone: String = "off",
        val network: NetworkMode? = null,
        val busy: Boolean = false,
    )

    fun rows(device: Device, s: State): List<Row> {
        val m68k = device.runs68k
        fun has(present: Boolean) = !m68k || present
        fun onOff(on: Boolean?) = when (on) { true -> "On"; false -> "Off"; null -> null }
        val rows = mutableListOf<Row>()
        if (device.holdPresent) rows += Row(Id.HOLD, "Hold", onOff(s.hold), enabled = device.holdSupported && s.hold != null && !s.busy)
        // The keyboard and beaming share the Palm's infrared port.
        if (has(device.keyboard)) rows += Row(Id.KEYBOARD, "Infrared keyboard", onOff(s.keyboard), enabled = device.keyboard && s.keyboard != null && s.beamOff)
        if (has(device.microphone)) rows += Row(Id.MICROPHONE, "Microphone", microphoneValue(s.microphone), enabled = device.microphone)
        if (has(device.network)) rows += Row(Id.NETWORK, "Network", s.network?.label, enabled = device.network && s.network != null)
        rows += Row(Id.DISPLAY, "Display…", group = 1)
        rows += Row(Id.CONNECTIONS, "Connections…", group = 1)
        rows += Row(Id.SCREENSHOT, "Screenshot", group = 1)
        rows += Row(Id.HIDE_BAR, "Hide bar", group = 2)
        rows += Row(Id.SAVE_COPY, "Save a copy…", group = 2)
        rows += Row(Id.LOG, "Emulator log", group = 2)
        rows += Row(Id.RESET, "Reset", group = 3)
        rows += Row(Id.HARD_RESET, "Hard reset…", destructive = true, group = 3)
        return rows
    }

    // "off Permission denied" says why the microphone is off.
    fun microphoneValue(state: String) = when {
        state == "on" || state == "pending" -> "On"
        state.startsWith("off ") -> state.removePrefix("off ")
        else -> "Off"
    }

    // The file name of a screenshot, from the machine's name and the time as yyyyMMdd-HHmmss.
    fun screenshotName(machine: String, stamp: String) =
        "Frond ${machine.replace(Regex("[\\\\/:*?\"<>|]"), "_").trim().ifEmpty { "Palm" }} $stamp.png"
}
