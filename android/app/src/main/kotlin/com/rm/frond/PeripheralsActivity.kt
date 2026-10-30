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
        c.discard()
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
        )
        // Without the emulator (the process was restarted here) there's nothing to show.
        if (emulator == null) {
            finish()
            return
        }
        model.refresh()
        setContent {
            FrondTheme {
                Surface(Modifier.fillMaxSize()) {
                    BackHandler(enabled = model.path != null) {
                        if (model.path == "/") model.leaveFiles() else model.up()
                    }
                    PeripheralsScreen(model, onClose = { finish() })
                }
            }
        }
    }

    override fun onStop() {
        super.onStop()
        if (!isFinishing && !isChangingConfigurations) FrondActivity.running?.saveInBackground()
    }
}
