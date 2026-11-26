package com.rm.frond

import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.io.IOException

// Where the CLIE jog controls go; arg is the saved name and the core's --jog-controls value.
enum class JogControls(val label: String, val arg: String) {
    HIDDEN("Hidden", "hidden"),
    SCREEN("Left of screen", "screen"),
    KEYBOARD("Left of keyboard", "keyboard"),
    ;

    // A CLIE without a keyboard puts KEYBOARD's jog controls beside its silkscreen and buttons.
    fun label(besideButtons: Boolean) = if (this == KEYBOARD && besideButtons) "Beside the buttons" else label
}

// Where the CLIE jog controls go in landscape with the game controller; arg is the saved name and
// the core's --jog-controls-landscape value.
enum class JogLandscape(val label: String, val arg: String) {
    HIDDEN("Hidden", "hidden"),
    BESIDE("Left of controller", "beside"),
    UNDER("Under screen", "under"),
    OVER("Over screen", "over"),
}

// CPU engine; arg is the saved name and the core's --engine value. Fast falls back to Exact
// where the core has no JIT.
enum class Engine(val label: String, val arg: String) {
    EXACT("Exact", "exact"),
    FAST("Fast", "fast"),
}

// A device's speed; arg is the saved name and both cores' --speed value. Original runs at the real
// clock (an ARM device follows the clock its guest sets), a multiplier at that many times it, Uncapped
// as fast as the host can. An older Frond reads a multiplier as Original.
enum class CpuSpeed(val label: String, val arg: String) {
    ORIGINAL("Original", "original"),
    X1_5("1.5x", "1.5x"),
    X2("2x", "2x"),
    X3("3x", "3x"),
    X4("4x", "4x"),
    X8("8x", "8x"),
    UNCAPPED("Uncapped", "uncapped");

    companion object {
        // The 68k core has no 8x; its Uncapped is the same speed.
        fun options(m68k: Boolean) = entries.filter { !(m68k && it == X8) }

        // What the core is given for this device.
        fun forCore(speed: CpuSpeed, m68k: Boolean) = if (m68k && speed == X8) UNCAPPED else speed
    }
}

// Zodiac: where the games' music goes; arg is the saved name. The game's own FM sound, a SoundFont the core plays in
// the app, or a MIDI device through MidiManager.
enum class MusicOutput(val label: String, val arg: String) {
    FM("Built-in FM Synth", "fm"),
    SOUNDFONT("SoundFont", "soundfont"),
    MIDI("MIDI device", "midi"),
}

// Android: whether the bar over the running machine hides by itself over the Palm or takes its own space.
// arg is the saved name.
enum class BarMode(val label: String, val arg: String) {
    AUTO_HIDE("Auto-hide", "autohide"),
    OWN_SPACE("Own space", "space"),
}

// The phone key held to record on a device with a voice or record key; arg is the saved name. The key is only
// used while that machine runs. keyCode is an Android KeyEvent code, 0 for none.
enum class RecordKey(val label: String, val arg: String, val keyCode: Int) {
    NONE("None", "none", 0),
    VOLUME_DOWN("Volume down", "volume-down", android.view.KeyEvent.KEYCODE_VOLUME_DOWN),
    VOLUME_UP("Volume up", "volume-up", android.view.KeyEvent.KEYCODE_VOLUME_UP),
    L3("Gamepad L3", "l3", android.view.KeyEvent.KEYCODE_BUTTON_THUMBL),
    SELECT("Gamepad Select", "select", android.view.KeyEvent.KEYCODE_BUTTON_SELECT),
    ;

    fun matches(code: Int) = keyCode != 0 && code == keyCode

    companion object {
        fun byArg(arg: String?) = entries.firstOrNull { it.arg == arg } ?: NONE
    }
}

// Which per-machine rows Edit settings and the running menu show. Every device has speed. A 68k device
// has contrast (on a grey LCD) and no engine or low-latency audio.
data class MachineRows(val contrast: Boolean, val speed: Boolean, val engine: Boolean, val audioPreview: Boolean) {
    companion object {
        fun of(device: Device) = MachineRows(
            contrast = device.lcdContrast,
            speed = true,
            engine = !device.runs68k,
            audioPreview = !device.runs68k,
        )
    }
}

