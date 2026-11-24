package com.rm.frond

// The bottom sheets over the running Palm: which ones a device gets and the rows in them, apart from the drawing in
// ui/RunningSheets.kt. A 68k device lists only the hardware it has; the others grey out what isn't done yet.
object RunningSheets {
    enum class Sheet { APPLICATIONS, CARD, DISPLAY, CONNECTIONS, CONTROLS }

    // The sheets with a button on the bar, in order.
    // Storage is on every machine: Internal always, the slot where the device has one.
    @Suppress("UNUSED_PARAMETER")
    fun barSheets(device: Device): List<Sheet> = listOf(Sheet.APPLICATIONS, Sheet.CARD)

    // The sheet a menu row opens, if any.
    fun menuSheet(id: RunningMenu.Id): Sheet? = when (id) {
        RunningMenu.Id.DISPLAY -> Sheet.DISPLAY
        RunningMenu.Id.CONNECTIONS -> Sheet.CONNECTIONS
        else -> null
    }

    fun title(sheet: Sheet, device: Device) = when (sheet) {
        Sheet.APPLICATIONS -> "Applications"
        Sheet.CARD -> "Storage"
        Sheet.DISPLAY -> "Settings"
        Sheet.CONNECTIONS -> "Connections"
        Sheet.CONTROLS -> "Controls"
    }

    // Controller keys can be set from the Display sheet on every device that has Palm buttons to set, as in Settings.
    fun controlsShown(device: Device) = PalmButton.listed(device).isNotEmpty()

    // ---- Applications ----

    enum class AppAction { LAUNCH, EXPORT, DELETE }

    val appActions = listOf(AppAction.LAUNCH, AppAction.EXPORT, AppAction.DELETE)

    // A Palm database name the core takes: not blank, 31 bytes at most.
    fun validDatabaseName(name: String) = name.isNotBlank() && name.trim().toByteArray(Charsets.ISO_8859_1).size <= 31

    // The databases the by-name dialog lists, filtered by what is typed.
    fun matching(all: List<PalmDatabases.Info>, typed: String) = all.filter { it.name.contains(typed.trim(), ignoreCase = true) }

    fun applications(all: List<PalmDatabases.Info>) = all.filter { it.application }

    // ---- Card ----

    enum class CardIcon { IN, OUT, EJECTED, NONE }

    // In the Palm, ejected for the file view, out (the slot is empty and its linked card waits in Cards), or none.
    fun cardIcon(state: SdCard.State, linkedOut: Boolean) = when (state) {
        SdCard.State.INSERTED, SdCard.State.PALM_WIFI -> CardIcon.IN
        SdCard.State.EJECTED -> CardIcon.EJECTED
        SdCard.State.NONE -> if (linkedOut) CardIcon.OUT else CardIcon.NONE
        SdCard.State.UNKNOWN -> CardIcon.NONE
    }

    enum class CardRow { SLOT, LINKED, INSERT_LINKED, FILES, EJECT, INSERT, EXPORT_IMAGE, TRY_AGAIN, WRITE_PROTECT, INSERT_PALM_WIFI, EJECT_PALM_WIFI, UNLINK }

    // linked: the empty slot has a linked card; linkedFree: that card can go in now.
    // `n` is the slot: 1 for the second SD slot. `oldPlace`: a frond-68k card not in Cards yet,
    // which doesn't open in Files. `unlinkable`: the slot is linked to a card, in or out.
    fun cardRows(device: Device, state: SdCard.State, linked: Boolean, linkedFree: Boolean, n: Int = 0, oldPlace: Boolean = false, unlinkable: Boolean = false): List<CardRow> {
        if (!device.cardSupported || !Cards.hasSlot(device, n)) return emptyList()
        if (state == SdCard.State.PALM_WIFI) return listOf(CardRow.SLOT, CardRow.EJECT_PALM_WIFI)
        val rows = mutableListOf(CardRow.SLOT)
        if (state == SdCard.State.NONE && linked) {
            rows += CardRow.LINKED
            if (linkedFree) rows += CardRow.INSERT_LINKED
        }
        when (state) {
            SdCard.State.INSERTED -> rows += listOf(CardRow.FILES, CardRow.EJECT)
            SdCard.State.EJECTED -> {
                rows += CardRow.INSERT
                rows += CardRow.FILES
                rows += CardRow.EXPORT_IMAGE
            }
            SdCard.State.NONE, SdCard.State.PALM_WIFI -> Unit
            SdCard.State.UNKNOWN -> rows += CardRow.TRY_AGAIN
        }
        if (oldPlace) rows -= CardRow.FILES
        if (unlinkable && !oldPlace && state != SdCard.State.UNKNOWN) rows += CardRow.UNLINK
        // frond-68k's stick takes its protection from Cards when it goes in.
        if (((device.memoryStick && !device.runs68k && n == 0) || device.sdWriteProtection) &&
            state != SdCard.State.NONE && state != SdCard.State.UNKNOWN) rows += CardRow.WRITE_PROTECT
        if (n == 0 && device.palmWifiCard && (state == SdCard.State.NONE || state == SdCard.State.EJECTED))
            rows += CardRow.INSERT_PALM_WIFI
        return rows
    }

