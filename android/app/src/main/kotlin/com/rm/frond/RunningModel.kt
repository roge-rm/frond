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
    private var secondWriteProtected by mutableStateOf<Boolean?>(null)
    fun protectionOf(n: Int) = if (n == 0) writeProtected else secondWriteProtected
    override val title get() = device.cardLabel
    override val canWrite get() = (!device.memoryStick && !device.sdWriteProtection) || protectionOf(cardSlot) == false
    override val leaveLabel get() = "Insert"
    override val prompt = ClashPrompt()
    // The second SD slot: the Zodiac's and the Dana's.
    val twoSlots = Cards.hasSlot(device, 1)
    var secondState by mutableStateOf(SdCard.State.UNKNOWN)
        private set
    var secondShown by mutableStateOf(SlotShown("No card"))
        private set
    fun stateOf(n: Int) = if (n == 0) state else secondState
    fun shownOf(n: Int) = if (n == 0) slotShown else secondShown
    // The slot jobs, the picker and the file view work on, set before each starts.
    var cardSlot by mutableStateOf(0)
        private set
    // Picks slot `n` for the next card job; false while the file view has the other slot's card out.
    fun selectSlot(n: Int): Boolean {
        val c = card ?: return false
        if (busy != null || !c.select(n)) return false
        cardSlot = n
        return true
    }
    // Add device's cards per slot, put in one after another once the slots read empty.
    private val startCards = mutableListOf<String>()
    fun putInAtStart(cards: List<String>) { startCards.clear(); startCards += cards }
    private fun nextStartCard() {
        while (startCards.isNotEmpty() && busy == null && path == null) {
            val n = Cards.SLOTS - startCards.size
            // Not read yet: wait for the next read.
            if (stateOf(n) == SdCard.State.UNKNOWN) return
            val id = startCards.removeAt(0)
            if (id.isEmpty() || stateOf(n) != SdCard.State.NONE || !selectSlot(n)) continue
            changeCard(id)
            return
        }
    }
    // Slot `n`'s state. A library card the core left out at start goes in when the first read the
    // core answers finds the slot empty: the Zodiac's slot 2 card when it changed while the
    // machine was off, and either slot's card on frond-68k, which takes cards by path once it
    // runs. Not while the file view has a card out. A put-in that fails is said once.
    private val startPutIns = BooleanArray(Cards.SLOTS)
    @Volatile private var startPutInError: Pair<String, String>? = null
    private fun slotState(c: SdCard, n: Int): SdCard.State = synchronized(startPutIns) { leftOutState(c, n) }
    private fun leftOutState(c: SdCard, n: Int): SdCard.State {
        if (n != 0 && !twoSlots) return SdCard.State.UNKNOWN
        var s = c.stateOf(n)
        // Only the first read the core answers counts.
        if (s == SdCard.State.UNKNOWN || startPutIns[n] || SdCard.editing != null) return s
        startPutIns[n] = true
        if (s != SdCard.State.NONE || (n == 0 && !device.runs68k)) return s
        val dir = machineDir ?: return s
        val argument = if (device.runs68k) Cards.startAttachArgument(sessions, dir, n) else Cards.secondPutIn(sessions, dir)
        if (argument != null) try {
            c.putInAtStart(n, argument)
            s = c.stateOf(n)
        } catch (e: Exception) {
            Log.w(FrondActivity.TAG, "card ${n + 1} put-in: ${e.message}")
            startPutInError = (if (twoSlots) "Couldn't put card ${n + 1} in" else "Couldn't put the card in") to SdCard.message(e)
        }
        return s
    }
    private fun sayStartPutInError() {
        startPutInError?.let { alert = it }
        startPutInError = null
    }
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
    // Shown for an empty list in the by-name dialog, e.g. while the Palm is asleep.
    var foundNote by mutableStateOf<String?>(null)

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
                readGamepad()
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
            val s = withContext(Dispatchers.IO) { slotState(c, 0) }
            val protection = withContext(Dispatchers.IO) { c.protection(0, s == SdCard.State.INSERTED || s == SdCard.State.EJECTED) }
            val pro = withContext(Dispatchers.IO) { c.memoryStickPro() }
            val slot = withContext(Dispatchers.IO) { slotInfo(s) }
            val second = withContext(Dispatchers.IO) { slotState(c, 1) }
            val protection2 = withContext(Dispatchers.IO) {
                if (twoSlots) c.protection(1, second == SdCard.State.INSERTED || second == SdCard.State.EJECTED) else null
            }
            val shown2 = withContext(Dispatchers.IO) { if (twoSlots) slotInfo(second, 1) else SlotShown("No card") }
            slotShown = slot
            state = s
            secondState = second
            secondShown = shown2
            writeProtected = protection
            secondWriteProtected = protection2
            memoryStickPro = pro
            changed = c.changed
            link?.sdProgress = null
            busy = null
            progress = null
            canCancelFiles = false
            cancellingFiles = false
            if (error != null) alert = failTitle to error else sayStartPutInError()
            nextStartCard()
        }
    }

    // The card's state without a busy dialog, for the bar's icon.
    fun readCard() {
        val c = card ?: return
        if (busy != null || waitingForGdb || !device.cardSupported) return
        activity.lifecycleScope.launch {
            val s = withContext(Dispatchers.IO) { slotState(c, 0) }
            val protection = withContext(Dispatchers.IO) { c.protection(0, s == SdCard.State.INSERTED || s == SdCard.State.EJECTED) }
            val pro = withContext(Dispatchers.IO) { c.memoryStickPro() }
            val slot = withContext(Dispatchers.IO) { slotInfo(s) }
            if (busy != null) return@launch
            val second = withContext(Dispatchers.IO) { slotState(c, 1) }
            val protection2 = withContext(Dispatchers.IO) {
                if (twoSlots) c.protection(1, second == SdCard.State.INSERTED || second == SdCard.State.EJECTED) else null
            }
            val shown2 = withContext(Dispatchers.IO) { if (twoSlots) slotInfo(second, 1) else SlotShown("No card") }
            if (busy != null) return@launch
            slotShown = slot
            state = s
            secondState = second
            secondShown = shown2
            writeProtected = protection
            secondWriteProtected = protection2
            memoryStickPro = pro
            sayStartPutInError()
            nextStartCard()
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
    // with where it is (null when it's free); whether an SD exchange is pending; and whether it's a
    // frond-68k card still linked from where an older Frond kept it (it couldn't be moved into
    // Cards), which can be ejected, put back and exported, but not changed or opened in Files.
    data class SlotShown(val name: String, val linked: String? = null, val linkedId: String? = null,
                         val linkedHolder: String? = null, val pending: Boolean = false, val oldPlace: Boolean = false)
    var slotShown by mutableStateOf(SlotShown("No card"))
        private set

    private fun slotInfo(s: SdCard.State, n: Int = 0): SlotShown {
        val dir = machineDir
        val pending = n == 0 && SdCard.pendingFor(activity.applicationContext, FrondActivity.running?.session) != null
        val label = if (n == 0) device.cardLabel else "SD card"
        if (dir == null) return SlotShown(if (s == SdCard.State.NONE) "No card" else label, pending = pending)
        val library = Cards.libraryFor(sessions)
        val slot = Cards.readSlot(dir, n)
        val file = Cards.cardFileIn(dir, n)
        if (device.runs68k && file == null && (s == SdCard.State.INSERTED || s == SdCard.State.EJECTED)) return SlotShown(label, oldPlace = true)
        val name = when {
            s == SdCard.State.NONE -> "No card"
            file != null && slot.card != null -> Cards.readCard(Cards.entryDir(library, slot.card))?.name ?: Cards.kindName(Cards.kindOfFile(file))
            file != null -> Cards.kindName(Cards.kindOfFile(file))
            else -> label
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

    fun openPicker(select: String? = null, n: Int = cardSlot) {
        val dir = machineDir ?: return
        if (busy != null || waiting() || shownOf(n).pending || shownOf(n).oldPlace || !selectSlot(n)) return
        val keep = picker?.selected
        val reopening = picker != null
        activity.lifecycleScope.launch {
            val shown = withContext(Dispatchers.IO) {
                Cards.reconcile(sessions, dir)
                val slot = Cards.readSlot(dir, n)
                val current = if (Cards.cardFileIn(dir, n) != null) slot.card else null
                val choices = CardChoices.pickerChoices(sessions, dir, device, n)
                val title = if (twoSlots) "Card ${n + 1} for $name" else "${if (device.memoryStick) "Memory Stick" else "SD card"} for $name"
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
        if (stateOf(cardSlot) == SdCard.State.INSERTED) { confirmingChange = true; return }
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
            val n = c.slot
            if (c.state() != SdCard.State.NONE) {
                val (identity, protected) = c.takeOut()
                Cards.tookOut(sessions, dir, identity, protected, n)?.let { throw SdCard.Failed(it) }
            }
            if (to == null) return@job
            val (card, error) = Cards.toRunning(sessions, dir, to, n)
            if (card == null) throw SdCard.Failed(error ?: "Couldn't find that card.")
            try {
                c.putIn(Cards.hex(card.identity.copyOf(32)), card.writeProtected)
            } catch (e: Exception) {
                Cards.putInFailed(sessions, dir, to, n)
                throw e
            }
        }
    }

    // A linked card that's free goes in on a tap.
    fun insertLinked(n: Int = 0) {
        val shown = shownOf(n)
        val id = shown.linkedId ?: return
        if (shown.linkedHolder == null && selectSlot(n)) changeCard(id)
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
                Cards.readCard(Cards.entryDir(Cards.libraryFor(sessions), id))?.let { it to CardChoices.linkOptions(sessions, id, dir, Cards.SlotRef(dir, cardSlot)) }
            }
        }
    }

    fun linkFromPicker(card: Cards.Card, machines: List<Cards.SlotRef>) {
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
                }) { last = d.install(it, if (device.installsToCard) displayName(it) ?: "file" else null) }
            }
            busy = null
            val summary = result.summary()
            if (summary != null) alert = "Couldn't install" to summary
            else note = if (uris.size == 1) last else if (device.installsToCard) "${uris.size} files copied to the SD card" else "${uris.size} applications installed"
            loadApps()
        }
    }

    // Install on This Device is on when the core has guest services and nothing else is running.
    val canInstallFrondBench get() = databases != null && busy == null

    fun installFrondBench() {
        val d = databases ?: return
        if (busy != null || waiting()) return
        busy = "Installing"
        activity.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    d.install { FrondBench.open(activity.applicationContext) }
                    null
                } catch (e: Exception) {
                    if (e is PalmDatabases.Failed) e.message else "Couldn't install FrondBench."
                }
            }
            busy = null
            if (error != null) alert = "Couldn't install" to error else note = "FrondBench installed"
            loadApps()
        }
    }

    // The applications in RAM, as the core lists them; null until the first read.
    var apps by mutableStateOf<List<PalmDatabases.Info>?>(null)
        private set
    // Shown for an empty list instead of "No applications", e.g. while the Palm is asleep.
    var appsNote by mutableStateOf<String?>(null)
        private set

    fun loadApps() {
        val d = databases ?: return
        if (waitingForGdb) return
        activity.lifecycleScope.launch {
            var note: String? = null
            apps = withContext(Dispatchers.IO) {
                try { RunningSheets.applications(d.list()) } catch (e: Exception) {
                    Log.w(FrondActivity.TAG, "application list: ${e.message}")
                    if (e is PalmDatabases.Failed && e.code == "asleep") note = "The Palm is asleep"
                    emptyList()
                }
            }
            appsNote = note
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
        foundNote = null
        naming = what
        listing = true
        val asked = ++asks
        activity.lifecycleScope.launch {
            var note: String? = null
            val list = withContext(Dispatchers.IO) {
                try { d.list() } catch (e: Exception) {
                    Log.w(FrondActivity.TAG, "database list: ${e.message}")
                    if (e is PalmDatabases.Failed && e.code == "asleep") note = "The Palm is asleep"
                    emptyList()
                }
            }
            // A later question has its own list coming.
            if (asked != asks) return@launch
            foundNote = note
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
        if (!device.infraredKeyboard) { keyboard = false; return }
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

    // The one background notification for both printers.
    private val printNotices = PrintNotices(activity.applicationContext, device)

    // The Dana's USB printer; null on any other device.
    val printer: PrinterModel? = if (PrintJob.rowShown(device) && link != null)
        PrinterModel(activity, device, link, sessionId, UsbPort.forStart(device, settings.usbPort), settings.wifiModule, printNotices, { FrondActivity.running?.inBackground == true }, { settings.infrared == InfraredChoice.PRINTER }) { alert = "USB printer" to it }
    else null

    // The infrared printer's jobs, on every device with infrared; polled while the infrared choice is Printer.
    val irPrinter: PrintJobs? = if (device.infrared && link != null)
        PrintJobs(activity, device, link, "irprinter", "print-ir", "infrared printer", printNotices, { FrondActivity.running?.inBackground == true },
            { if (printer?.plugged == true) "infrared printer" else null }) { alert = "Infrared printer" to it }
            .also { it.startPolling { settings.infrared == InfraredChoice.PRINTER } }
    else null

    // The core's Dana view changed (keyboard button, pan, zoom); keeps the sheet's choices current.
    fun acceptDanaView(view: DanaView) {
        if (settings.danaView != view) settings = settings.copy(danaView = view)
    }

    // Saves one field of the machine settings, leaving the others as they are on disk.
    fun changeSettings(next: MachineSettings) {
        val id = sessionId ?: return
        val old = settings
        if (device.isDana() && next.danaView != old.danaView) {
            val running = FrondActivity.running
            if (running == null) {
                alert = "Couldn't change display" to "The emulator isn't running."
                return
            }
            running.changeDanaView(next.danaView) { _, error ->
                settings = machineSettings()
                if (error != null) alert = "Couldn't change display" to error
            }
            return
        }
        try {
            Sessions.updateSettings(activity.applicationContext, id) { cur ->
                var s = cur
                if (next.size != old.size) s = s.copy(size = next.size)
                if (next.smoothing != old.smoothing) s = s.copy(smoothing = next.smoothing)
                if (next.showMips != old.showMips) s = s.copy(showMips = next.showMips)
                if (next.contrast != old.contrast) s = s.copy(contrast = next.contrast, contrastSet = true)
                if (next.speed != old.speed) s = s.copy(speed = next.speed)
                if (next.sound != old.sound && device.sound) s = s.copy(sound = next.sound)
                if (next.showKeyboard != old.showKeyboard) s = s.copy(showKeyboard = next.showKeyboard)
                if (next.jogControls != old.jogControls) s = s.copy(jogControls = next.jogControls)
                if (next.jogLandscape != old.jogLandscape) s = s.copy(jogLandscape = next.jogLandscape)
                if (next.barMode != old.barMode) s = s.copy(barMode = next.barMode)
                if (next.fastAudio != old.fastAudio && device.lowLatencyAudio) s = s.copy(fastAudio = next.fastAudio)
                if (next.rumble != old.rumble) s = s.copy(rumble = next.rumble)
                if (next.batteryMode != old.batteryMode) s = s.copy(batteryMode = next.batteryMode)
                if (next.batteryLevel != old.batteryLevel) s = s.copy(batteryLevel = next.batteryLevel)
                if (next.batteryCharging != old.batteryCharging) s = s.copy(batteryCharging = next.batteryCharging)
                if (device.headphonesSupported && next.headphonesIn != old.headphonesIn) s = s.copy(headphonesIn = next.headphonesIn)
                s
            }
            settings = machineSettings()
            FrondActivity.running?.applyDisplay()
            FrondActivity.running?.applySpeed()
            FrondActivity.running?.applyMips()
            FrondActivity.running?.applyBarMode()
            FrondActivity.running?.applyRumble()
            FrondActivity.running?.battery?.changed(settings)
            if (settings.headphonesIn != old.headphonesIn) FrondActivity.running?.applyHeadphones()
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

    // The core's microphone state as it changed, unless a request of ours is waiting.
    fun acceptMicrophone(state: String) {
        if (device.microphone && microphone != "pending") microphone = state
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

    // The camera's source as the app chose it, and the core's camera-state.
    var camera by mutableStateOf(CameraFrames.Choice.PATTERN)
        private set
    var cameraState by mutableStateOf("pattern")
        private set

    fun readCamera() {
        if (!device.camera) return
        activity.lifecycleScope.launch {
            val state = withContext(Dispatchers.IO) { link?.request("camera-state", timeoutSeconds = 10) }
            if (state?.ok == true) cameraFromCore(state.message)
        }
    }

    // The core's state changed; anything but frames means the feed has stopped.
    fun cameraFromCore(state: String) {
        cameraState = state
        if (!CameraFrames.stillFeeding(state)) camera = CameraFrames.Choice.PATTERN
    }

    fun chooseCamera(choice: CameraFrames.Choice, picture: android.net.Uri? = null) {
        if (!device.camera || FrondActivity.running !== microphoneOwner) return
        microphoneOwner?.chooseCamera(choice, picture)
    }

    fun cameraChosen(choice: CameraFrames.Choice) {
        camera = choice
        cameraState = if (choice == CameraFrames.Choice.PATTERN) "pattern" else "frames"
    }

    fun cameraFailed(why: String) { camera = CameraFrames.Choice.PATTERN; cameraState = "pattern $why" }

    fun cameraDenied() = cameraFailed("Permission denied")

    fun attachKeyboard(on: Boolean) {
        if (!device.infraredKeyboard) return
        device(if (on) "Attaching" else "Detaching", if (on) "Couldn't attach the keyboard" else "Couldn't detach the keyboard", false) { l ->
            val reply = l.request("keyboard-attach", if (on) "on" else "off", timeoutSeconds = 30)
            if (reply.ok) activity.runOnUiThread { keyboard = reply.message == "attached" }
            if (reply.ok) null else reply.message
        }
    }

    // The CLIE game controller: see RunningMenu.State.gamepad.
    var gamepad by mutableStateOf<String?>(null)
        private set

    fun readGamepad() {
        val l = link ?: return
        if (!device.clieGamepad || waitingForGdb) return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("gamepad-state", timeoutSeconds = 30) }
            if (reply.ok) adoptGamepad(reply.message)
        }
    }

    private fun adoptGamepad(message: String) {
        gamepad = RunningMenu.gamepadState(message)
        FrondActivity.running?.gamepadStateRead(gamepad)
    }

    // Plugs the pad in or out without a reset, and keeps the choice as the machine's setting.
    fun changeGamepad(on: Boolean) {
        if (!device.clieGamepad || gamepad == null) return
        device(if (on) "Plugging in" else "Unplugging", "Couldn't change the game controller", false) { l ->
            val changed = l.request("gamepad", if (on) "on" else "off", timeoutSeconds = 30)
            val current = l.request("gamepad-state", timeoutSeconds = 30)
            if (current.ok) activity.runOnUiThread { adoptGamepad(current.message) }
            if (changed.ok) {
                sessionId?.let { id ->
                    try { Sessions.updateSettings(activity.applicationContext, id) { it.copy(gamepad = on) } } catch (_: Exception) {}
                }
            }
            if (!changed.ok) changed.message else if (!current.ok) current.message else null
        }
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

    // Infrared over a connection, as IrLink says. Kept up to date while the menu is open.
    var beam by mutableStateOf(IrLink.status.value)
        private set

    fun watchBeam() {
        activity.lifecycleScope.launch {
            IrLink.status.collect {
                val changed = it.active != beam.active
                beam = it
                // Beaming ending can come with the core restoring the keyboard from a loaded session.
                if (changed) readKeyboard()
            }
        }
    }

    // The infrared setting is kept with the machine.
    fun changeBeam(choice: String) = device("Changing infrared beaming", "Couldn't change infrared beaming", false) { l ->
        val leftPrinter = settings.infrared == InfraredChoice.PRINTER && choice != InfraredChoice.PRINTER
        sessionId?.let { id -> Sessions.updateSettings(activity.applicationContext, id) { it.copy(infrared = choice) } }
        activity.runOnUiThread { settings = settings.copy(infrared = choice) }
        if (InfraredChoice.linkId(choice) != null) FrondActivity.running?.askLocalNetwork()
        val error = IrLink.apply(activity.applicationContext, l, choice, name, device.label)
        // The core throws its printer output away when the mode changes, but only if it did change.
        if (leftPrinter && error == null) irPrinter?.dropAll()
        error
    }

    // The serial port over a connection, as SerialLink says, and USB HotSync over the network, as its relay says.
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

    var serialSync by mutableStateOf(false)
        private set
    var usbSync by mutableStateOf(false)
        private set

    fun acceptSyncRoute(port: String, state: String) {
        if (SyncProtocol.forDevice(device) != SyncProtocol.HOT_SYNC) return
        val attached = state == "sync" || state.startsWith("sync ")
        if (port == "serial") serialSync = attached
        if (port == "usb") usbSync = attached
    }

    fun readSyncPorts() {
        if (SyncProtocol.forDevice(device) != SyncProtocol.HOT_SYNC) return
        val l = link ?: return
        activity.lifecycleScope.launch {
            for ((port, supported) in listOf("serial" to device.serialPort, "usb" to device.usb)) {
                if (!supported) continue
                val reply = withContext(Dispatchers.IO) { l.request("$port-state", timeoutSeconds = 5) }
                if (reply.ok) acceptSyncRoute(port, reply.message)
            }
        }
    }

    // The machine's serial setting, applied to the core and any external link.
    fun changeSerial(choice: String) {
        val attachedSync = serialSync
        device("Changing the serial port", "Couldn't change the serial port", false) { l ->
            sessionId?.let { id -> Sessions.updateSettings(activity.applicationContext, id) { it.copy(serial = choice) } }
            activity.runOnUiThread { settings = settings.copy(serial = choice) }
            if (InfraredChoice.linkId(choice) != null) FrondActivity.running?.askLocalNetwork()
            SerialLink.apply(activity.applicationContext, l, choice, name, device.label, attachedSync = attachedSync)
        }
    }

    fun changeUsbSync() = device("Changing USB HotSync", "Couldn't change USB HotSync", false) { l ->
        val reply = l.request("usb-mode", "sync", timeoutSeconds = 30)
        if (reply.ok) UsbLink.stop()
        if (reply.ok) null else reply.message
    }

    fun changeUsb(mode: BeamMode) = device("Changing USB HotSync", "Couldn't change USB HotSync", false) { l ->
        UsbLink.set(activity.applicationContext, l, mode)
    }

    // Windows CE: the USB cable in or out (usb-mode on or off), with the host network behind its RNDIS link.
    var usbCable by mutableStateOf(false)
        private set

    // Reads the cable back from the core's usb-state ("on ..." or "off ..."), so the row matches a session that
    // was saved or started with it in.
    fun loadUsbCable() {
        val l = link ?: return
        if (!device.usbNetwork) return
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { runCatching { l.request("usb-state", timeoutSeconds = 5) }.getOrNull() }
            if (reply != null && reply.ok) usbCable = reply.message.startsWith("on")
        }
    }

    fun changeUsbCable(on: Boolean) = device("Changing USB", "Couldn't change USB", false) { l ->
        val reply = l.request("usb-mode", if (on) "on" else "off", timeoutSeconds = 30)
        if (reply.ok) activity.runOnUiThread { usbCable = reply.message.startsWith("on") }
        if (reply.ok) null else reply.message
    }

    // Bluetooth on This Frond's radio or a connection, as BluetoothLink says, and what the core says about
    // the Palm's radio.
    var bluetooth by mutableStateOf(BluetoothLink.status.value)
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
            BluetoothLink.status.collect {
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

    // Off, radio or link:<id>: kept in the machine's settings, then applied to the core and the link.
    fun changeBluetooth(choice: String) = device("Changing Bluetooth", "Couldn't change Bluetooth", false) { l ->
        sessionId?.let { id -> Sessions.updateSettings(activity.applicationContext, id) { it.copy(bluetooth = choice) } }
        activity.runOnUiThread { settings = settings.copy(bluetooth = choice) }
        if (InfraredChoice.linkId(choice) != null) FrondActivity.running?.askLocalNetwork()
        BluetoothLink.apply(activity.applicationContext, l, choice, name, device.label).also { activity.runOnUiThread { readRadio() } }
    }

    // Both go back to the Palm, where the reset or rotation shows.
    fun reset() = device("Resetting", "Couldn't reset", true) { send(it, "reset") }

    fun rotate() = device("Rotating", "Couldn't rotate", true) { send(it, "rotate") }

    var cradleAttached by mutableStateOf<Boolean?>(null)
        private set

    private fun cradleSetting(reply: ControlLink.Reply): Boolean? = if (reply.ok) when (reply.message) {
        "attached" -> true
        "detached" -> false
        else -> null
    } else null

    fun readCradle() {
        val l = link ?: return
        if (!device.zodiac || waitingForGdb) return
        cradleAttached = null
        activity.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { l.request("cradle-setting", timeoutSeconds = 5) }
            cradleAttached = cradleSetting(reply)
        }
    }

    fun changeCradle(attached: Boolean) {
        if (!device.zodiac || cradleAttached == null) return
        device("Changing cradle", "Couldn't change the cradle", false) { l ->
            val changed = l.request("cradle", if (attached) "attached" else "detached", timeoutSeconds = 30)
            val current = l.request("cradle-setting", timeoutSeconds = 5)
            activity.runOnUiThread { cradleAttached = cradleSetting(current) }
            if (!changed.ok) changed.message else if (!current.ok) current.message else null
        }
    }

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
