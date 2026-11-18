package com.rm.frond

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice

// Where the guest's vibrate motor (the Zodiac's rumble) goes: the controller playing when it has a
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
class Rumble(private val context: Context) {
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
        val pad = controller?.let { controllerVibrator(it) }
        val phone = phoneVibrator()
        val vibrator = when (RumbleChoice.target(enabled, pad != null, phone != null)) {
            RumbleTarget.CONTROLLER -> pad
            RumbleTarget.PHONE -> phone
            RumbleTarget.NONE -> null
        } ?: return
        // Repeats the 1 s step until cancelled.
        vibrator.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 1000), 0))
        running = vibrator
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