// Preferences owned by one added machine, outside the guest's saved state.
data class MachineSettings(
    val size: ScreenSize = ScreenSize.FIT,
    val smoothing: Boolean = false,
    // ARM devices: a speed readout in the corner of the screen.
    val showMips: Boolean = false,
    val sound: Boolean = true,
    val fastAudio: Boolean = false,
    val microphone: Boolean = false,
    val showKeyboard: Boolean = true,
    // New machines put the jog controls left of the keyboard.
    val jogControls: JogControls = JogControls.KEYBOARD,
    // CLIE with the game controller: the jog controls in landscape.
    val jogLandscape: JogLandscape = JogLandscape.BESIDE,
    // Applies at next start.
    val engine: Engine = Engine.EXACT,
    // 68k devices: LCD contrast 0 to 100. Speed is for every device and applies at once.
    val contrast: Int = DEFAULT_CONTRAST,
    // False until a contrast is chosen, so a session's own saved contrast stays.
    val contrastSet: Boolean = false,
    val speed: CpuSpeed = CpuSpeed.ORIGINAL,
    // Android: whether the bar over the running machine shows.
    val barShown: Boolean = true,
    val barMode: BarMode = BarMode.AUTO_HIDE,
    // Infrared: off, phone (the core's own mobile phone), printer (an IrDA printer), or link:<id> for a connection (InfraredChoice). Applied at every start.
    val infrared: String = InfraredChoice.OFF,
    // Serial: off, a connection, a snap-on modem or Sync Manager.
    val serial: String = InfraredChoice.OFF,
    // Bluetooth: off, radio (This Frond's radio) or link:<id> (BluetoothChoice). Applied at every start.
    val bluetooth: String = BluetoothChoice.OFF,
    val danaView: DanaView = DanaView(),
    // Dana: what is on the USB host port, off, printer or wifi (applied at every start).
    val usbPort: String = UsbPort.OFF,
    // Dana Wireless: the built-in Wi-Fi module's radio (applied at every start).
    val wifiModule: Boolean = true,
    // The guest motor drives the controller's or the phone's vibrator.
    val rumble: Boolean = true,
    // Zodiac in Fast: the ROM takes each stick sample as it is (the core's --direct-stick).
    val directStick: Boolean = true,
    // CLIE TG50: the game controller on the connector. null leaves what the session has (nothing was chosen yet).
    val gamepad: Boolean? = null,
    // The battery the Palm reads: Maximum, a set level (0 to 100), or this phone's. Charging is the Palm on its charger,
    // used by Maximum and Set level. Applied live and at every start.
    val batteryMode: BatteryMode = BatteryMode.MAX,
    val batteryLevel: Int = DEFAULT_BATTERY_LEVEL,
    val batteryCharging: Boolean = true,
    val headphonesIn: Boolean = false,
    // Zodiac: Music output, the SoundFont's copy in the app's files (SoundFonts) and the MIDI device's name. With
    // nothing picked the game's own sound plays. Applied at every start.
    val musicOutput: MusicOutput = MusicOutput.FM,
    val soundFont: String = "",
    val midiDevice: String = "",
    // A device with a voice or record key: the phone key held to record.
    val recordKey: RecordKey = RecordKey.NONE,
) {
    fun json(): JSONObject = JSONObject().put("size", size.name).put("smoothing", smoothing).put("showMips", showMips)
        .put("sound", sound).put("fastAudio", fastAudio).put("microphone", microphone)
        .put("showKeyboard", showKeyboard).put("jogControls", jogControls.arg)
        .put("showJogControls", jogControls != JogControls.HIDDEN).put("jogControlsLandscape", jogLandscape.arg)
        .put("engine", engine.arg).put("speed", speed.arg).put("barShown", barShown).put("barMode", barMode.arg).put("infrared", infrared).put("serial", serial).put("bluetooth", bluetooth).put("danaView", danaView.json()).put("usbPort", usbPort).put("usbPrinter", usbPort == UsbPort.PRINTER).put("wifiModule", wifiModule).put("rumble", rumble).also { if (contrastSet) it.put("contrast", contrast); gamepad?.let { on -> it.put("gamepad", on) }
        // Saved only when not the default.
        if (batteryMode != BatteryMode.MAX) it.put("batteryMode", batteryMode.arg)
        if (batteryLevel != DEFAULT_BATTERY_LEVEL) it.put("batteryLevel", batteryLevel)
        if (!batteryCharging) it.put("batteryCharging", false)
        if (headphonesIn) it.put("headphonesIn", true)
        if (!directStick) it.put("directStick", false)
        if (musicOutput != MusicOutput.FM) it.put("musicOutput", musicOutput.arg)
        if (soundFont.isNotEmpty()) it.put("soundFont", soundFont)
        if (midiDevice.isNotEmpty()) it.put("midiDevice", midiDevice)
        if (recordKey != RecordKey.NONE) it.put("recordKey", recordKey.arg) }

    fun applyTo(settings: Settings) = settings.copy(size = size, smoothing = smoothing, sound = sound)

    // m68k: frond-68k's arguments, with contrast and speed and no engine. both: --speed unless Original.
    // tg50Controls: Show keyboard and the jog controls; jogControls: the jog controls on their own.
    fun arguments(soundSupported: Boolean = true, tg50Controls: Boolean = false, m68k: Boolean = false, colourLcd: Boolean = false,
                  jogControls: Boolean = tg50Controls, gamepadSupported: Boolean = false, touchHidden: Boolean = false,
                  headphonesSupported: Boolean = false): List<String> = buildList {
        addAll(listOf("--window-fit", this@MachineSettings.size.arg))
        if (gamepadSupported) this@MachineSettings.gamepad?.let { addAll(listOf("--gamepad", if (it) "on" else "off")) }
        if (smoothing) add("--smooth-lcd")
        if (tg50Controls && !showKeyboard) add("--hide-keyboard")
        if (jogControls) addAll(listOf("--jog-controls", if (touchHidden) JogControls.HIDDEN.arg else this@MachineSettings.jogControls.arg))
        if (gamepadSupported) addAll(listOf("--jog-controls-landscape", if (touchHidden) JogLandscape.HIDDEN.arg else jogLandscape.arg))
        if (touchHidden) add("--hide-buttons")
        if (!soundSupported || !sound) add("--no-sound")
        if (headphonesSupported && headphonesIn) add("--headphones-in")
        if (m68k) {
            if (contrastSet && !colourLcd) addAll(listOf("--contrast", contrast.toString()))
        } else if (engine == Engine.FAST) addAll(listOf("--engine", engine.arg))
        if (speed != CpuSpeed.ORIGINAL) addAll(listOf("--speed", CpuSpeed.forCore(speed, m68k).arg))
    }

    // The Zodiac's Direct stick for the core, used only with the Fast engine.
    fun directStickArguments(): List<String> =
        if (engine == Engine.FAST && directStick) listOf("--direct-stick") else emptyList()

    // The Zodiac's Music output for the core: a SoundFont that is there, or a MIDI device the activity found
    // (midiDeviceId, its MidiManager id). Nothing for the game's own sound or a choice with nothing to use.
    fun musicArguments(soundFontThere: Boolean, midiDeviceId: Int?): List<String> = when {
        musicOutput == MusicOutput.SOUNDFONT && soundFont.isNotEmpty() && soundFontThere -> listOf("--music-soundfont", soundFont)
        musicOutput == MusicOutput.MIDI && midiDeviceId != null -> ZodiacMidiChoice.arguments(midiDeviceId)
        else -> emptyList()
    }

    // Audio arguments: the host period always, so low-latency limits are sized for Android's
    // bursty output like normal audio, plus --fast-audio on qualified firmware (qualified reads
    // the NOR, so it's only called with low latency on) or --low-latency-queue.
    fun audioArguments(lowLatencyAvailable: Boolean, period: Int, qualified: () -> Boolean): List<String> = buildList {
        if (fastAudio && lowLatencyAvailable) add(if (qualified()) "--fast-audio" else "--low-latency-queue")
        addAll(listOf("--audio-period", period.toString()))
    }

    // The core's display command argument, for changing these while it runs.
    fun displayArgument(tg50Controls: Boolean = false, m68k: Boolean = false, colourLcd: Boolean = false,
                        jogControls: Boolean = tg50Controls, gamepadSupported: Boolean = false, touchHidden: Boolean = false): String = buildString {
        append("fit=${this@MachineSettings.size.arg} smooth=${if (smoothing) "on" else "off"}")
        if (tg50Controls) append(" keyboard=${if (showKeyboard) "on" else "off"}")
        if (jogControls) append(" jog=${if (touchHidden) JogControls.HIDDEN.arg else this@MachineSettings.jogControls.arg}")
        if (gamepadSupported) append(" jog-landscape=${if (touchHidden) JogLandscape.HIDDEN.arg else jogLandscape.arg}")
        if (m68k && !colourLcd) append(" contrast=$contrast")
        append(" buttons=${if (touchHidden) "off" else "on"}")
    }

    // The pad state the core reports, as the setting, so a session loaded with its own state starts
    // with it next time, as on the desktop. Null when nothing changes: busy or unknown, or the setting
    // already matches.
    fun gamepadFromCore(state: String?): MachineSettings? {
        val attached = when (state) { "attached" -> true; "detached" -> false; else -> return null }
        return if (gamepad == attached) null else copy(gamepad = attached)
    }

    fun forDevice(from: Device, to: Device): MachineSettings =
        copy(
            engine = if (engine == from.defaultEngine) to.defaultEngine else engine,
            serial = when {
                serial == InfraredChoice.PPP && !(to.runs68k && to.serialPort) -> InfraredChoice.OFF
                serial == InfraredChoice.KEYBOARD && !to.serialKeyboard -> InfraredChoice.OFF
                else -> serial
            },
        )

    companion object {
        const val DEFAULT_CONTRAST = 50
        const val DEFAULT_BATTERY_LEVEL = 100

        // A new machine's settings for a device.
        fun defaults(device: Device) = MachineSettings(engine = device.defaultEngine)

        // Settings saved before the choice only had showJogControls: shown was left of the screen.
        private fun jogControls(json: JSONObject): JogControls =
            JogControls.entries.firstOrNull { it.arg == json.optString("jogControls") }
                ?: if (json.optBoolean("showJogControls", true)) JogControls.SCREEN else JogControls.HIDDEN

        fun from(json: JSONObject?, fallback: MachineSettings = MachineSettings()): MachineSettings {
            if (json == null) return fallback
            return MachineSettings(
                size = ScreenSize.entries.firstOrNull { it.name == json.optString("size") } ?: fallback.size,
                smoothing = json.optBoolean("smoothing", fallback.smoothing),
                showMips = json.optBoolean("showMips", fallback.showMips),
                sound = json.optBoolean("sound", fallback.sound),
                fastAudio = json.optBoolean("fastAudio", fallback.fastAudio),
                microphone = json.optBoolean("microphone", fallback.microphone),
                showKeyboard = json.optBoolean("showKeyboard", true),
                jogControls = jogControls(json),
                jogLandscape = JogLandscape.entries.firstOrNull { it.arg == json.optString("jogControlsLandscape") } ?: fallback.jogLandscape,
                engine = Engine.entries.firstOrNull { it.arg == json.optString("engine") } ?: fallback.engine,
                contrast = json.optInt("contrast", fallback.contrast).coerceIn(0, 100),
                contrastSet = json.has("contrast") || fallback.contrastSet,
                speed = CpuSpeed.entries.firstOrNull { it.arg == json.optString("speed") } ?: fallback.speed,
                barShown = json.optBoolean("barShown", fallback.barShown),
                barMode = BarMode.entries.firstOrNull { it.arg == json.optString("barMode") } ?: fallback.barMode,
                infrared = InfraredChoice.normaliseInfrared(json.optString("infrared", fallback.infrared)),
                serial = InfraredChoice.normaliseSerial(json.optString("serial", fallback.serial)),
                bluetooth = BluetoothChoice.normalise(json.optString("bluetooth", fallback.bluetooth)),
                danaView = DanaView.from(json.optJSONObject("danaView"), fallback.danaView),
                usbPort = if (json.has("usbPort")) UsbPort.normalise(json.optString("usbPort"))
                    else if (json.has("usbPrinter")) UsbPort.normalise(null, json.optBoolean("usbPrinter")) else fallback.usbPort,
                wifiModule = json.optBoolean("wifiModule", fallback.wifiModule),
                rumble = json.optBoolean("rumble", fallback.rumble),
                directStick = json.optBoolean("directStick", fallback.directStick),
                gamepad = if (json.has("gamepad")) json.optBoolean("gamepad") else fallback.gamepad,
                batteryMode = BatteryMode.entries.firstOrNull { it.arg == json.optString("batteryMode") } ?: BatteryMode.MAX,
                batteryLevel = json.optInt("batteryLevel", DEFAULT_BATTERY_LEVEL).coerceIn(0, 100),
                batteryCharging = json.optBoolean("batteryCharging", true),
                headphonesIn = json.optBoolean("headphonesIn", false),
                musicOutput = MusicOutput.entries.firstOrNull { it.arg == json.optString("musicOutput") } ?: MusicOutput.FM,
                soundFont = json.optString("soundFont", ""),
                midiDevice = json.optString("midiDevice", ""),
                recordKey = RecordKey.byArg(json.optString("recordKey")),
            )
        }
    }
}

