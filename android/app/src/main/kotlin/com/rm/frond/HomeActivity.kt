package com.rm.frond

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.fillMaxSize
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import com.rm.frond.ui.AboutScreen
import com.rm.frond.ui.CardsScreen
import com.rm.frond.ui.ControlsEditor
import com.rm.frond.ui.ConnectionsScreen
import com.rm.frond.ui.FirmwareScreen
import com.rm.frond.ui.FrondTheme
import com.rm.frond.ui.HomeScreen
import com.rm.frond.ui.SettingsScreen
import com.rm.frond.ui.SyncManagerScreen
import com.rm.frond.ui.MachineSettingsScreen
import java.io.File

// `cards`: Add device's card per slot of a two-slot machine, a drawer card's id or "" for none.
data class MachineDraft(val device: Device, val settings: MachineSettings = MachineSettings(), val sessionId: String? = null,
                        val cards: List<String> = listOf("", ""), val zaurus: ZaurusImages.Picks = ZaurusImages.Picks())

// Import, export and duplicate run here, so a recreated activity doesn't cancel them or leave the
// Copying dialog up. The state is shared with whichever HomeModel is current.
internal object HomeWork {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    // Add device: the cards picked per slot for a new session, by its id, until it launches.
    val startCards = HashMap<String, List<String>>()
    var copying by mutableStateOf(false)
    var cancellable by mutableStateOf(false)
    // What the dialog says, and how far a card job has got, 0 to 1, when known.
    var label by mutableStateOf("Copying")
    var progress by mutableStateOf<Float?>(null)
    var job: Job? = null
    var model: HomeModel? = null
    val starts = LatestStart(scope)
}

// What the home screen shows. Lives in the activity and reloads from storage on resume.
class HomeModel(private val activity: HomeActivity) {
    var sessions by mutableStateOf(emptyList<Session>())
        private set
    var firmwareReady by mutableStateOf(emptyMap<Device, Boolean>())
        private set
    // A message for a dialog, with its title.
    var alert by mutableStateOf<Pair<String, String>?>(null)
    var firmwareOpen by mutableStateOf(false)
        private set
    // The device tapped in Add device, preselected on the firmware screen and started after a good import.
    var startAfterImport by mutableStateOf<Device?>(null)
        private set
    var machineDraft by mutableStateOf<MachineDraft?>(null)
    var copying: Boolean
        get() = HomeWork.copying
        set(value) { HomeWork.copying = value }
    val cancellable: Boolean get() = HomeWork.cancellable
    val copyLabel: String get() = HomeWork.label
    val copyProgress: Float? get() = HomeWork.progress
    // The Cards screen, when open.
    var cards by mutableStateOf<CardsModel?>(null)
        private set
    // The Connections screen, when open; it watches the link engine while Home is in front.
    var connections by mutableStateOf<ConnectionsModel?>(null)
        private set
    // A short message for the home screen's snackbar.
    var notice by mutableStateOf<String?>(null)
    var running by mutableStateOf(false)
        private set
    var syncManagerOpen by mutableStateOf(false)
    var settingsOpen by mutableStateOf(false)
    var aboutOpen by mutableStateOf(false)
    var settings by mutableStateOf(Settings())
        private set

    // The device each shown device's controller map is for, once its firmware is resolved.
    var controllerDevices by mutableStateOf(emptyMap<Device, Device>())
        private set
    // The editor for the device whose controls Settings is showing, if any.
    var controls by mutableStateOf<ControlsEditor?>(null)
        private set

    fun openControls(device: Device) {
        controls = ControlsEditor(activity, device, controllerDevices[device] ?: device)
    }

    fun closeControls() {
        controls?.capturing = null
        controls = null
    }

    // Devices with at least one session, in Device.shown order.
    val added get() = Device.shown.filter { d -> sessions.any { it.device == d } }

    fun update(changed: Settings) {
        settings = changed
        Settings.save(activity, changed)
    }

