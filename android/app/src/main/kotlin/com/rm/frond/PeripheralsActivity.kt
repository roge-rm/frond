package com.rm.frond

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.lifecycleScope
import com.rm.frond.ui.FrondTheme
import com.rm.frond.ui.LogScreen
import com.rm.frond.ui.PeripheralsScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// What the emulator's menu screen shows. Card and database work runs one job at a time off the main thread.
class PeripheralsModel(
    private val activity: PeripheralsActivity,
    val device: Device,
    private val card: SdCard?,
    private val databases: PalmDatabases?,
    private val link: ControlLink?,
    // The running session's name, for the copy's file name.
    val name: String,
) {
    var state by mutableStateOf(SdCard.State.UNKNOWN)
        private set
    var busy by mutableStateOf<String?>(null)
        private set
    var alert by mutableStateOf<Pair<String, String>?>(null)
    var note by mutableStateOf<String?>(null)
    // The card folder open in the file view, or null when the view is closed.
    var path by mutableStateOf<String?>(null)
        private set
    var entries by mutableStateOf(emptyList<FatImage.Entry>())
        private set
    var free by mutableStateOf(0L)
        private set
    var changed by mutableStateOf(false)
        private set
    // Asking whether to insert the changed card on leaving the file view.
    var askingLeave by mutableStateOf(false)
    // Asking for a database name: "launch" or "export", with the RAM databases found for the list.
    var naming by mutableStateOf<String?>(null)
    var found by mutableStateOf(emptyList<PalmDatabases.Info>())

    val available get() = card != null

    fun job(label: String, failTitle: String, work: (SdCard) -> Unit) {
        val c = card ?: return
        if (busy != null) return
        busy = label
        activity.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    work(c)
                    null
                } catch (e: Exception) {
                    SdCard.message(e)
                }
            }
            val s = withContext(Dispatchers.IO) { c.state() }
            state = s
            changed = c.changed
            busy = null
            if (error != null) alert = failTitle to error
        }
    }

    // Database work through the control link; leaves the card state alone.
    fun palm(label: String, failTitle: String, work: (PalmDatabases) -> Unit) {
        val d = databases ?: return
        if (busy != null) return
        busy = label
        activity.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    work(d)
                    null
                } catch (e: Exception) {
                    if (e is PalmDatabases.Failed) e.message else "Couldn't read or write the file."
                }
            }
            busy = null
            if (error != null) alert = failTitle to error
        }
    }

    fun install(uri: android.net.Uri) = palm("Installing", "Couldn't install") { d ->
        val result = d.install(uri)
        activity.runOnUiThread { note = result }
    }

    // Asks for a name at once and fills the list of RAM databases in when it's read (an export of
    // all of them, which can take a while). A failed list still lets you type a name.
    var listing by mutableStateOf(false)
        private set
    private var asks = 0

    fun askName(what: String) {
        val d = databases ?: return
        if (busy != null || naming != null) return
        found = emptyList()
        naming = what
        listing = true
        val asked = ++asks
        activity.lifecycleScope.launch {
            val list = withContext(Dispatchers.IO) {
                try { d.list() } catch (e: Exception) { emptyList() }
            }
            // A later question has its own list coming.
            if (asked != asks) return@launch
            found = if (what == "launch") list.filter { it.application } else list
            listing = false
        }
    }

    // Launching returns to the Palm, where the application opens.
    fun launch(name: String) = palm("Launching", "Couldn't launch") { d ->
        d.launch(name)
        activity.runOnUiThread { activity.finish() }
    }

    fun export(to: android.net.Uri, name: String?) = palm("Exporting", "Couldn't export") { d ->
        d.export(to, name)
        activity.runOnUiThread { note = if (name == null) "RAM databases exported" else "$name exported" }
    }

    // Device menu work through the control link; returns to the Palm when done if back is set.
    private fun device(label: String, failTitle: String, back: Boolean, work: (ControlLink) -> String?) {
        val l = link ?: return
        if (busy != null) return
        busy = label
        activity.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) { work(l) }
            busy = null
            if (error != null) alert = failTitle to error
            else if (back) activity.finish()
        }
    }

    private fun send(l: ControlLink, command: String): String? {
        val reply = l.request(command, timeoutSeconds = 30)
        return if (reply.ok) null else reply.message
    }

    // The Palm infrared keyboard: attached or not, null until the core says.
    var keyboard by mutableStateOf<Boolean?>(null)
        private set

    fun readKeyboard() {
        val l = link ?: return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("keyboard-state", timeoutSeconds = 30) }
            if (reply.ok) keyboard = reply.message == "attached"
        }
    }

    fun attachKeyboard(on: Boolean) =
        device(if (on) "Attaching" else "Detaching", if (on) "Couldn't attach the keyboard" else "Couldn't detach the keyboard", false) { l ->
            val reply = l.request("keyboard-attach", if (on) "on" else "off", timeoutSeconds = 30)
            if (reply.ok) activity.runOnUiThread { keyboard = reply.message == "attached" }
            if (reply.ok) null else reply.message
        }

    // The core's network mode, null until it says.
    var network by mutableStateOf<NetworkMode?>(null)
        private set

    fun readNetwork() {
        val l = link ?: return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("network-state", timeoutSeconds = 30) }
            if (reply.ok) network = NetworkMode.byId(reply.message)
        }
    }

    // Live, like the desktop's Network menu; changing it closes the Palm's connections.
    fun changeNetwork(mode: NetworkMode) = device("Changing the network", "Couldn't change the network", false) { l ->
        val reply = l.request("network-mode", mode.id, timeoutSeconds = 30)
        if (reply.ok) activity.runOnUiThread { network = NetworkMode.byId(reply.message) }
        if (reply.ok) null else reply.message
    }

    // Both go back to the Palm, where the reset or rotation shows.
    fun reset() = device("Resetting", "Couldn't reset", true) { send(it, "reset") }

    fun rotate() = device("Rotating", "Couldn't rotate", true) { send(it, "rotate") }

    fun saveCopy(to: android.net.Uri) = device("Saving", "Couldn't save a copy", false) { l ->
        Sessions.saveCopy(activity.applicationContext, l, to).also { if (it == null) activity.runOnUiThread { note = "Copy saved" } }
    }

    // The core's log while the log view is open, or null when it's closed.
    var log by mutableStateOf<String?>(null)

    fun showLog() {
        activity.lifecycleScope.launch {
            log = withContext(Dispatchers.IO) { CoreLog.read(activity.applicationContext) }
        }
    }

    fun refresh() = job("Reading", "Couldn't read the card") { }

    // Lists the open folder again after a change.
    private fun relist(c: SdCard, at: String) {
        val (list, space) = c.read { it.list(at) to it.freeBytes }
        activity.runOnUiThread {
            path = at
            entries = list
            free = space
        }
    }

    fun openFiles() = job("Ejecting", "Couldn't open the card") { c ->
        c.beginEdit()
        relist(c, "/")
    }

    fun open(at: String) = job("Reading", "Couldn't open the folder") { c -> relist(c, at) }

    fun up() {
        val p = path ?: return
        if (p == "/") return
        open(p.substringBeforeLast('/').ifEmpty { "/" })
    }

    fun addFile(uri: android.net.Uri) {
        val at = path ?: return
        job("Copying", "Couldn't add the file") { c ->
            val name = c.addFile(at, uri)
            relist(c, at)
            activity.runOnUiThread { note = "$name added" }
        }
    }

    fun makeFolder(name: String) {
        val at = path ?: return
        job("Copying", "Couldn't make the folder") { c ->
            c.makeFolder(at, name)
            relist(c, at)
        }
    }

    fun exportFile(entry: FatImage.Entry, to: android.net.Uri) =
        job("Copying", "Couldn't export the file") { c ->
            c.exportFile(entry, to)
            activity.runOnUiThread { note = "${entry.name} exported" }
        }

    // Leaving the file view: with changes, asks whether to insert them first.
    fun leaveFiles() {
        if (changed) askingLeave = true else insertEdited()
    }

    // Closes the file view and puts the card back in the Palm, edited or not.
    fun insertEdited() = job("Inserting", "Couldn't insert the card") { c ->
        c.finishEdit()
        activity.runOnUiThread { path = null }
    }

    // Closes the file view and leaves the card ejected, without the changes.
    fun discard() = job("Reading", "Couldn't read the card") { c ->
        c.cancelEdit()
        activity.runOnUiThread { path = null }
    }
}

