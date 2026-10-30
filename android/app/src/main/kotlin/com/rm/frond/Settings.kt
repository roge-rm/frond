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
    val fastAudio: Boolean = false,
) {
    // HomeActivity passes these to FrondActivity, which runs in its own process.
    fun putInto(intent: Intent): Intent = intent
        .putExtra(EXTRA_SIZE, size.name)
        .putExtra(EXTRA_SMOOTHING, smoothing)
        .putExtra(EXTRA_ORIENTATION, orientation.name)
        .putExtra(EXTRA_SOUND, sound)
        .putExtra(EXTRA_FAST_AUDIO, fastAudio)

    companion object {
        private const val FILE = "settings"
        private const val EXTRA_SIZE = "size"
        private const val EXTRA_SMOOTHING = "smoothing"
        private const val EXTRA_ORIENTATION = "orientation"
        private const val EXTRA_SOUND = "sound"
        private const val EXTRA_FAST_AUDIO = "fastAudio"

        fun load(context: Context): Settings {
            val p = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
            return Settings(
                size = enumOr(p.getString(EXTRA_SIZE, null), ScreenSize.FIT),
                smoothing = p.getBoolean(EXTRA_SMOOTHING, false),
                orientation = enumOr(p.getString(EXTRA_ORIENTATION, null), Orientation.PHONE),
                sound = p.getBoolean(EXTRA_SOUND, true),
                fastAudio = p.getBoolean(EXTRA_FAST_AUDIO, false),
            )
        }

        fun save(context: Context, settings: Settings) {
            context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
                .putString(EXTRA_SIZE, settings.size.name)
                .putBoolean(EXTRA_SMOOTHING, settings.smoothing)
                .putString(EXTRA_ORIENTATION, settings.orientation.name)
                .putBoolean(EXTRA_SOUND, settings.sound)
                .putBoolean(EXTRA_FAST_AUDIO, settings.fastAudio)
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
                fastAudio = extras.getBoolean(EXTRA_FAST_AUDIO, saved.fastAudio),
            )
        }

        private inline fun <reified T : Enum<T>> enumOr(name: String?, fallback: T): T =
            enumValues<T>().firstOrNull { it.name == name } ?: fallback
    }
}