    // Runs `work` off the main thread while the Copying dialog shows. `work` gets a cancel check and
    // returns an error message or null; a cancel shows nothing.
    fun runCopy(errorTitle: String, success: String?, cancellable: Boolean = false, reload: Boolean = true,
                work: (cancelled: () -> Boolean) -> String?) =
        runJob("Copying", errorTitle, success, cancellable, reload) { cancelled, _ -> work(cancelled) }

    // As runCopy, with `label` in the dialog and a progress bar fed by `work`'s progress (bytes).
    fun runJob(label: String, errorTitle: String, success: String?, cancellable: Boolean = false, reload: Boolean = true,
               onSuccess: ((HomeModel) -> Unit)? = null,
               work: (cancelled: () -> Boolean, progress: (Long, Long) -> Unit) -> String?) {
        if (HomeWork.copying) return
        HomeWork.copying = true
        HomeWork.cancellable = cancellable
        HomeWork.label = label
        HomeWork.progress = null
        HomeWork.job = HomeWork.scope.launch {
            var error: String? = null
            var finished = false
            try {
                error = withContext(Dispatchers.IO) {
                    try {
                        work({ !isActive }) { done, total ->
                            if (total > 0) HomeWork.scope.launch { if (HomeWork.copying) HomeWork.progress = done.toFloat() / total }
                        }
                    } catch (e: kotlinx.coroutines.CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        // A damaged card or a bug shows as a message, not a crash.
                        e.message ?: "Something went wrong."
                    }
                }
                finished = true
            } finally {
                HomeWork.copying = false
                HomeWork.cancellable = false
                HomeWork.progress = null
                HomeWork.job = null
                val current = HomeWork.model
                if (reload) current?.reload()
                if (finished && error == COPY_CANCELLED) Unit
                else if (finished && error != null) current?.alert = errorTitle to error
                else if (finished && current != null) {
                    current.notice = success
                    onSuccess?.invoke(current)
                }
            }
        }
    }

    fun cancelCopy() {
        if (HomeWork.cancellable) HomeWork.job?.cancel()
    }

    fun reload() {
        Settings.migrateFastAudio(activity)
        Firmware.recover(activity)
        // A recovery session left by an SD exchange that never finished, once the emulator is gone.
        if (!emulatorRunning()) Sessions.adoptRecovery(activity)?.let {
            alert = "Exchange didn't finish" to "The session from before the SD card exchange is here as \"$it\"."
        }
        refreshRunning()
        sessions = Sessions.list(activity)
        firmwareReady = Device.shown.associateWith { Firmware.ready(activity, it) }
        settings = Settings.load(activity)
        controls?.reload()
        controllerDevices = Device.shown.associateWith { Firmware.runningDevice(it, null, Firmware.nor(activity, it)) }
        cards?.reload()
    }

    fun openCards() {
        if (cards == null) cards = CardsModel(activity, this).also { it.open() }
    }

    fun openConnections() {
        if (connections == null) connections = ConnectionsModel(activity).also { it.open() }
    }

    fun closeConnections() {
        connections?.close()
        connections = null
    }

    fun closeCards() {
        CardImageFiles.current?.takeIf { it.isOpen }?.close()
        cards = null
    }

