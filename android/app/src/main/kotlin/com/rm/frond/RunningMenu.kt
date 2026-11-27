package com.rm.frond

import com.rm.frond.RunningSheets.ZaurusUsbMode

// The rows of the running bar's menu for a device, and their states. A 68k device lists only the hardware it has, the others grey out what isn't done yet.
object RunningMenu {
    enum class Id { HOLD, KEYBOARD, GAMEPAD, MICROPHONE, NETWORK, SYNC_MANAGER, DISPLAY, CONNECTIONS, PC_LINK, SCREENSHOT, HIDE_BAR, SAVE_COPY, LOG, RESET, HARD_RESET }

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
        // The CLIE game controller: "attached", "detached", "busy" (the connector is in use) or null until the core says.
        val gamepad: String? = null,
        val zaurusUsbCable: ZaurusUsbMode? = null,
        val zaurusUsbDriveAttached: Boolean? = null,
    )

    fun rows(device: Device, s: State): List<Row> {
        val m68k = device.runs68k
        fun has(present: Boolean) = !m68k || present
        fun onOff(on: Boolean?) = when (on) { true -> "On"; false -> "Off"; null -> null }
        val rows = mutableListOf<Row>()
        if (device.holdPresent) rows += Row(Id.HOLD, "Hold", onOff(s.hold), enabled = device.holdSupported && s.hold != null && !s.busy)
        // The keyboard and beaming share the Palm's infrared port.
        if (has(device.keyboard)) rows += Row(Id.KEYBOARD, "Infrared keyboard", onOff(s.keyboard), enabled = device.keyboard && s.keyboard != null && s.beamOff)
        if (device.clieGamepad) rows += gamepadRow(s)
        if (has(device.microphone)) rows += Row(Id.MICROPHONE, "Microphone", microphoneValue(s.microphone), enabled = device.microphone)
        if (has(device.networkSupported)) rows += Row(Id.NETWORK, "Network", s.network?.label, enabled = device.networkSupported && s.network != null)
        if (SyncProtocol.forDevice(device) != null) rows += Row(Id.SYNC_MANAGER, "Sync Manager…", group = 1)
        rows += Row(Id.DISPLAY, "Settings…", group = 1)
        rows += Row(Id.CONNECTIONS, "Connections…", group = 1)
        if (device.zaurus) {
            val portUse = when {
                s.zaurusUsbDriveAttached == true -> "USB flash drive inserted"
                s.zaurusUsbCable != null && s.zaurusUsbCable != ZaurusUsbMode.OFF -> "USB cable connected"
                else -> null
            }
            rows += Row(Id.PC_LINK, "PC Link…", portUse,
                enabled = !s.busy && ZaurusUsbMode.portFree(s.zaurusUsbCable, s.zaurusUsbDriveAttached), group = 1)
        }
        rows += Row(Id.SCREENSHOT, "Screenshot", group = 1)
        rows += Row(Id.HIDE_BAR, "Hide bar", group = 2)
        rows += Row(Id.SAVE_COPY, "Save a copy…", enabled = !device.zaurus, group = 2)
        rows += Row(Id.LOG, "Emulator log", group = 2)
        rows += Row(Id.RESET, "Reset", group = 3)
        rows += Row(Id.HARD_RESET, "Hard reset…", enabled = device.hardResetWorks, destructive = true, group = 3)
        return rows
    }

    private fun gamepadRow(s: State): Row = when (s.gamepad) {
        "busy" -> Row(Id.GAMEPAD, "Game controller (connector in use)", enabled = false)
        else -> Row(Id.GAMEPAD, "Game controller", when (s.gamepad) { "attached" -> "On"; "detached" -> "Off"; else -> null },
            enabled = s.gamepad != null && !s.busy)
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

    // The core's gamepad-state reply as a menu state: "attached", "detached", "busy", or null when the device has no pad.
    fun gamepadState(reply: String): String? = when {
        reply.startsWith("attached") -> "attached"
        reply.startsWith("detached busy") -> "busy"
        reply.startsWith("detached") -> "detached"
        else -> null
    }
}
