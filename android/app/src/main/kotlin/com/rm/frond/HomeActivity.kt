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
import com.rm.frond.ui.ConnectionsScreen
import com.rm.frond.ui.FirmwareScreen
import com.rm.frond.ui.FrondTheme
import com.rm.frond.ui.HomeScreen
import com.rm.frond.ui.SettingsScreen
import com.rm.frond.ui.MachineSettingsScreen
import java.io.File

data class MachineDraft(val device: Device, val settings: MachineSettings = MachineSettings(), val sessionId: String? = null)

// Import, export and duplicate run here, so a recreated activity doesn't cancel them or leave the
// Copying dialog up. The state is shared with whichever HomeModel is current.
internal object HomeWork {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    var copying by mutableStateOf(false)
    var cancellable by mutableStateOf(false)
    // What the dialog says, and how far a card job has got, 0 to 1, when known.
    var label by mutableStateOf("Copying")
    var progress by mutableStateOf<Float?>(null)
    var job: Job? = null
    var model: HomeModel? = null
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
    var settingsOpen by mutableStateOf(false)
    var aboutOpen by mutableStateOf(false)
    var settings by mutableStateOf(Settings())
        private set

    // Controller mapping per device, and the Palm button waiting for a controller key, if any.
    var controllers by mutableStateOf(emptyMap<Device, ControllerMap>())
        private set
    var controllerDevices by mutableStateOf(emptyMap<Device, Device>())
        private set
    var capturing by mutableStateOf<Pair<Device, PalmButton>?>(null)
    // The device whose controls Settings is showing, if any.
    var controlsFor by mutableStateOf<Device?>(null)

    // Devices with at least one session, in Device.shown order.
    val added get() = Device.shown.filter { d -> sessions.any { it.device == d } }

    fun assign(key: Int) {
        val (device, button) = capturing ?: return
        capturing = null
        setController(device, controllers.getValue(device).assign(button, key))
    }

    fun clearButton(device: Device, button: PalmButton) {
        capturing = null
        setController(device, controllers.getValue(device).clear(button))
    }

    fun resetController(device: Device) {
        capturing = null
        ControllerMap.reset(activity, device)
        controllers = controllers + (device to ControllerMap.defaults(controllerDevices[device] ?: device))
    }

    private fun setController(device: Device, map: ControllerMap) {
        ControllerMap.save(activity, device, map)
        controllers = controllers + (device to map)
    }

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
                else if (finished) current?.notice = success
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
        controllerDevices = Device.shown.associateWith { Firmware.runningDevice(it, null, Firmware.nor(activity, it)) }
        controllers = controllerDevices.mapValues { (_, device) -> ControllerMap.load(activity, device) }
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

    fun start(requested: Session) {
        if (copying || emulatorRunning()) return
        val session = Sessions.read(activity, requested.id) ?: return
        val device = session.device
        if (session.old || device == null) {
            alert = "Saved by an older Frond" to "This session was saved by an older Frond and can't be loaded."
            return
        }
        if (!session.saved && firmwareReady[device] != true) {
            alert = "No firmware" to "Import the ${device.label} firmware first."
            return
        }
        Sessions.markUsed(activity, session)
        activity.startActivity(
            session.settings.applyTo(settings).putInto(Intent(activity, Emulators.activity(device)))
                .putExtra(FrondActivity.EXTRA_SESSION, session.file.path)
                .putExtra(FrondActivity.EXTRA_DEVICE, device.id)
                .putExtra(FrondActivity.EXTRA_NOR, Firmware.nor(activity, device).path)
                .putExtra(FrondActivity.EXTRA_NAND, Firmware.nand(activity, device).path),
        )
    }

    private fun emulatorRunning(): Boolean {
        val manager = activity.getSystemService(android.app.ActivityManager::class.java) ?: return true
        val names = Emulators.processes.map { activity.packageName + it }
        return manager.runningAppProcesses.orEmpty().any { it.processName in names }
    }

    fun refreshRunning() {
        running = emulatorRunning()
    }

    fun addMachine() {
        if (!copying && !emulatorRunning()) machineDraft = MachineDraft(Device.shown.first())
    }

    fun cancelMachine() {
        machineDraft = null
    }

    fun chooseMachine(device: Device) {
        machineDraft?.takeIf { it.sessionId == null }?.let { machineDraft = it.copy(device = device) }
    }

    fun changeMachineSettings(settings: MachineSettings) {
        machineDraft = machineDraft?.copy(settings = settings)
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
            } else if (firmwareReady[draft.device] != true) {
                openFirmware(draft.device)
            } else {
                val session = Sessions.create(activity, draft.device, settings = draft.settings)
                machineDraft = null
                reload()
                start(session)
            }
        } catch (e: Exception) {
            alert = "Couldn't save settings" to "Couldn't write the machine settings."
        }
    }

    fun firmwareImported(device: Device) {
        val adding = startAfterImport != null
        closeFirmware()
        if (adding) machineDraft = machineDraft?.copy(device = device)
    }

    fun restore(state: Bundle?) {
        if (state == null) return
        state.getString("machineDraft")?.let {
            val json = org.json.JSONObject(it)
            val device = Device.byId(json.optString("device")) ?: return@let
            machineDraft = MachineDraft(device, MachineSettings.from(json.optJSONObject("settings")),
                json.optString("sessionId").takeIf(String::isNotEmpty))
        }
        firmwareOpen = state.getBoolean("firmwareOpen")
        startAfterImport = Device.byId(state.getString("startAfterImport"))
    }

    fun saveState(state: Bundle) {
        machineDraft?.let {
            state.putString("machineDraft", org.json.JSONObject().put("device", it.device.id)
                .put("settings", it.settings.json()).put("sessionId", it.sessionId).toString())
        }
        state.putBoolean("firmwareOpen", firmwareOpen)
        state.putString("startAfterImport", startAfterImport?.id)
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
                            model.capturing != null -> model.capturing = null
                            model.controlsFor != null -> model.controlsFor = null
                            else -> model.settingsOpen = false
                        }
                    }
                    // The last one registered wins, so About closes first.
                    BackHandler(enabled = model.aboutOpen) { model.aboutOpen = false }
                    when {
                        model.aboutOpen -> AboutScreen(model)
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
        if (model.capturing != null && model.settingsOpen && ControllerMap.fromController(event)) {
            if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                model.assign(event.keyCode)
                assignedKey = event.keyCode
            }
            return true
        }
        return super.dispatchKeyEvent(event)
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
        }
        // FrondActivity leaves a note when the core stopped without being asked to.
        val note = File(filesDir, FrondActivity.START_ERROR)
        if (note.isFile) {
            model.alert = "Emulator stopped" to note.readText()
            note.delete()
        }
    }

    override fun onStart() {
        super.onStart()
        model.connections?.open()
    }

    override fun onStop() {
        model.connections?.close()
        super.onStop()
    }

    override fun onPause() {
        processWatch?.cancel()
        processWatch = null
        super.onPause()
    }

}
