package com.rm.frond

import android.content.Context
import android.system.Os
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import java.io.File

// The running machine's infrared over a connection, in the emulator's process. The core listens on a
// Unix socket in a private folder (irda-mode unix:PATH); the link engine joins it to the chosen connection
// as the desktop hub does. Set at every start from the machine's settings and when changed while it runs.
object IrLink {
    private const val TAG = "Frond"

    data class Status(
        // The machine's setting: off or link:<id>.
        val choice: String = InfraredChoice.OFF,
        // The core's infrared is on a connection.
        val active: Boolean = false,
        val ir: LinkWire.IrStatus? = null,
        val error: String? = null,
    ) {
        val text: String get() = when {
            error != null -> "Off. $error"
            !active && InfraredChoice.linkId(choice) != null -> "Infrared: Unknown connection"
            !active -> "Off"
            ir != null -> LinkWire.irText(ir)
            else -> "Infrared: connecting"
        }
    }

    private val state = MutableStateFlow(Status())
    val status: StateFlow<Status> get() = state

    private val lock = Any()
    private var connection: LinkConnection? = null
    private var paused = false
    private var attach: JSONObject? = null

    // <no_backup>/link-run, mode 0700 as the core's unix: endpoint wants it.
    fun socketPath(context: Context): String? {
        val folder = File(context.noBackupFilesDir, "link-run")
        if (!folder.isDirectory && !folder.mkdirs()) return null
        try { Os.chmod(folder.path, "700".toInt(8)) } catch (e: Exception) { return null }
        return File(folder, "ir-${android.os.Process.myPid()}.sock").path
    }

    // Sets the core's infrared and the link for choice. Blocks on the control link, so never on the main
    // thread. Returns an error to show, or null.
    fun apply(context: Context, control: ControlLink, choice: String, machine: String, device: String): String? {
        synchronized(lock) {
            val id = InfraredChoice.active(choice, LinkStore.choices(InfraredChoice.folder(context)))
            if (id == null) {
                detach()
                val wasOn = state.value.active
                state.value = Status(choice)
                if (wasOn) {
                    val reply = control.request("irda-mode", "off", timeoutSeconds = 30)
                    if (!reply.ok) return reply.message
                }
                return null
            }
            val path = socketPath(context) ?: return "Couldn't make a folder for the infrared socket.".also { state.value = Status(choice, error = it) }
            val reply = control.request("irda-mode", "unix:$path", timeoutSeconds = 30)
            if (!reply.ok) {
                detach()
                state.value = Status(choice, error = reply.message)
                return reply.message
            }
            state.value = Status(choice, active = true)
            val op = JSONObject().put("op", "ir-attach").put("machine", machine).put("device", device)
                .put("link", id).put("socket", path).put("paused", paused)
            attach = op
            val c = connection ?: Links.open(context, preferHost = true) { event -> onEvent(event) }.also { connection = it }
            c.setSticky("ir", op)
            Log.i(TAG, "infrared: link $id, core on $path")
            return null
        }
    }

    private fun onEvent(event: JSONObject) {
        if (event.optString("type") != "ir") return
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
            connection?.send(JSONObject().put("op", "ir-state").put("state", if (on) "paused" else "running"))
        }
    }

    private fun detach() {
        attach = null
        connection?.let {
            it.setSticky("ir", null)
            it.close()
        }
        connection = null
    }

    // The emulator is ending: the link closes, the core goes with the process.
    fun stop() {
        synchronized(lock) {
            detach()
            state.value = Status()
        }
    }
}