    // Starts a session. With an emulator process still listed it brings a shown emulator forward,
    // or waits for a closing one and ends it if it stays, then starts. The wait lives in HomeWork,
    // so a recreated Home keeps it; the latest tap wins, and leaving Home drops it.
    fun start(requested: Session) {
        if (copying) return
        HomeWork.starts.start(requested.id) {
            val asked = System.currentTimeMillis()
            var ended = false
            var toldModel: HomeModel? = null
            while (isActive) {
                val m = HomeWork.model
                if (m == null) {
                    delay(EmulatorStart.POLL_MS)
                    continue
                }
                val waited = System.currentTimeMillis() - asked
                when (EmulatorStart.decide(m.emulatorRunning(), m.bringEmulatorForward(dryRun = true), waited, ended)) {
                    EmulatorStart.Action.START -> { m.launch(requested); return@start }
                    EmulatorStart.Action.BRING_FORWARD -> { m.bringEmulatorForward(dryRun = false); return@start }
                    EmulatorStart.Action.WAIT -> if (toldModel !== m) {
                        toldModel = m
                        m.notice = "Waiting for the last session to close"
                    }
                    EmulatorStart.Action.END_STALE -> {
                        ended = true
                        m.endEmulatorProcesses()
                    }
                    EmulatorStart.Action.GIVE_UP -> {
                        m.alert = "Couldn't start" to "The last session's emulator is still running. Force stop Frond in Android's app settings and try again."
                        return@start
                    }
                }
                delay(EmulatorStart.POLL_MS)
            }
        }
    }

    // Home has gone to the background for good: nothing is started from there.
    fun cancelStart() {
        HomeWork.starts.cancel()
        if (notice == "Waiting for the last session to close") notice = null
    }

    // Brings a task with an emulator activity on top to the front. With dryRun it only says
    // whether there is one.
    internal fun bringEmulatorForward(dryRun: Boolean): Boolean {
        val manager = activity.getSystemService(android.app.ActivityManager::class.java) ?: return false
        val names = setOf(FrondActivity::class.java.name, Frond68kActivity::class.java.name)
        val task = manager.appTasks.orEmpty().firstOrNull { task ->
            val top = try { task.taskInfo?.topActivity } catch (_: IllegalArgumentException) { null }
            top != null && top.className in names
        } ?: return false
        if (!dryRun) task.moveToFront()
        return true
    }

    // Ends emulator processes left with no activity. They run under this app's uid, so this is allowed.
    internal fun endEmulatorProcesses() {
        val manager = activity.getSystemService(android.app.ActivityManager::class.java) ?: return
        val names = Emulators.processes.map { activity.packageName + it }
        manager.runningAppProcesses.orEmpty().filter { it.processName in names }.forEach {
            android.util.Log.w(FrondActivity.TAG, "ending ${it.processName} (${it.pid}): no activity after ${EmulatorStart.GRACE_MS} ms")
            android.os.Process.killProcess(it.pid)
        }
    }

    internal fun launch(requested: Session) {
        val session = Sessions.read(activity, requested.id) ?: return
        val device = session.device
        if (session.old || device == null) {
            alert = "Saved by an older Frond" to "This session was saved by an older Frond and can't be loaded."
            return
        }
        if (device.zaurus) {
            if (!File(session.folder, ZaurusImages.CF).isFile ||
                !session.saved && !File(session.folder, ZaurusImages.NAND).isFile) {
                alert = "Missing Zaurus image" to "The machine's NAND backup or CF image is missing."
                return
            }
        } else if (!session.saved && firmwareReady[device] != true) {
            alert = "No firmware" to "Import the ${device.label} firmware first."
            return
        }
        Sessions.markUsed(activity, session)
        activity.startActivity(
            session.settings.applyTo(settings).putInto(Intent(activity, Emulators.activity(device)))
                .putExtra(FrondActivity.EXTRA_SESSION, session.file.path)
                .putExtra(FrondActivity.EXTRA_DEVICE, device.id)
                .apply {
                    if (!device.zaurus) {
                        putExtra(FrondActivity.EXTRA_NOR, Firmware.nor(activity, device).path)
                        putExtra(FrondActivity.EXTRA_NAND, Firmware.nand(activity, device).path)
                    }
                    HomeWork.startCards.remove(session.id)?.let { putExtra(FrondActivity.EXTRA_START_CARDS, it.toTypedArray()) }
                },
        )
    }

    internal fun emulatorRunning(): Boolean {
        val manager = activity.getSystemService(android.app.ActivityManager::class.java) ?: return true
        val names = Emulators.processes.map { activity.packageName + it }
        return manager.runningAppProcesses.orEmpty().any { it.processName in names }
    }

