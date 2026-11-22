package com.rm.frond

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice

// Where the guest's vibrate motor goes: the controller playing when it has a
// vibrator, else the phone's, or nowhere when the machine's Rumble setting is off.
enum class RumbleTarget { CONTROLLER, PHONE, NONE }

object RumbleChoice {
    fun target(enabled: Boolean, controllerVibrates: Boolean, phoneVibrates: Boolean): RumbleTarget = when {
        !enabled -> RumbleTarget.NONE
        controllerVibrates -> RumbleTarget.CONTROLLER
        phoneVibrates -> RumbleTarget.PHONE
        else -> RumbleTarget.NONE
    }
}

// Runs a vibrator while the guest's motor is on. The guest steps any pattern itself, so this only starts
// and stops. UI thread only.
class Rumble(private val context: Context, private val notification: Boolean = false) {
    var enabled = true
        set(value) {
            field = value
            if (!value) stop()
        }
    // The controller that last sent a key or a stick move, or null for none.
    var controller: Int? = null
    private var on = false
    private var running: Vibrator? = null

    fun set(motor: Boolean) {
        if (motor == on) return
        on = motor
        if (!motor) {
            stop()
            return
        }
        if (notification &&
            context.getSystemService(AudioManager::class.java)?.ringerMode == AudioManager.RINGER_MODE_SILENT
        ) return
        val pad = if (notification) null else controller?.let { controllerVibrator(it) }
        val phone = phoneVibrator()
        val vibrator = when (RumbleChoice.target(enabled, pad != null, phone != null)) {
            RumbleTarget.CONTROLLER -> pad
            RumbleTarget.PHONE -> phone
            RumbleTarget.NONE -> null
        } ?: return
        // Repeats the 1 s step until cancelled.
        val effect = VibrationEffect.createWaveform(longArrayOf(0, 1000), 0)
        if (notification) vibrateNotification(vibrator, effect)
        else vibrator.vibrate(effect)
        running = vibrator
    }

    @Suppress("DEPRECATION")
    private fun vibrateNotification(vibrator: Vibrator, effect: VibrationEffect) {
        val attributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION).build()
        vibrator.vibrate(effect, attributes)
    }

    // Stops it, as when the app goes to the background; the next on starts it again.
    fun stop() {
        running?.cancel()
        running = null
        on = false
    }

    fun forget(device: Int) {
        if (controller == device) controller = null
    }

    private fun controllerVibrator(id: Int): Vibrator? {
        val device = InputDevice.getDevice(id) ?: return null
        val vibrator = if (Build.VERSION.SDK_INT >= 31) device.vibratorManager.defaultVibrator
            else @Suppress("DEPRECATION") device.vibrator
        return vibrator.takeIf { it.hasVibrator() }
    }

    private fun phoneVibrator(): Vibrator? {
        val vibrator = if (Build.VERSION.SDK_INT >= 31) context.getSystemService(VibratorManager::class.java)?.defaultVibrator
            else context.getSystemService(Vibrator::class.java)
        return vibrator?.takeIf { it.hasVibrator() }
    }
}
