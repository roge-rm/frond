package com.rm.frond

import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.io.IOException

// Where the TG50 jog controls go; arg is the saved name and the core's --jog-controls value.
enum class JogControls(val label: String, val arg: String) {
    HIDDEN("Hidden", "hidden"),
    SCREEN("Left of screen", "screen"),
    KEYBOARD("Left of keyboard", "keyboard"),
}

// CPU engine; arg is the saved name and the core's --engine value. Fast falls back to Exact
// where the core has no JIT.
enum class Engine(val label: String, val arg: String) {
    EXACT("Exact", "exact"),
    FAST("Fast", "fast"),
}

// A 68k device's speed; arg is the saved name and frond-68k's --speed value. Original runs at the real
// clock, Uncapped as fast as the host can with the guest's timers on real time.
enum class CpuSpeed(val label: String, val arg: String) {
    ORIGINAL("Original", "original"),
    UNCAPPED("Uncapped", "uncapped"),
}

// Which per-machine rows Edit settings and the running menu show. A 68k device has contrast and speed
// and no engine or low-latency audio; the engine shows only in debug builds.
data class MachineRows(val contrast: Boolean, val speed: Boolean, val engine: Boolean, val audioPreview: Boolean) {
    companion object {
        fun of(device: Device, debuggable: Boolean) = MachineRows(
            contrast = device.runs68k,
            speed = device.runs68k,
            engine = debuggable && !device.runs68k,
            audioPreview = !device.runs68k,
        )
    }
}

// Preferences owned by one added machine, outside the guest's saved state.
data class MachineSettings(
    val size: ScreenSize = ScreenSize.FIT,
    val smoothing: Boolean = false,
    val sound: Boolean = true,
    val fastAudio: Boolean = false,
    val microphone: Boolean = false,
    val showKeyboard: Boolean = true,
    // New machines put the jog controls left of the keyboard.
    val jogControls: JogControls = JogControls.KEYBOARD,
    val sonyAudioPreview: Boolean = false,
    // Applies at next start.
    val engine: Engine = Engine.EXACT,
    // 68k devices: LCD contrast 0 to 100, and speed (applies at next start).
    val contrast: Int = DEFAULT_CONTRAST,
    // False until a contrast is chosen, so a session's own saved contrast stays.
    val contrastSet: Boolean = false,
    val speed: CpuSpeed = CpuSpeed.ORIGINAL,
) {
    fun json(): JSONObject = JSONObject().put("size", size.name).put("smoothing", smoothing)
        .put("sound", sound).put("fastAudio", fastAudio).put("microphone", microphone)
        .put("showKeyboard", showKeyboard).put("jogControls", jogControls.arg)
        .put("showJogControls", jogControls != JogControls.HIDDEN).put("sonyAudioPreview", sonyAudioPreview)
        .put("engine", engine.arg).put("speed", speed.arg).also { if (contrastSet) it.put("contrast", contrast) }

    fun applyTo(settings: Settings) = settings.copy(size = size, smoothing = smoothing, sound = sound)

    // m68k: frond-68k's arguments, with contrast and speed and no engine.
    fun arguments(soundSupported: Boolean = true, tg50Controls: Boolean = false, m68k: Boolean = false): List<String> = buildList {
        addAll(listOf("--window-fit", this@MachineSettings.size.arg))
        if (smoothing) add("--smooth-lcd")
        if (tg50Controls && !showKeyboard) add("--hide-keyboard")
        if (tg50Controls) addAll(listOf("--jog-controls", jogControls.arg))
        if (!soundSupported || !sound) add("--no-sound")
        if (m68k) {
            if (contrastSet) addAll(listOf("--contrast", contrast.toString()))
            if (speed == CpuSpeed.UNCAPPED) addAll(listOf("--speed", speed.arg))
        } else if (engine == Engine.FAST) {
            addAll(listOf("--engine", engine.arg))
        }
    }

    // The core's display command argument, for changing these while it runs.
    fun displayArgument(tg50Controls: Boolean = false, m68k: Boolean = false): String = buildString {
        append("fit=${this@MachineSettings.size.arg} smooth=${if (smoothing) "on" else "off"}")
        if (tg50Controls) append(" keyboard=${if (showKeyboard) "on" else "off"} jog=${jogControls.arg}")
        if (m68k) append(" contrast=$contrast")
    }

    companion object {
        const val DEFAULT_CONTRAST = 50

        // Settings saved before the choice only had showJogControls: shown was left of the screen.
        private fun jogControls(json: JSONObject): JogControls =
            JogControls.entries.firstOrNull { it.arg == json.optString("jogControls") }
                ?: if (json.optBoolean("showJogControls", true)) JogControls.SCREEN else JogControls.HIDDEN

        fun from(json: JSONObject?, fallback: MachineSettings = MachineSettings()): MachineSettings {
            if (json == null) return fallback
            return MachineSettings(
                size = ScreenSize.entries.firstOrNull { it.name == json.optString("size") } ?: fallback.size,
                smoothing = json.optBoolean("smoothing", fallback.smoothing),
                sound = json.optBoolean("sound", fallback.sound),
                fastAudio = json.optBoolean("fastAudio", fallback.fastAudio),
                microphone = json.optBoolean("microphone", fallback.microphone),
                showKeyboard = json.optBoolean("showKeyboard", true),
                jogControls = jogControls(json),
                sonyAudioPreview = json.optBoolean("sonyAudioPreview", false),
                engine = Engine.entries.firstOrNull { it.arg == json.optString("engine") } ?: fallback.engine,
                contrast = json.optInt("contrast", fallback.contrast).coerceIn(0, 100),
                contrastSet = json.has("contrast") || fallback.contrastSet,
                speed = CpuSpeed.entries.firstOrNull { it.arg == json.optString("speed") } ?: fallback.speed,
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
