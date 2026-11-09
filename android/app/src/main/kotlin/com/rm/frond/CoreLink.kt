package com.rm.frond

import android.content.Context
import android.system.Os
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import java.io.File

// The running machine's infrared or serial port over a connection, in the emulator's process. The core
// listens on a Unix socket in a private folder (irda-mode or serial-mode unix:PATH); the link engine joins it
// to the chosen connection as the desktop hub does. Set at every start from the machine's settings and when
// changed while it runs. key is the hub's name for the kind (ir-attach, serial-attach, its events).
open class CoreLink(
    private val key: String, private val modeCommand: String, private val title: String, private val socketName: String,
) {
    data class Status(
        // The machine's setting: off or link:<id>.
        val choice: String = InfraredChoice.OFF,
        // The core's port is on a connection.
        val active: Boolean = false,
        val ir: LinkWire.IrStatus? = null,
        val error: String? = null,
        val title: String = "Infrared",
    ) {
        val text: String get() = when {
            error != null -> "Off. $error"
            choice == InfraredChoice.MODEM && error == null -> "Modem"
            choice == InfraredChoice.PHONE && error == null -> "Mobile phone"
            !active && InfraredChoice.linkId(choice) != null -> "$title: Unknown connection"
            !active -> "Off"
            ir != null -> LinkWire.channelText(title, ir)
            else -> "$title: connecting"
        }
    }

    private val state = MutableStateFlow(Status(title = title))
    val status: StateFlow<Status> get() = state

    private val lock = Any()
    private var connection: LinkConnection? = null
    private var paused = false
    private var attach: JSONObject? = null

    fun socketPath(context: Context): String? = linkRunSocket(context, socketName)

    // Sets the core's port and the link for choice. Blocks on the control link, so never on the main
    // thread. Returns an error to show, or null.
    fun apply(context: Context, control: ControlLink, choice: String, machine: String, device: String): String? {
        synchronized(lock) {
            // The modem and the phone are the core's own: no link, no socket.
            val own = when {
                choice == InfraredChoice.MODEM && modeCommand == "serial-mode" -> "modem"
                choice == InfraredChoice.PHONE && modeCommand == "irda-mode" -> "phone"
                else -> null
            }
            if (own != null) {
                detach()
                val reply = control.request(modeCommand, own, timeoutSeconds = 30)
                state.value = if (reply.ok) Status(choice, title = title) else Status(choice, error = reply.message, title = title)
                return if (reply.ok) null else reply.message
            }
            val id = InfraredChoice.active(choice, LinkStore.choices(InfraredChoice.folder(context)))
            if (id == null) {
                detach()
                val wasOn = state.value.active || state.value.choice == InfraredChoice.MODEM || state.value.choice == InfraredChoice.PHONE
                state.value = Status(choice, title = title)
                if (wasOn) {
                    val reply = control.request(modeCommand, "off", timeoutSeconds = 30)
                    if (!reply.ok) return reply.message
                }
                return null
            }
            val path = socketPath(context)
                ?: return "Couldn't make a folder for the ${title.lowercase()} socket.".also { state.value = Status(choice, error = it, title = title) }
            val reply = control.request(modeCommand, "unix:$path", timeoutSeconds = 30)
            if (!reply.ok) {
                detach()
                state.value = Status(choice, error = reply.message, title = title)
                return reply.message
            }
            state.value = Status(choice, active = true, title = title)
            val op = JSONObject().put("op", "$key-attach").put("machine", machine).put("device", device)
                .put("link", id).put("socket", path).put("paused", paused)
            attach = op
            val c = connection ?: Links.open(context, preferHost = true) { event -> onEvent(event) }.also { connection = it }
            c.setSticky(key, op)
            Log.i(TAG, "$key: link $id, core on $path")
            return null
        }
    }

    private fun onEvent(event: JSONObject) {
        if (event.optString("type") != key) return
        synchronized(lock) {
            if (connection == null) return
            val ir = LinkWire.IrStatus(
                state = event.optString("state"), peer = event.optString("peer"), machine = event.optString("machine"),
                device = event.optString("device"), busyBy = event.optString("busyBy"), far = event.optString("far", "running"),
                core = event.optBoolean("core"),
            )
            state.value = state.value.copy(ir = ir)
        }
    }

    // The app went to the background: the core pauses and the far side sees the Palm paused.
    fun setPaused(on: Boolean) {
        synchronized(lock) {
            if (paused == on) return
            paused = on
            attach = attach?.put("paused", on)
            connection?.send(JSONObject().put("op", "$key-state").put("state", if (on) "paused" else "running"))
        }
    }

    private fun detach() {
        attach = null
        connection?.let {
            it.setSticky(key, null)
            it.close()
        }
        connection = null
    }

    // The emulator is ending: the link closes, the core goes with the process.
    fun stop() {
        synchronized(lock) {
            detach()
            state.value = Status(title = title)
        }
    }

    private companion object {
        const val TAG = "Frond"
    }
}

