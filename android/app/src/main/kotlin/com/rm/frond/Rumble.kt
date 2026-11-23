package com.rm.frond

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice

// Where the guest's vibrate motor goes. While any game controller is connected it is the controller's
// vibrator, or nowhere when none vibrates; the phone only buzzes with no controller connected.
// NONE also when the machine's Rumble setting is off.
enum class RumbleTarget { CONTROLLER, PHONE, NONE }

object RumbleChoice {
    fun target(enabled: Boolean, controllerConnected: Boolean, controllerVibrates: Boolean, phoneVibrates: Boolean): RumbleTarget = when {
        !enabled -> RumbleTarget.NONE
        controllerConnected -> if (controllerVibrates) RumbleTarget.CONTROLLER else RumbleTarget.NONE
        phoneVibrates -> RumbleTarget.PHONE
        else -> RumbleTarget.NONE
    }

    // The device to vibrate: the last used one if it is connected and vibrates, else the first that does.
    fun pick(connected: List<Int>, vibrates: (Int) -> Boolean, last: Int?): Int? =
        last?.takeIf { it in connected && vibrates(it) } ?: connected.firstOrNull(vibrates)
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
    private var runningPad: Int? = null

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
        val connected = connectedControllers()
        val padId = RumbleChoice.pick(connected, { controllerVibrator(it) != null }, controller)
        val pad = padId?.let { controllerVibrator(it) }
        val phone = phoneVibrator()
        val vibrator = when (RumbleChoice.target(enabled, connected.isNotEmpty(), pad != null, phone != null)) {
            RumbleTarget.CONTROLLER -> pad
            RumbleTarget.PHONE -> phone
            RumbleTarget.NONE -> null
        } ?: return
        // Repeats the 1 s step until cancelled.
        val effect = VibrationEffect.createWaveform(longArrayOf(0, 1000), 0)
        if (notification) vibrateNotification(vibrator, effect)
        else vibrator.vibrate(effect)
        running = vibrator
        runningPad = if (vibrator === pad) padId else null
    }

    // Real external game controllers. A handheld's built-in pad doesn't count, so its own motor still buzzes.
    private fun connectedControllers(): List<Int> = InputDevice.getDeviceIds().filter { id ->
        val d = InputDevice.getDevice(id) ?: return@filter false
        val external = Build.VERSION.SDK_INT < 29 || d.isExternal
        !d.isVirtual && external && (d.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            d.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)
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
        runningPad = null
        on = false
    }

    fun forget(device: Int) {
        if (controller == device) controller = null
        // The motor stays on in the guest, but the gone controller's vibrator is cancelled.
        if (runningPad == device) {
            running?.cancel()
            running = null
            runningPad = null
        }
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