    // The card row opens the picker when the card state is known and nothing is running.
    fun slotUsable(state: SdCard.State, busy: Boolean, pending: Boolean) = state != SdCard.State.UNKNOWN && state != SdCard.State.PALM_WIFI && !busy && !pending

    // ---- Display and audio ----

    enum class DisplayRow { SIZE, SMOOTH, CONTRAST, SHOW_KEYBOARD, JOG, SPEED, CAMERA }
    enum class AudioRow { SOUND, HEADPHONES, MICROPHONE, AUDIO_PREVIEW, RUMBLE }

    // The groups of the settings sheet, in the desktop's order. Game controller is where the controller keys are set.
    enum class Group(val label: String) { DISPLAY("Display"), AUDIO("Audio"), GAME_CONTROLLER("Game controller"), BATTERY("Battery"), SPEED("Speed") }

    fun groups(device: Device): List<Group> {
        val groups = mutableListOf(Group.DISPLAY, Group.AUDIO)
        if (controlsShown(device)) groups += Group.GAME_CONTROLLER
        groups += Group.BATTERY
        if (DisplayRow.SPEED in displayRows(device)) groups += Group.SPEED
        return groups
    }

    // Speed has its own group, so the Display group lists the rest.
    fun displayRows(device: Device): List<DisplayRow> {
        val rows = mutableListOf(DisplayRow.SIZE, DisplayRow.SMOOTH)
        if (device.lcdContrast) rows += DisplayRow.CONTRAST
        if (device.id == "sony-clie-tg50") rows += DisplayRow.SHOW_KEYBOARD
        if (device.jogControls) rows += DisplayRow.JOG
        rows += DisplayRow.SPEED
        if (device.camera) rows += DisplayRow.CAMERA
        return rows
    }

    fun audioRows(device: Device): List<AudioRow> {
        val rows = mutableListOf(AudioRow.SOUND)
        if (device.headphonesSupported) rows += AudioRow.HEADPHONES
        if (!device.runs68k || device.microphone) rows += AudioRow.MICROPHONE
        if (!device.runs68k) rows += AudioRow.AUDIO_PREVIEW
        if (device.vibrate) rows += AudioRow.RUMBLE
        return rows
    }

    // ---- Connections ----

    enum class ConnRow { BEAM, BLUETOOTH, SERIAL, USB, USB_RETRY, USB_PORT, WIFI_MODULE, DEBUGGER, CRADLE, HOTSYNC }

    fun connectionRows(
        device: Device, usb: NetRelay.Phase, debuggerShown: Boolean,
    ): List<ConnRow> {
        val m68k = device.runs68k
        val rows = mutableListOf(ConnRow.BEAM)
        if (!m68k || device.bluetooth) rows += ConnRow.BLUETOOTH
        if (!device.serialAbsent) rows += ConnRow.SERIAL
        if (!m68k || device.usb) rows += ConnRow.USB
        if (device.usb && !device.usbNetwork && usb == NetRelay.Phase.DISCONNECTED) rows += ConnRow.USB_RETRY
        if (PrintJob.rowShown(device)) rows += ConnRow.USB_PORT
        if (device.builtInWifi) rows += ConnRow.WIFI_MODULE
        if (debuggerShown) rows += ConnRow.DEBUGGER
        if (device.zodiac) rows += ConnRow.CRADLE
        if (device.hotSyncButton) rows += ConnRow.HOTSYNC
        return rows
    }

    // Beaming needs the infrared port and the keyboard off it.
    fun beamUsable(device: Device, keyboard: Boolean?) = device.infrared && keyboard == false
}
