package com.rm.frond

import android.net.Uri
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

// What the model needs from the running bar's activity.
class ModelHost(
    context: android.content.Context,
    val lifecycleScope: kotlinx.coroutines.CoroutineScope,
    private val ui: (Runnable) -> Unit,
    private val onFinish: () -> Unit,
) {
    val applicationContext: android.content.Context = context.applicationContext
    val contentResolver: android.content.ContentResolver get() = applicationContext.contentResolver
    val cacheDir: File get() = applicationContext.cacheDir
    fun runOnUiThread(block: () -> Unit) = ui(Runnable(block))
    fun finish() = onFinish()
}

// What the running bar's menu and sheets show. Card and database work runs one job at a time off the main thread.
class RunningModel(
    private val activity: ModelHost,
    val device: Device,
    private val card: SdCard?,
    private val databases: PalmDatabases?,
    private val link: ControlLink?,
    // The running session's name, for the copy's file name.
    val name: String,
) : FilesModel {
    var state by mutableStateOf(SdCard.State.UNKNOWN)
        private set
    var writeProtected by mutableStateOf<Boolean?>(null)
        private set
    override val title get() = device.cardLabel
    override val canWrite get() = !device.memoryStick || writeProtected == false
    override val leaveLabel get() = "Insert"
    override val prompt = ClashPrompt()
    // Whether the stick in the slot is a Memory Stick Pro, which exports as a raw image.
    var memoryStickPro by mutableStateOf(false)
        private set
    var canCancelFiles by mutableStateOf(false)
        private set
    var cancellingFiles by mutableStateOf(false)
        private set
    override var busy by mutableStateOf<String?>(null)
        private set
    // How far a long SD job has got, 0 to 1, or null when it isn't known.
    var progress by mutableStateOf<Float?>(null)
        private set
    var alert by mutableStateOf<Pair<String, String>?>(null)
    var note by mutableStateOf<String?>(null)
    // The card folder open in the file view, or null when the view is closed.
    override var path by mutableStateOf<String?>(null)
        private set
    override var entries by mutableStateOf(emptyList<FatImage.Entry>())
        private set
    override var free by mutableStateOf(0L)
        private set
    override var total by mutableStateOf(0L)
        private set
    override var shortcuts by mutableStateOf(emptyList<String>())
        private set
    override var changed by mutableStateOf(false)
        private set
    // Asking whether to insert the changed card on leaving the file view.
    // Asking for a database name: "launch", "export" or "other" (the sheets' by-name dialog), with the RAM databases found for the list.
    var naming by mutableStateOf<String?>(null)
    var found by mutableStateOf(emptyList<PalmDatabases.Info>())

    val available get() = card != null

    // While the core waits for its first gdb nothing answers the control link, so work isn't started; the reads
    // run again once it connects.
    var waitingForGdb by mutableStateOf(FrondActivity.running?.waitingForGdb == true)
        private set

    fun watchGdb() {
        val l = link ?: return
        if (!waitingForGdb) return
        l.whenConnected {
            activity.runOnUiThread {
                if (!waitingForGdb) return@runOnUiThread
                waitingForGdb = false
                readCard()
                readKeyboard()
                readHold()
                readNetwork()
                readRadio()
            }
        }
    }

    // Says so instead of starting work the core can't answer yet.
    private fun waiting(): Boolean {
        if (waitingForGdb) note = "Waiting for gdb"
        return waitingForGdb
    }

    fun job(label: String, failTitle: String, cancellable: Boolean = false, work: (SdCard) -> Unit) {
        val c = card ?: return
        if (busy != null || waiting()) return
        c.beginJob()
        canCancelFiles = cancellable
        cancellingFiles = false
        busy = label
        progress = null
        link?.sdProgress = { done, total -> activity.runOnUiThread { if (busy != null) progress = done.toFloat() / total } }
        activity.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    work(c)
                    null
                } catch (e: Exception) {
                    if (e is SdCard.Failed && e.code == "cancelled") {
                        activity.runOnUiThread { note = "Cancelled" }; null
                    } else SdCard.message(e)
                }
            }
            val s = withContext(Dispatchers.IO) { c.state() }
            val protection = withContext(Dispatchers.IO) { c.protection() }
            val pro = withContext(Dispatchers.IO) { c.memoryStickPro() }
            val slot = withContext(Dispatchers.IO) { slotInfo(s) }
            slotShown = slot
            state = s
            writeProtected = protection
            memoryStickPro = pro
            changed = c.changed
            link?.sdProgress = null
            busy = null
            progress = null
            canCancelFiles = false
            cancellingFiles = false
            if (error != null) alert = failTitle to error
        }
    }

    // The card's state without a busy dialog, for the bar's icon.
    fun readCard() {
        val c = card ?: return
        if (busy != null || waitingForGdb || !device.cardSupported) return
        activity.lifecycleScope.launch {
            val s = withContext(Dispatchers.IO) { c.state() }
            val protection = withContext(Dispatchers.IO) { c.protection() }
            val pro = withContext(Dispatchers.IO) { c.memoryStickPro() }
            val slot = withContext(Dispatchers.IO) { slotInfo(s) }
            if (busy != null) return@launch
            slotShown = slot
            state = s
            writeProtected = protection
            memoryStickPro = pro
        }
    }

    fun cancelFiles() {
        if (!canCancelFiles || cancellingFiles) return
        cardCancel?.let {
            it.set(true)
            cancellingFiles = true
            return
        }
        val cancellation = card?.cancelJob()
        if (cancellation == null) { canCancelFiles = false; return }
        cancellingFiles = true
        activity.lifecycleScope.launch(Dispatchers.IO) { try { cancellation.input?.close() } catch (_: Exception) { } }
    }

    // ---- The card in the slot, as Cards knows it ----

    // The running machine's folder, when it's one of the saved machines Cards looks after.
    private val machineDir: File?
        get() {
            val dir = FrondActivity.running?.session?.let { File(it).absoluteFile.parentFile } ?: return null
            return dir.takeIf { it.parentFile?.canonicalFile == sessions.canonicalFile }
        }
    private val sessions: File get() = Sessions.folder(activity.applicationContext)

    // What the card row says: the card's name or "No card"; a linked card the slot is empty for,
    // with where it is (null when it's free); and whether an SD exchange is pending.
    data class SlotShown(val name: String, val linked: String? = null, val linkedId: String? = null,
                         val linkedHolder: String? = null, val pending: Boolean = false)
    var slotShown by mutableStateOf(SlotShown("No card"))
        private set

    private fun slotInfo(s: SdCard.State): SlotShown {
        val dir = machineDir
        val pending = SdCard.pendingFor(activity.applicationContext, FrondActivity.running?.session) != null
        if (dir == null) return SlotShown(if (s == SdCard.State.NONE) "No card" else device.cardLabel, pending = pending)
        val library = Cards.libraryFor(sessions)
        val slot = Cards.readSlot(dir)
        val file = Cards.cardFileIn(dir)
        val name = when {
            s == SdCard.State.NONE -> "No card"
            file != null && slot.card != null -> Cards.readCard(Cards.entryDir(library, slot.card))?.name ?: Cards.kindName(Cards.kindOfFile(file))
            file != null -> Cards.kindName(Cards.kindOfFile(file))
            else -> device.cardLabel
        }
        if (s != SdCard.State.NONE || slot.link == null) return SlotShown(name, pending = pending)
        val card = Cards.readCard(Cards.entryDir(library, slot.link)) ?: return SlotShown(name, pending = pending)
        val free = File(Cards.entryDir(library, slot.link), Cards.fileOfKind(card.kind)).isFile
        val holder = if (free) null else card.machine?.let { Cards.machineName(File(sessions, it)) } ?: "another machine"
        return SlotShown(name, card.name, card.id, holder, pending)
    }

    // The picker: its choices, the one picked and the card the slot holds now.
    class Picker(val title: String, val choices: List<CardChoices.Choice>, val selected: String?, val current: String?)
    var picker by mutableStateOf<Picker?>(null)
    var pickerLinking by mutableStateOf<Pair<Cards.Card, List<CardChoices.LinkOption>>?>(null)
    var pickerMaking by mutableStateOf(false)
    // The picker's OK on an inserted card waits for the eject to be confirmed.
    var confirmingChange by mutableStateOf(false)
    @Volatile private var cardCancel: java.util.concurrent.atomic.AtomicBoolean? = null

    fun openPicker(select: String? = null) {
        val dir = machineDir ?: return
        if (busy != null || waiting() || slotShown.pending) return
        val keep = picker?.selected
        val reopening = picker != null
        activity.lifecycleScope.launch {
            val shown = withContext(Dispatchers.IO) {
                Cards.reconcile(sessions, dir)
                val slot = Cards.readSlot(dir)
                val current = if (Cards.cardFileIn(dir) != null) slot.card else null
                val choices = CardChoices.pickerChoices(sessions, dir, device)
                val title = "${if (device.memoryStick) "Memory Stick" else "SD card"} for $name"
                Picker(title, choices, CardChoices.pickerStart(choices, select ?: if (reopening) keep else current), current)
            }
            picker = shown
        }
    }

    fun pick(id: String?) {
        picker = picker?.let { Picker(it.title, it.choices, id, it.current) }
    }

    fun pickerOk() {
        val p = picker ?: return
        if (p.selected == p.current) { picker = null; return }
        if (state == SdCard.State.INSERTED) { confirmingChange = true; return }
        picker = null
        changeCard(p.selected)
    }

    fun confirmChange(yes: Boolean) {
        confirmingChange = false
        val p = picker ?: return
        if (!yes) return
        picker = null
        changeCard(p.selected)
    }

    // Eject (confirmed), take the old card out to the drawer, then put the new one in. The old
    // card is never lost: a failed put-in sends the new one back to the drawer.
    fun changeCard(to: String?) {
        val dir = machineDir ?: return
        job("Changing the card", "Couldn't change the card") { c ->
            if (c.state() == SdCard.State.INSERTED) c.eject()
            if (c.state() != SdCard.State.NONE) {
                val (identity, protected) = c.takeOut()
                Cards.tookOut(sessions, dir, identity, protected)?.let { throw SdCard.Failed(it) }
            }
            if (to == null) return@job
            val (card, error) = Cards.toRunning(sessions, dir, to)
            if (card == null) throw SdCard.Failed(error ?: "Couldn't find that card.")
            try {
                c.putIn(Cards.hex(card.identity.copyOf(32)), card.writeProtected)
            } catch (e: Exception) {
                Cards.putInFailed(sessions, dir, to)
                throw e
            }
        }
    }

    // A linked card that's free goes in on a tap.
    fun insertLinked() {
        val id = slotShown.linkedId ?: return
        if (slotShown.linkedHolder == null) changeCard(id)
    }

    // A new card from the picker, made on a worker with progress and Cancel, then selected.
    fun makeCard(cardName: String, kind: String, size: Int) {
        pickerMaking = false
        if (busy != null) return
        val cancel = java.util.concurrent.atomic.AtomicBoolean(false)
        cardCancel = cancel
        canCancelFiles = true
        cancellingFiles = false
        busy = "Making the card"
        progress = null
        activity.lifecycleScope.launch {
            var made: String? = null
            val error = withContext(Dispatchers.IO) {
                Cards.makeCard(sessions, cardName, kind, size, { cancel.get() }, { done, total ->
                    if (total > 0) activity.runOnUiThread { if (busy != null) progress = done.toFloat() / total }
                }) { made = it }
            }
            cardCancel = null
            busy = null
            progress = null
            canCancelFiles = false
            cancellingFiles = false
            when (error) {
                null -> openPicker(made)
                COPY_CANCELLED -> note = "Cancelled"
                else -> alert = "Couldn't make the card" to error
            }
        }
    }

    fun startPickerLink() {
        val id = picker?.selected ?: return
        val dir = machineDir ?: return
        activity.lifecycleScope.launch {
            pickerLinking = withContext(Dispatchers.IO) {
                Cards.readCard(Cards.entryDir(Cards.libraryFor(sessions), id))?.let { it to CardChoices.linkOptions(sessions, id, dir, dir) }
            }
        }
    }

    fun linkFromPicker(card: Cards.Card, machines: List<File>) {
        pickerLinking = null
        val dir = machineDir ?: return
        activity.lifecycleScope.launch {
            val moved = mutableListOf<String>()
            val error = withContext(Dispatchers.IO) { Cards.setLinks(sessions, card.id, machines, dir, moved) }
            if (error != null) alert = "Couldn't link the card" to error
            else if (moved.isNotEmpty()) note = "${moved.joinToString(", ")} now linked to ${card.name}"
            openPicker(card.id)
            refresh()
        }
    }

    // Database work through the control link; leaves the card state alone.
    fun palm(label: String, failTitle: String, work: (PalmDatabases) -> Unit) {
        val d = databases ?: return
        if (busy != null || waiting()) return
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

    // Installs the picked files one after the other, in the order picked; a failure doesn't stop
    // the rest. One busy state for the batch, and one alert naming what failed.
    fun install(uris: List<android.net.Uri>) {
        val d = databases ?: return
        if (uris.isEmpty() || busy != null || waiting()) return
        busy = "Installing"
        activity.lifecycleScope.launch {
            var last = ""
            val result = withContext(Dispatchers.IO) {
                InstallBatch.run(uris, { displayName(it) ?: "A file" }, { n, total ->
                    if (total > 1) activity.runOnUiThread { busy = "Installing $n of $total" }
                }) { last = d.install(it) }
            }
            busy = null
            val summary = result.summary()
            if (summary != null) alert = "Couldn't install" to summary
            else note = if (uris.size == 1) last else "${uris.size} applications installed"
            loadApps()
        }
    }

    // The applications in RAM, as the core lists them; null until the first read.
    var apps by mutableStateOf<List<PalmDatabases.Info>?>(null)
        private set

    fun loadApps() {
        val d = databases ?: return
        if (waitingForGdb) return
        activity.lifecycleScope.launch {
            apps = withContext(Dispatchers.IO) {
                try { RunningSheets.applications(d.list()) } catch (e: Exception) {
                    Log.w(FrondActivity.TAG, "application list: ${e.message}")
                    emptyList()
                }
            }
        }
    }

    private fun displayName(uri: android.net.Uri): String? = try {
        activity.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }

    // Asks for a name at once and fills the list of RAM databases in when the core has read it.
    // A failed list still lets you type a name.
    var listing by mutableStateOf(false)
        private set
    private var asks = 0

    fun askName(what: String) {
        val d = databases ?: return
        if (busy != null || naming != null || waiting()) return
        found = emptyList()
        naming = what
        listing = true
        val asked = ++asks
        activity.lifecycleScope.launch {
            val list = withContext(Dispatchers.IO) {
                try { d.list() } catch (e: Exception) {
                    Log.w(FrondActivity.TAG, "database list: ${e.message}")
                    emptyList()
                }
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

    fun delete(name: String) = palm("Deleting", "Couldn't delete") { d ->
        try {
            d.deleteApplication(name)
            activity.runOnUiThread { note = "$name deleted" }
        } finally {
            activity.runOnUiThread { loadApps() }
        }
    }

    fun export(to: android.net.Uri, name: String?) = palm("Exporting", "Couldn't export") { d ->
        d.export(to, name)
        activity.runOnUiThread { note = if (name == null) "RAM databases exported" else "$name exported" }
    }

    // Device menu work through the control link; returns to the Palm when done if back is set.
    private fun device(label: String, failTitle: String, back: Boolean, work: (ControlLink) -> String?) {
        val l = link ?: return
        if (busy != null || waiting()) return
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
        if (waitingForGdb) return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("keyboard-state", timeoutSeconds = 30) }
            if (reply.ok) keyboard = reply.message == "attached"
        }
    }

    private val sessionId = Sessions.forPath(activity.applicationContext, FrondActivity.running?.session)?.id
    private fun machineSettings() = sessionId?.let { Sessions.read(activity.applicationContext, it)?.settings } ?: MachineSettings()

    // The physical switch is part of the guest session, independent of machine preferences.
    var hold by mutableStateOf<Boolean?>(null)
        private set

    fun readHold() {
        val l = link ?: return
        if (!device.holdPresent || waitingForGdb) return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("hold-state", timeoutSeconds = 30) }
            hold = if (reply.ok) when (reply.message) { "on" -> true; "off" -> false; else -> null } else null
        }
    }

    fun changeHold(on: Boolean) {
        if (!device.holdSupported || hold == null) return
        device("Changing Hold", "Couldn't change Hold", false) { l ->
            val changed = l.request("hold", if (on) "on" else "off", timeoutSeconds = 30)
            val current = l.request("hold-state", timeoutSeconds = 30)
            val actual = if (current.ok) when (current.message) { "on" -> true; "off" -> false; else -> null } else null
            activity.runOnUiThread { hold = actual }
            if (!changed.ok) changed.message else if (!current.ok) current.message else null
        }
    }

    // The saved machine settings. Sound applies when the Palm shows again, display changes go to the core at
    // once and show with the Palm, and the audio preview applies at the next start.
    var settings by mutableStateOf(machineSettings())
        private set

    // Saves one field of the machine settings, leaving the others as they are on disk.
    fun changeSettings(next: MachineSettings) {
        val id = sessionId ?: return
        val old = settings
        try {
            Sessions.updateSettings(activity.applicationContext, id) { cur ->
                var s = cur
                if (next.size != old.size) s = s.copy(size = next.size)
                if (next.smoothing != old.smoothing) s = s.copy(smoothing = next.smoothing)
                if (next.contrast != old.contrast) s = s.copy(contrast = next.contrast, contrastSet = true)
                if (next.speed != old.speed && device.runs68k) s = s.copy(speed = next.speed)
                if (next.sound != old.sound && device.sound) s = s.copy(sound = next.sound)
                if (next.showKeyboard != old.showKeyboard) s = s.copy(showKeyboard = next.showKeyboard)
                if (next.jogControls != old.jogControls) s = s.copy(jogControls = next.jogControls)
                if (next.barMode != old.barMode) s = s.copy(barMode = next.barMode)
                if (next.fastAudio != old.fastAudio && device.lowLatencyAudio) s = s.copy(fastAudio = next.fastAudio)
                s
            }
            settings = machineSettings()
            FrondActivity.running?.applyDisplay()
            FrondActivity.running?.applySpeed()
            FrondActivity.running?.applyBarMode()
        } catch (e: Exception) {
            alert = "Couldn't save settings" to "Couldn't write the machine settings."
        }
    }

    var microphone by mutableStateOf("off")
        private set
    private val microphoneOwner = FrondActivity.running

    fun readMicrophone() {
        if (!device.microphone) return
        activity.lifecycleScope.launch {
            val state = withContext(Dispatchers.IO) { link?.request("microphone-state", timeoutSeconds = 10) }
            if (state?.ok == true && microphone != "pending") microphone = state.message
        }
    }

    fun changeMicrophone(on: Boolean) {
        if (!device.microphone || FrondActivity.running !== microphoneOwner) return
        microphoneOwner?.microphoneOnReturn(on)
        val id = sessionId ?: return
        try { Sessions.updateSettings(activity.applicationContext, id) { it.copy(microphone = on) } }
        catch (e: Exception) { alert = "Couldn't save settings" to "Couldn't write the machine settings." }
        if (on) microphone = "pending"
        else {
            microphone = "off"
            activity.lifecycleScope.launch { withContext(Dispatchers.IO) { link?.request("microphone-mode", "off", timeoutSeconds = 10) } }
        }
    }

    // The request has gone to the core: reads what it says now.
    fun settleMicrophone() {
        if (microphone == "pending") microphone = "off"
        readMicrophone()
    }

    fun cancelMicrophoneRequest() {
        microphoneOwner?.microphoneOnReturn(false)
        if (microphone == "pending") microphone = "off"
    }

    fun microphoneDenied() { changeMicrophone(false); microphone = "off Permission denied" }

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
        if (waitingForGdb) return
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

    // Infrared beaming over the network, as the relay says. Kept up to date while the menu is open.
    var beam by mutableStateOf(IrBeam.status.value)
        private set

    fun watchBeam() {
        activity.lifecycleScope.launch {
            IrBeam.status.collect {
                val changed = it.phase != beam.phase
                beam = it
                // Beaming ending can come with the core restoring the keyboard from a loaded session.
                if (changed) readKeyboard()
            }
        }
    }

    fun changeBeam(mode: BeamMode) = device("Changing infrared beaming", "Couldn't change infrared beaming", false) { l ->
        IrBeam.set(activity.applicationContext, l, mode)
    }

    fun retryBeam() = IrBeam.retry()

    // The serial port and USB HotSync over the network, as their relays say.
    var serial by mutableStateOf(SerialLink.status.value)
        private set
    var usb by mutableStateOf(UsbLink.status.value)
        private set

    // The debugger's relay, on only when Settings turned it on for this run.
    var debugger by mutableStateOf(DebugLink.status.value)
        private set

    fun watchLinks() {
        activity.lifecycleScope.launch { SerialLink.status.collect { serial = it } }
        activity.lifecycleScope.launch { UsbLink.status.collect { usb = it } }
        activity.lifecycleScope.launch { DebugLink.status.collect { debugger = it } }
    }

    fun changeSerial(mode: BeamMode) = device("Changing the serial port", "Couldn't change the serial port", false) { l ->
        SerialLink.set(activity.applicationContext, l, mode)
    }

    fun changeUsb(mode: BeamMode) = device("Changing USB HotSync", "Couldn't change USB HotSync", false) { l ->
        UsbLink.set(activity.applicationContext, l, mode)
    }

    // Bluetooth over the network, as the relay says, and what the core says about the Palm's radio.
    var bluetooth by mutableStateOf(BtLink.status.value)
        private set
    var radio by mutableStateOf<String?>(null)
        private set
    private var bluetoothWatcher: Job? = null
    private var bluetoothRefresh: Job? = null
    private var bluetoothForeground = false
    private var bluetoothEpoch = 0L
    private var radioReading = false

    fun watchBluetooth() {
        if (bluetoothWatcher != null) return
        bluetoothWatcher = activity.lifecycleScope.launch {
            BtLink.status.collect {
                bluetooth = it
                ++bluetoothEpoch
                radio = null
                readRadio()
            }
        }
    }

    fun startBluetoothRefresh() {
        if (bluetoothForeground) return
        bluetoothForeground = true
        ++bluetoothEpoch
        radio = null
        bluetoothRefresh = activity.lifecycleScope.launch {
            while (isActive) {
                readRadio()
                delay(1000)
            }
        }
    }

    fun stopBluetoothRefresh() {
        bluetoothForeground = false
        ++bluetoothEpoch
        bluetoothRefresh?.cancel()
        bluetoothRefresh = null
    }

    fun readRadio() {
        val l = link ?: return
        if (FrondActivity.running?.control !== l) {
            radio = null
            return
        }
        if (!device.bluetooth || !bluetoothForeground || waitingForGdb || radioReading) return
        val epoch = bluetoothEpoch
        radioReading = true
        activity.lifecycleScope.launch {
            try {
                val reply = withContext(Dispatchers.IO) { l.readBluetoothState() }
                if (reply != null && bluetoothForeground && epoch == bluetoothEpoch && FrondActivity.running?.control === l) {
                    radio = if (reply.ok) reply.message else null
                }
            } finally {
                // Pausing stops the scheduler; the blocking request keeps its
                // single-flight slot until the control wait actually returns.
                radioReading = false
            }
        }
    }

    fun changeBluetooth(mode: BtMode) = device("Changing Bluetooth", "Couldn't change Bluetooth", false) { l ->
        BtLink.set(activity.applicationContext, l, mode).also { activity.runOnUiThread { readRadio() } }
    }

    // Both go back to the Palm, where the reset or rotation shows.
    fun reset() = device("Resetting", "Couldn't reset", true) { send(it, "reset") }

    fun rotate() = device("Rotating", "Couldn't rotate", true) { send(it, "rotate") }

    // Presses the cradle's HotSync button and goes back to the Palm.
    fun hotSync() = device("Starting HotSync", "Couldn't start HotSync", true) { send(it, "hotsync") }

    // Erases the Palm, after saving the running session when saveFirst is set.
    fun hardReset(saveFirst: Boolean) = device(if (saveFirst) "Saving" else "Resetting", "Couldn't hard reset", true) { l ->
        runHardReset(FrondActivity.running?.session, saveFirst, onSaved = { FrondActivity.running?.savedMachine() }) { command, argument ->
            l.request(command, argument, timeoutSeconds = if (command == "save") 120 else 30)
        }
    }

    fun saveCopy(to: android.net.Uri) = device("Saving", "Couldn't save a copy", false) { l ->
        Sessions.saveCopy(activity.applicationContext, l, to, FrondActivity.running?.session).also { if (it == null) activity.runOnUiThread { note = "Copy saved" } }
    }

    // The core's log while the log view is open, or null when it's closed.
    var log by mutableStateOf<String?>(null)

    fun showLog() {
        activity.lifecycleScope.launch {
            log = withContext(Dispatchers.IO) { CoreLog.read(activity.applicationContext) }
        }
    }

    companion object {
        // Sends hard-reset, after a save of path when saveFirst is set; a failed save erases nothing.
        // onSaved runs after a good save. Returns an error message or null.
        fun runHardReset(path: String?, saveFirst: Boolean, onSaved: () -> Unit = {}, request: (String, String) -> ControlLink.Reply): String? {
            if (saveFirst) {
                if (path == null) return "There's no session to save."
                val saved = request("save", path)
                if (!saved.ok) return saved.message
                onSaved()
            }
            val reply = request("hard-reset", "")
            return if (reply.ok) null else reply.message
        }
    }

    fun refresh() {
        if (device.cardSupported) job("Reading", "Couldn't read the card") { }
    }

    // Lists the open folder again after a change.
    private fun relist(c: SdCard, wanted: String) {
        // The card's folders as the card names them.
        val at = c.read { CardFiles.realPath(it, wanted) }
        val listing = c.list(at)
        activity.runOnUiThread {
            path = at
            entries = listing.entries
            free = listing.free
            total = listing.total
        }
    }

    override fun open(at: String) { job("Reading", "Couldn't open the folder", cancellable = true) { c -> relist(c, at) } }

    override fun up() {
        val p = path ?: return
        if (p == "/") return
        open(p.substringBeforeLast('/').ifEmpty { "/" })
    }

    fun openFiles() = job("Ejecting", "Couldn't open the card", cancellable = true) { c ->
        c.beginEdit()
        val found = SHORTCUTS.filter { c.hasFolder(it) }
        activity.runOnUiThread { shortcuts = found }
        relist(c, "/")
    }

    private fun finishMutation(c: SdCard) {
        c.finishMutation()
        activity.runOnUiThread { canCancelFiles = false; cancellingFiles = false }
    }

    private fun resolver(several: Boolean) = CardFiles.Resolver { name -> prompt.ask(name, several) { activity.runOnUiThread(it) } }

    private fun progressed(fraction: Float) = activity.runOnUiThread { if (busy != null) progress = fraction }

    // A change to the card, then the folder listed again.
    private fun change(label: String, failTitle: String, done: String? = null, work: (SdCard, String) -> String?) {
        val at = path ?: return
        job(label, failTitle, cancellable = true) { c ->
            val text = work(c, at)
            finishMutation(c)
            relist(c, at)
            (done ?: text)?.let { activity.runOnUiThread { note = it } }
        }
    }

    private fun counted(report: CardFiles.Report, what: String): String =
        "${report.done} $what" + if (report.skipped > 0) ", ${report.skipped} skipped" else ""

    override fun addFiles(uris: List<Uri>) = change("Copying", "Couldn't add the files") { c, at ->
        val (added, skipped) = c.addFiles(at, uris, resolver(uris.size > 1), ::progressed)
        counted(CardFiles.Report(added, skipped), "added")
    }

    override fun addFolder(tree: Uri) = change("Copying", "Couldn't add the folder") { c, at ->
        val root = FolderCard.Document.root(activity.applicationContext.contentResolver, tree)
        counted(c.addTree(at, root, resolver(true), ::progressed), "files added")
    }

    override fun makeFolder(name: String) = change("Copying", "Couldn't make the folder") { c, at -> c.makeFolder(at, name); null }

    override fun renameEntry(entry: FatImage.Entry, newName: String) =
        change("Renaming", "Couldn't rename the entry") { c, at -> c.renameEntry(at, entry.name, newName); null }

    override fun deleteEntries(entries: List<FatImage.Entry>) =
        change("Deleting", "Couldn't delete", "Deleted") { c, at -> c.deleteEntries(at, entries.map { it.name }); null }

    override fun paste(sources: List<String>, move: Boolean) = change(if (move) "Moving" else "Copying", "Couldn't paste") { c, at ->
        counted(c.paste(sources, at, move, resolver(sources.size > 1), ::progressed), if (move) "moved" else "copied")
    }

    override fun exportFile(entry: FatImage.Entry, to: Uri) {
        job("Copying", "Couldn't export the file", cancellable = true) { c ->
            c.exportFile(entry, to)
            activity.runOnUiThread { note = "${entry.name} exported" }
        }
    }

    override fun exportEntries(entries: List<FatImage.Entry>, tree: Uri) {
        val at = path ?: return
        job("Copying", "Couldn't copy to the folder", cancellable = true) { c ->
            val host = DocumentHost.root(activity.applicationContext.contentResolver, tree)
            val n = c.exportEntries(at, entries, host, ::progressed)
            activity.runOnUiThread { note = "$n files copied" }
        }
    }

    // As the machine stops: a job on the card is cancelled and the card goes back in before the
    // final save. Blocks; not on the main thread.
    fun putCardBackForStop() {
        val c = card ?: return
        c.cancelJob()
        try { c.finishEdit() } catch (e: Exception) { Log.w(FrondActivity.TAG, "card put back at stop: ${e.message}") }
    }

    // Files… on Internal, while it's open.
    var internal by mutableStateOf<InternalFiles?>(null)
        private set

    fun openInternal() {
        val d = databases ?: return
        if (busy != null || waiting() || internal != null) return
        internal = InternalFiles(activity, d) { internal = null }.also { it.refresh() }
    }

    // Leaving the file view puts the card back; its changes are already on it.
    override fun leaveFiles() = insertEdited()

    // Closes the file view and puts the card back in the Palm, edited or not. The insert waits for
    // the Palm to notice the card was out, up to a few seconds, so the busy label stays up.
    override fun insertEdited() { job("Inserting", "Couldn't insert the card") { c ->
        c.finishEdit()
        activity.runOnUiThread { path = null }
    } }

}

// Card folders the file view offers a shortcut to when they exist.
private val SHORTCUTS = listOf("/PALM/Launcher", "/DCIM")
