package com.rm.frond

import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.io.IOException

// Preferences owned by one added machine, outside the guest's saved state.
data class MachineSettings(
    val size: ScreenSize = ScreenSize.FIT,
    val smoothing: Boolean = false,
    val sound: Boolean = true,
    val fastAudio: Boolean = false,
    val microphone: Boolean = false,
    val showKeyboard: Boolean = true,
    val showJogControls: Boolean = true,
    val sonyAudioPreview: Boolean = false,
) {
    fun json(): JSONObject = JSONObject().put("size", size.name).put("smoothing", smoothing)
        .put("sound", sound).put("fastAudio", fastAudio).put("microphone", microphone)
        .put("showKeyboard", showKeyboard).put("showJogControls", showJogControls).put("sonyAudioPreview", sonyAudioPreview)

    fun applyTo(settings: Settings) = settings.copy(size = size, smoothing = smoothing, sound = sound)

    fun arguments(soundSupported: Boolean = true, tg50Controls: Boolean = false): List<String> = buildList {
        addAll(listOf("--window-fit", this@MachineSettings.size.arg))
        if (smoothing) add("--smooth-lcd")
        if (tg50Controls && !showKeyboard) add("--hide-keyboard")
        if (tg50Controls && !showJogControls) add("--hide-jog-controls")
        if (!soundSupported || !sound) add("--no-sound")
    }

    companion object {
        fun from(json: JSONObject?, fallback: MachineSettings = MachineSettings()): MachineSettings {
            if (json == null) return fallback
            return MachineSettings(
                ScreenSize.entries.firstOrNull { it.name == json.optString("size") } ?: fallback.size,
                json.optBoolean("smoothing", fallback.smoothing),
                json.optBoolean("sound", fallback.sound),
                json.optBoolean("fastAudio", fallback.fastAudio),
                json.optBoolean("microphone", fallback.microphone),
                json.optBoolean("showKeyboard", true),
                json.optBoolean("showJogControls", true),
                json.optBoolean("sonyAudioPreview", false),
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