    fun refreshRunning() {
        running = emulatorRunning()
    }

    fun addMachine() {
        if (!copying && !emulatorRunning()) Device.shown.first().let { machineDraft = MachineDraft(it, MachineSettings.defaults(it)) }
    }

    fun cancelMachine() {
        if (copying && machineDraft?.device?.zaurus == true) return
        val picked = machineDraft?.settings?.soundFont
        machineDraft = null
        if (!picked.isNullOrEmpty()) pruneSoundFonts()
    }

    fun chooseMachine(device: Device) {
        machineDraft?.takeIf { it.sessionId == null }?.let {
            machineDraft = it.copy(device = device, settings = it.settings.forDevice(it.device, device))
        }
    }

    fun chooseZaurusImages(change: (ZaurusImages.Picks) -> ZaurusImages.Picks) {
        val draft = machineDraft ?: return
        if (draft.device.zaurus && draft.sessionId == null)
            machineDraft = draft.copy(zaurus = change(draft.zaurus))
    }

    private fun addZaurus(draft: MachineDraft) {
        if (!draft.zaurus.ready) return
        val app = activity.applicationContext
        lateinit var session: Session
        val label = if (draft.zaurus.create) "Creating the CF image" else "Copying Zaurus images"
        runJob(label, "Couldn't add the Zaurus", null, cancellable = true, onSuccess = { current ->
            current.machineDraft = null
            current.start(session)
        }) { cancelled, progress ->
            session = Sessions.createZaurus(app, draft.device, draft.settings, draft.zaurus, cancelled, progress)
            null
        }
    }

    fun changeMachineSettings(settings: MachineSettings) {
        machineDraft = machineDraft?.copy(settings = settings)
    }

    // Copies a picked SoundFont into the app's files and puts it in the machine being edited.
    fun importSoundFont(uri: android.net.Uri) {
        val app = activity.applicationContext
        runJob("Copying", "Couldn't use the SoundFont", null, cancellable = true, reload = false) { cancelled, progress ->
            val path = SoundFonts.import(app, uri, cancelled, progress)
            HomeWork.scope.launch {
                val m = HomeWork.model
                // A copy that finished after the copy or the draft was cancelled goes, unless a machine uses it.
                if (m == null || m.machineDraft == null || cancelled()) {
                    val keep = Sessions.list(app).map { it.settings.soundFont }.filter { it.isNotEmpty() }.toSet()
                    if (path !in keep && m?.machineDraft?.settings?.soundFont != path) File(path).delete()
                } else {
                    m.machineDraft = m.machineDraft?.let { d -> d.copy(settings = d.settings.copy(soundFont = path)) }
                }
            }
            null
        }
    }

    // Removes the SoundFont copies no machine uses any more.
    private fun pruneSoundFonts() {
        val keep = Sessions.list(activity).map { it.settings.soundFont }.filter { it.isNotEmpty() }.toSet()
        try { SoundFonts.prune(activity, keep) } catch (e: Exception) { }
    }

    // Add device's card for slot `n` ("" for none).
    fun chooseStartCard(n: Int, id: String) {
        machineDraft = machineDraft?.let { d -> d.copy(cards = d.cards.toMutableList().also { it[n] = id }) }
    }

    fun edit(session: Session) {
        if (copying || emulatorRunning()) return
        val current = Sessions.read(activity, session.id) ?: return
        if (current.old || current.device == null) return
        machineDraft = MachineDraft(current.device, current.settings, current.id)
    }

