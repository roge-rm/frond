package com.rm.frond

import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo

// How the device fills the phone screen. arg is the core's --window-fit value.
enum class ScreenSize(val label: String, val arg: String) {
    FIT("Fit", "fit"),
    WHOLE("Whole steps", "whole"),
    FILL("Fill", "fill"),
}

enum class Orientation(val label: String, val request: Int) {
    PHONE("Follow the phone", ActivityInfo.SCREEN_ORIENTATION_FULL_USER),
    PORTRAIT("Portrait", ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT),
    LANDSCAPE("Landscape", ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE),
}

data class Settings(
    val size: ScreenSize = ScreenSize.FIT,
    val smoothing: Boolean = false,
    val orientation: Orientation = Orientation.PHONE,
    val sound: Boolean = true,
    // The core's gdb server, relayed by DebugLink on debuggerPort. The Palm waits for gdb before it runs.
    val debugger: Boolean = false,
    val debuggerPort: Int = DebugLink.DEFAULT_PORT,
) {
    // HomeActivity passes these to FrondActivity, which runs in its own process.
    fun putInto(intent: Intent): Intent = intent
        .putExtra(EXTRA_SIZE, size.name)
        .putExtra(EXTRA_SMOOTHING, smoothing)
        .putExtra(EXTRA_ORIENTATION, orientation.name)
        .putExtra(EXTRA_SOUND, sound)
        .putExtra(EXTRA_DEBUGGER, debugger)
        .putExtra(EXTRA_DEBUGGER_PORT, debuggerPort)

    companion object {
        private const val FILE = "settings"
        private const val EXTRA_SIZE = "size"
        private const val EXTRA_SMOOTHING = "smoothing"
        private const val EXTRA_ORIENTATION = "orientation"
        private const val EXTRA_SOUND = "sound"
        private const val EXTRA_FAST_AUDIO = "fastAudio"
        private const val FAST_AUDIO_PREFIX = "fastAudio."
        private const val FAST_AUDIO_MIGRATED = "fastAudioPerDevice"
        private const val EXTRA_DEBUGGER = "debugger"
        private const val EXTRA_DEBUGGER_PORT = "debuggerPort"

        // Reads the file again when the other process wrote it: Home and the emulator run in their own
        // processes and both change Sound.
        @Suppress("DEPRECATION")
        fun load(context: Context): Settings {
            val p = context.getSharedPreferences(FILE, Context.MODE_MULTI_PROCESS)
            return Settings(
                size = enumOr(p.getString(EXTRA_SIZE, null), ScreenSize.FIT),
                smoothing = p.getBoolean(EXTRA_SMOOTHING, false),
                orientation = enumOr(p.getString(EXTRA_ORIENTATION, null), Orientation.PHONE),
                sound = p.getBoolean(EXTRA_SOUND, true),
                debugger = p.getBoolean(EXTRA_DEBUGGER, false),
                debuggerPort = NetRelay.port(p.getInt(EXTRA_DEBUGGER_PORT, DebugLink.DEFAULT_PORT).toString()) ?: DebugLink.DEFAULT_PORT,
            )
        }

        // Low-latency audio per device. Read at each start: the core takes it as a start-up argument.
        @Suppress("DEPRECATION")
        fun fastAudio(context: Context, device: Device): Boolean {
            val p = context.getSharedPreferences(FILE, Context.MODE_MULTI_PROCESS)
            return device.fastAudio && p.getBoolean(FAST_AUDIO_PREFIX + device.id, false)
        }

        fun legacyMachine(context: Context, device: Device?): MachineSettings {
            migrateFastAudio(context)
            val saved = load(context)
            return MachineSettings(saved.size, saved.smoothing, saved.sound,
                device?.let { fastAudio(context, it) } ?: false)
        }

        // Once: a global switch left on turns on every device that can use it, and the old key goes.
        fun migrateFastAudio(context: Context) {
            val p = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            if (p.getBoolean(FAST_AUDIO_MIGRATED, false)) return
            val edit = p.edit().putBoolean(FAST_AUDIO_MIGRATED, true).remove(EXTRA_FAST_AUDIO)
            if (p.getBoolean(EXTRA_FAST_AUDIO, false)) Device.entries.filter { it.fastAudio }.forEach { edit.putBoolean(FAST_AUDIO_PREFIX + it.id, true) }
            edit.apply()
        }

        fun save(context: Context, settings: Settings) {
            context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
                .putString(EXTRA_ORIENTATION, settings.orientation.name)
                .putBoolean(EXTRA_DEBUGGER, settings.debugger)
                .putInt(EXTRA_DEBUGGER_PORT, settings.debuggerPort)
                .apply()
        }

        // Settings from FrondActivity's intent, else the saved ones.
        fun from(context: Context, intent: Intent?): Settings {
            val saved = load(context)
            val extras = intent?.extras ?: return saved
            return Settings(
                size = enumOr(extras.getString(EXTRA_SIZE), saved.size),
                smoothing = extras.getBoolean(EXTRA_SMOOTHING, saved.smoothing),
                orientation = enumOr(extras.getString(EXTRA_ORIENTATION), saved.orientation),
                sound = extras.getBoolean(EXTRA_SOUND, saved.sound),
                debugger = extras.getBoolean(EXTRA_DEBUGGER, saved.debugger),
                debuggerPort = extras.getInt(EXTRA_DEBUGGER_PORT, saved.debuggerPort),
            )
        }

        private inline fun <reified T : Enum<T>> enumOr(name: String?, fallback: T): T =
            enumValues<T>().firstOrNull { it.name == name } ?: fallback
    }
}