// The Home and emulator processes share a lock and replace metadata atomically. Keep the lock file
// in place so another process cannot lock an unlinked inode. Updates read the latest sidecar.
internal object SessionMetadata {
    @Synchronized
    fun <T> locked(file: File, work: () -> T): T {
        file.parentFile!!.mkdirs()
        return RandomAccessFile(File(file.parentFile, ".metadata.lock"), "rw").use { lock ->
            lock.channel.lock().use { work() }
        }
    }

    fun read(file: File, defaults: () -> MachineSettings): JSONObject = locked(file) {
        val json = JSONObject(file.readText())
        if (!json.has("settings")) {
            json.put("settings", defaults().json())
            writeUnlocked(file, json)
        }
        json
    }

    fun write(file: File, json: JSONObject) = locked(file) { writeUnlocked(file, json) }

    fun update(file: File, change: (JSONObject) -> Unit) = locked(file) {
        val json = JSONObject(file.readText())
        change(json)
        writeUnlocked(file, json)
    }

    private fun writeUnlocked(file: File, json: JSONObject) {
        val temporary = File(file.path + ".tmp")
        try {
            temporary.outputStream().use { output ->
                output.write(json.toString().toByteArray(Charsets.UTF_8))
                output.fd.sync()
            }
            if (!temporary.renameTo(file)) throw IOException("can't write ${file.name}")
        } finally {
            temporary.delete()
        }
    }
}