    fun saveMachine() {
        val draft = machineDraft ?: return
        if (copying || emulatorRunning()) {
            alert = "Machine is busy" to "Stop the machine before editing its settings."
            return
        }
        try {
            val id = draft.sessionId
            if (id != null) {
                val current = Sessions.read(activity, id)
                if (current == null || current.old || current.device != draft.device) {
                    machineDraft = null
                    reload()
                    return
                }
                Sessions.updateSettings(activity, id) { draft.settings }
                machineDraft = null
                reload()
                pruneSoundFonts()
            } else if (draft.device.zaurus) {
                addZaurus(draft)
            } else if (firmwareReady[draft.device] != true) {
                openFirmware(draft.device)
            } else {
                val cards = if (Cards.hasSlot(draft.device, 1)) draft.cards else emptyList()
                if (cards.size == 2 && cards[0].isNotEmpty() && cards[0] == cards[1]) {
                    alert = "Cards" to "Pick a different card for each slot."
                    return
                }
                val session = Sessions.create(activity, draft.device, settings = draft.settings)
                DevicePicker.remember(activity, draft.device)
                if (cards.any { it.isNotEmpty() }) HomeWork.startCards[session.id] = cards
                machineDraft = null
                reload()
                pruneSoundFonts()
                start(session)
            }
        } catch (e: Exception) {
            alert = "Couldn't save settings" to "Couldn't write the machine settings."
        }
    }

    fun firmwareImported(device: Device) {
        val adding = startAfterImport != null
        closeFirmware()
        if (adding) machineDraft = machineDraft?.let { it.copy(device = device, settings = it.settings.forDevice(it.device, device)) }
    }

    fun restore(state: Bundle?) {
        if (state == null) return
        state.getString("machineDraft")?.let {
            val json = org.json.JSONObject(it)
            val device = Device.byId(json.optString("device")) ?: return@let
            machineDraft = MachineDraft(device, MachineSettings.from(json.optJSONObject("settings")),
                json.optString("sessionId").takeIf(String::isNotEmpty), zaurus = ZaurusImages.Picks.from(json))
        }
        firmwareOpen = state.getBoolean("firmwareOpen")
        startAfterImport = Device.byId(state.getString("startAfterImport"))
        syncManagerOpen = state.getBoolean("syncManagerOpen")
    }

    fun saveState(state: Bundle) {
        machineDraft?.let {
            state.putString("machineDraft", it.zaurus.putInto(org.json.JSONObject().put("device", it.device.id)
                .put("settings", it.settings.json()).put("sessionId", it.sessionId)).toString())
        }
        state.putBoolean("firmwareOpen", firmwareOpen)
        state.putString("startAfterImport", startAfterImport?.id)
        state.putBoolean("syncManagerOpen", syncManagerOpen)
    }

    fun openFirmware(thenStart: Device? = null) {
        startAfterImport = thenStart
        firmwareOpen = true
    }

    fun closeFirmware() {
        firmwareOpen = false
        startAfterImport = null
    }
}

class HomeActivity : ComponentActivity() {
    private val model = HomeModel(this)
    private var processWatch: Job? = null