// The emulator's menu: applications and databases, the SD card and the other peripherals of the running Palm. Runs in the emulator's process and uses
// FrondActivity's control link. While it's open the emulator is in the background and the guest is
// paused; Home from here saves the session like it does from the emulator.
class PeripheralsActivity : ComponentActivity() {
    private lateinit var model: PeripheralsModel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val device = Device.byId(intent?.getStringExtra(FrondActivity.EXTRA_DEVICE)) ?: Device.TX
        val emulator = FrondActivity.running
        model = PeripheralsModel(
            this,
            device,
            emulator?.let { SdCard(applicationContext, it.control, it.session) },
            emulator?.let { PalmDatabases(applicationContext, it.control) },
            emulator?.control,
            emulator?.session?.let { Sessions.read(this, java.io.File(it).name.removeSuffix(".frond"))?.name } ?: device.label,
        )
        // Without the emulator (the process was restarted here) there's nothing to show.
        if (emulator == null) {
            finish()
            return
        }
        model.refresh()
        model.readKeyboard()
        model.readNetwork()
        setContent {
            FrondTheme {
                Surface(Modifier.fillMaxSize()) {
                    BackHandler(enabled = model.path != null || model.log != null) {
                        if (model.log != null) model.log = null
                        else if (model.path == "/") model.leaveFiles() else model.up()
                    }
                    val log = model.log
                    if (log != null) LogScreen(log, onClose = { model.log = null })
                    else PeripheralsScreen(model, onClose = { finish() })
                }
            }
        }
    }

    override fun onStop() {
        super.onStop()
        if (!isFinishing && !isChangingConfigurations) FrondActivity.running?.saveInBackground()
    }
}
