package com.rm.frond

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.concurrent.Executors

// How the machine's battery follows: Maximum, a set level, or this phone. arg is the saved name.
enum class BatteryMode(val label: String, val arg: String) {
    MAX("Maximum", "max"),
    LEVEL("Set level", "level"),
    HOST("Host battery", "host"),
}

// What the phone reports: its level, and whether it is on external power.
data class HostBattery(val percent: Int = 100, val charging: Boolean = true)

// What the core said about the machine's battery: unknown until it answers battery-state.
data class BatteryInfo(val known: Boolean = false, val supported: Boolean = false, val chargingSupported: Boolean = true) {
    companion object {
        // "unavailable" means no battery setting at all; a trailing " no-charging" means it can't charge.
        fun parse(reply: String) = BatteryInfo(true, reply != "unavailable", !reply.endsWith(" no-charging"))
    }
}

// The Battery setting as the core's text and the phone's battery from Android's sticky intent.
object BatteryText {
    // "max [charging|battery]", "level N [...]" or "host N [...]". Host mode takes the phone's level and charging
    // state. Where the machine can't charge it always says battery.
    fun spec(mode: BatteryMode, level: Int, charging: Boolean, host: HostBattery, chargingSupported: Boolean = true): String {
        fun word(on: Boolean) = if (on && chargingSupported) "charging" else "battery"
        return when (mode) {
            BatteryMode.HOST -> "host ${host.percent.coerceIn(0, 100)} ${word(host.charging)}"
            BatteryMode.LEVEL -> "level ${level.coerceIn(0, 100)} ${word(charging)}"
            BatteryMode.MAX -> "max ${word(charging)}"
        }
    }

    // The ACTION_BATTERY_CHANGED extras as a host state. A missing level or scale reads as full; plugged (any
    // source) or a status of charging or full counts as external power. A phone with no battery reads full on power.
    fun host(level: Int, scale: Int, plugged: Int, status: Int, present: Boolean = true): HostBattery {
        if (!present) return HostBattery(100, true)
        val percent = if (level < 0 || scale <= 0) 100 else (level * 100.0 / scale).let { Math.round(it).toInt() }.coerceIn(0, 100)
        val charging = plugged > 0 || status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        return HostBattery(percent, charging)
    }

    fun host(intent: Intent?): HostBattery = intent?.let {
        host(
            it.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), it.getIntExtra(BatteryManager.EXTRA_SCALE, -1),
            it.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0), it.getIntExtra(BatteryManager.EXTRA_STATUS, 0),
            it.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true),
        )
    } ?: HostBattery()

    fun readHost(context: Context): HostBattery =
        host(context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)))
}

// Sends the running machine's Battery setting to the core: once at start after asking battery-state, then on each
// change of the setting and, in host mode, each time the phone's battery text changes. The setting is read through
// settings(); nothing here runs on the UI thread's time.
class BatteryControl(
    private val context: Context,
    private val link: ControlLink,
    private val settings: () -> MachineSettings?,
) {
    var info by mutableStateOf(BatteryInfo())
        private set
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "FrondBattery").apply { isDaemon = true } }
    private var host = HostBattery()
    private var sent: String? = null
    private var registered = false
    @Volatile private var stopped = false
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val next = BatteryText.host(intent)
            worker.execute { host = next; send() }
        }
    }

    // The text the core is told at launch, so the first start already follows the setting.
    fun launchSpec(): String {
        val saved = settings() ?: MachineSettings()
        host = if (saved.batteryMode == BatteryMode.HOST) BatteryText.readHost(context) else host
        sent = BatteryText.spec(saved.batteryMode, saved.batteryLevel, saved.batteryCharging, host)
        return sent!!
    }

    // Once the core answers: asks battery-state, then applies the setting again.
    fun start() {
        link.whenConnected {
            if (!link.connectedOnce) return@whenConnected
            worker.execute {
                if (stopped) return@execute
                val reply = link.request("battery-state", timeoutSeconds = 30)
                if (!reply.ok) { Log.w(TAG, "battery-state: ${reply.code} ${reply.message}"); return@execute }
                info = BatteryInfo.parse(reply.message)
                sent = null
                refresh()
            }
        }
    }

    // The saved setting changed: watch the phone in host mode, and send the text.
    fun changed() {
        worker.execute { refresh() }
    }

    private fun refresh() {
        if (stopped) return
        val saved = settings() ?: return
        val hostMode = saved.batteryMode == BatteryMode.HOST && info.supported
        if (hostMode) {
            host = BatteryText.readHost(context)
            if (!registered) {
                context.registerReceiver(receiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                registered = true
            }
        } else unregister()
        send()
    }

    private fun send() {
        if (stopped || !info.known || !info.supported) return
        val saved = settings() ?: return
        val spec = BatteryText.spec(saved.batteryMode, saved.batteryLevel, saved.batteryCharging, host, info.chargingSupported)
        if (spec == sent) return
        var reply = link.request("battery", spec, timeoutSeconds = 10)
        if (!reply.ok && !stopped) {
            // The core can be busy for a moment; one more try.
            Thread.sleep(500)
            reply = link.request("battery", spec, timeoutSeconds = 10)
        }
        if (reply.ok) sent = spec else Log.w(TAG, "battery: ${reply.code} ${reply.message}")
    }

    private fun unregister() {
        if (!registered) return
        registered = false
        try { context.unregisterReceiver(receiver) } catch (_: IllegalArgumentException) {}
    }

    // The machine stopped: no more reading or sending.
    fun stop() {
        stopped = true
        worker.execute { unregister() }
        worker.shutdown()
    }

    companion object {
        private const val TAG = "FrondBattery"
    }
}