    override fun onDestroy() {
        if (HomeWork.model === model) HomeWork.model = null
        // A card open in Files stays open across a recreate and closes when the screen really goes.
        if (isFinishing && !isChangingConfigurations) CardImageFiles.current?.close()
        super.onDestroy()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        HomeWork.model = model
        model.restore(savedInstanceState)
        // A card still open in Files from before a recreate: its view comes back.
        if (CardImageFiles.current?.isOpen == true) model.openCards()
        // Cards: stale copies go and existing machines' cards move in. Machines running elsewhere wait.
        if (savedInstanceState == null) {
            val app = applicationContext
            Thread({ Cards.startUp(Sessions.folder(app)) }, "FrondCards").start()
        }
        enableEdgeToEdge()
        setContent {
            FrondTheme {
                Surface(Modifier.fillMaxSize()) {
                    BackHandler(enabled = model.firmwareOpen) { model.closeFirmware() }
                    BackHandler(enabled = model.cards != null && !model.firmwareOpen && !model.settingsOpen) { model.closeCards() }
                    BackHandler(enabled = model.connections != null && model.cards == null && !model.firmwareOpen && !model.settingsOpen) { model.closeConnections() }
                    BackHandler(enabled = model.machineDraft != null && !model.firmwareOpen) { model.cancelMachine() }
                    BackHandler(enabled = model.settingsOpen) {
                        when {
                            model.controls?.capturing != null -> model.controls?.capturing = null
                            model.controls != null -> model.closeControls()
                            else -> model.settingsOpen = false
                        }
                    }
                    // The last one registered wins, so About closes first.
                    BackHandler(enabled = model.syncManagerOpen) { model.syncManagerOpen = false }
                    BackHandler(enabled = model.aboutOpen) { model.aboutOpen = false }
                    when {
                        model.aboutOpen -> AboutScreen(model)
                        model.syncManagerOpen -> SyncManagerScreen(model.sessions) { model.syncManagerOpen = false }
                        model.settingsOpen -> SettingsScreen(model)
                        model.firmwareOpen -> FirmwareScreen(model)
                        model.machineDraft != null -> MachineSettingsScreen(model)
                        model.cards != null -> CardsScreen(model, model.cards!!)
                        model.connections != null -> ConnectionsScreen(model, model.connections!!)
                        else -> HomeScreen(model)
                    }
                }
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        model.saveState(outState)
        super.onSaveInstanceState(outState)
    }

    // The key just assigned, whose release shouldn't reach the screen.
    private var assignedKey = 0

    // While Settings waits for a controller key, the next one pressed is assigned.
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (event.action == android.view.KeyEvent.ACTION_UP && event.keyCode == assignedKey) {
            assignedKey = 0
            return true
        }
        if (model.controls?.capturing != null && model.settingsOpen && ControllerMap.fromController(event)) {
            if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                model.controls?.assign(event.keyCode)
                assignedKey = event.keyCode
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    // Stick directions while Settings waits for a key: one pushed past half travel from nearer the
    // middle is assigned, so a stick already held over doesn't take the next button too.
    private val captureSticks = StickKeys()
    private val captureRight = RightStickAxes()
    private val captureHat = HatKeys()
    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        if (event.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK) && event.action == android.view.MotionEvent.ACTION_MOVE) {
            val capture = model.controls?.capturing != null && model.settingsOpen
            val assign = { key: Int, down: Boolean -> if (down && capture) model.controls?.assign(key) }
            captureSticks.update(event.deviceId, ControllerMap.STICK_LEFT,
                event.getAxisValue(android.view.MotionEvent.AXIS_X), event.getAxisValue(android.view.MotionEvent.AXIS_Y), assign)
            captureSticks.update(event.deviceId, ControllerMap.STICK_RIGHT,
                captureRight.value(event.deviceId, 0, event.getAxisValue(android.view.MotionEvent.AXIS_Z), RightStickAxes.centred(event, android.view.MotionEvent.AXIS_Z)),
                captureRight.value(event.deviceId, 1, event.getAxisValue(android.view.MotionEvent.AXIS_RZ), RightStickAxes.centred(event, android.view.MotionEvent.AXIS_RZ)), assign)
            captureHat.update(event.deviceId, event.getAxisValue(android.view.MotionEvent.AXIS_HAT_X),
                event.getAxisValue(android.view.MotionEvent.AXIS_HAT_Y), assign)
            if (capture) return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onResume() {
        super.onResume()
        model.reload()
        processWatch?.cancel()
        // Home can resume before the emulator process has finished exiting.
        processWatch = lifecycleScope.launch {
            while (model.running) {
                delay(250)
                model.refreshRunning()
            }
            // FrondActivity leaves a note when the core stopped without being asked to. It writes it
            // as its process ends, which can be after this resume.
            val note = File(filesDir, FrondActivity.START_ERROR)
            if (note.isFile) {
                model.alert = "Emulator stopped" to note.readText()
                note.delete()
            }
        }
    }

    override fun onStart() {
        super.onStart()
        model.connections?.open()
    }

    override fun onStop() {
        if (!isChangingConfigurations) model.cancelStart()
        model.connections?.close()
        super.onStop()
    }

    override fun onPause() {
        processWatch?.cancel()
        processWatch = null
        super.onPause()
    }

}