// <no_backup>/link-run/<name>-<pid>.sock, the folder at mode 0700 as the core's unix: endpoint and the
// engine's Bluetooth socket want it.
internal fun linkRunSocket(context: Context, name: String): String? {
    val folder = File(context.noBackupFilesDir, "link-run")
    if (!folder.isDirectory && !folder.mkdirs()) return null
    try { Os.chmod(folder.path, "700".toInt(8)) } catch (e: Exception) { return null }
    return File(folder, "$name-${android.os.Process.myPid()}.sock").path
}

object IrLink : CoreLink("ir", "irda-mode", "Infrared", "ir")

// Serial is lossless: the engine opens the core's socket only once the channel joins, so the Palm's DSR and DCD
// mean the far port is open, and the far Palm's RTS low stops the link and drops this one's CTS.
object SerialLink : CoreLink("serial", "serial-mode", "Serial", "serial")

// The running machine's Bluetooth: This Frond's radio or a connection, in the emulator's process. The link
// engine listens on a socket in link-run and each connection the core makes there is a controller (bt-attach,
// as the desktop hub); the core is pointed at it with bluetooth-mode once the engine listens. Set at every
// start from the machine's settings and when changed while it runs.
object BluetoothLink {
    data class Status(
        // The machine's setting: off, radio or link:<id>.
        val choice: String = BluetoothChoice.OFF,
        // The core's Bluetooth is on the engine's socket.
        val active: Boolean = false,
        val bt: LinkWire.BtStatus? = null,
        val error: String? = null,
    ) {
        val text: String get() = when {
            error != null -> "Off. $error"
            !active && InfraredChoice.linkId(choice) != null -> "Bluetooth: Unknown connection"
            !active -> "Off"
            bt != null -> LinkWire.bluetoothText(bt)
            choice == BluetoothChoice.RADIO -> "Bluetooth: This Frond's radio"
            else -> "Bluetooth: connecting"
        }
    }

    private const val TAG = "Frond"
    private const val LISTEN_MS = 5000L

    private val state = MutableStateFlow(Status())
    val status: StateFlow<Status> get() = state

    private val lock = Any()
    @Volatile private var connection: LinkConnection? = null
    private var attach: JSONObject? = null
    // The path the core's bluetooth-mode points at, or null while it's off.
    private var corePath: String? = null
    @Volatile private var listening: java.util.concurrent.CountDownLatch? = null

    // Sets the core's Bluetooth and the engine's attach for choice. Blocks on the control link and on the
    // engine listening, so never on the main thread. Returns an error to show, or null.
    fun apply(context: Context, control: ControlLink, choice: String, machine: String, device: String): String? {
        synchronized(lock) {
            val link = BluetoothChoice.active(choice, LinkStore.choices(InfraredChoice.folder(context)))
            if (link == null) {
                detach()
                state.value = Status(choice)
                return coreOff(control)
            }
            val path = linkRunSocket(context, "bt")
                ?: return "Couldn't make a folder for the Bluetooth socket.".also { state.value = Status(choice, error = it) }
            val op = JSONObject().put("op", "bt-attach").put("machine", machine).put("device", device)
                .put("link", link).put("socket", path)
            if (op.toString() == attach?.toString() && corePath == path) {
                state.value = state.value.copy(choice = choice, error = null)
                return null
            }
            val latch = java.util.concurrent.CountDownLatch(1)
            listening = latch
            attach = op
            state.value = Status(choice, active = corePath == path)
            val c = connection ?: Links.open(context, preferHost = true) { event -> onEvent(event) }.also { connection = it }
            c.setSticky("bt", op)
            // The core connects as soon as it's pointed there, so the engine listens first.
            if (!latch.await(LISTEN_MS, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                detach()
                val error = "Couldn't make the Bluetooth socket."
                state.value = Status(choice, error = error)
                coreOff(control)
                return error
            }
            if (corePath != path) {
                val reply = control.request("bluetooth-mode", path, timeoutSeconds = 30)
                if (!reply.ok) {
                    detach()
                    state.value = Status(choice, error = reply.message)
                    return reply.message
                }
                corePath = path
            }
            state.value = state.value.copy(choice = choice, active = true, error = null)
            Log.i(TAG, "bluetooth: ${if (link == BluetoothChoice.RADIO) "this Frond's radio" else "link $link"}, core on $path")
            return null
        }
    }

    private fun coreOff(control: ControlLink): String? {
        if (corePath == null) return null
        corePath = null
        val reply = control.request("bluetooth-mode", "off", timeoutSeconds = 30)
        return if (reply.ok) null else reply.message
    }

    private fun onEvent(event: JSONObject) {
        if (event.optString("type") != "bt" || connection == null) return
        val bt = LinkWire.BtStatus(
            state = event.optString("state"), peer = event.optString("peer"), listening = event.optBoolean("listening"),
            controllers = event.optInt("controllers"),
        )
        if (bt.listening) listening?.countDown()
        state.value = state.value.copy(bt = bt)
    }

    private fun detach() {
        attach = null
        listening = null
        connection?.let {
            it.setSticky("bt", null)
            it.close()
        }
        connection = null
    }

    // The emulator is ending: the attach and this Frond's radio go, the core goes with the process.
    fun stop() {
        synchronized(lock) {
            detach()
            corePath = null
            state.value = Status()
        }
        BtLink.stop()
    }
}
