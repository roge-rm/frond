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
import com.rm.frond.ui.FirmwareScreen
import com.rm.frond.ui.FrondTheme
import com.rm.frond.ui.HomeScreen
import com.rm.frond.ui.SettingsScreen
import java.io.File

// What the home screen shows. Lives in the activity and reloads from storage on resume.
class HomeModel(private val activity: HomeActivity) {
    var sessions by mutableStateOf(emptyList<Session>())
        private set
    var firmwareReady by mutableStateOf(emptyMap<Device, Boolean>())
        private set
    // A message for a dialog, with its title.
    var alert by mutableStateOf<Pair<String, String>?>(null)
    var firmwareOpen by mutableStateOf(false)
    var settingsOpen by mutableStateOf(false)
    var settings by mutableStateOf(Settings())
        private set

    // Controller mapping per device, and the Palm button waiting for a controller key, if any.
    var controllers by mutableStateOf(emptyMap<Device, ControllerMap>())
        private set
    var capturing by mutableStateOf<Pair<Device, PalmButton>?>(null)

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
        controllers = controllers + (device to ControllerMap.defaults(device))
    }

    private fun setController(device: Device, map: ControllerMap) {
        ControllerMap.save(activity, device, map)
        controllers = controllers + (device to map)
    }

    fun update(changed: Settings) {
        settings = changed
        Settings.save(activity, changed)
    }

    fun reload() {
        Firmware.recover(activity)
        // A recovery session left by an SD exchange that never finished, once the emulator is gone.
        if (!emulatorRunning()) Sessions.adoptRecovery(activity)?.let {
            alert = "Exchange didn't finish" to "The session from before the SD card exchange is here as \"$it\"."
        }
        sessions = Sessions.list(activity)
        firmwareReady = Device.shown.associateWith { Firmware.ready(activity, it) }
        settings = Settings.load(activity)
        controllers = Device.shown.associateWith { ControllerMap.load(activity, it) }
    }

    fun start(session: Session) {
        if (!session.saved && firmwareReady[session.device] != true) {
            alert = "No firmware" to "Import the ${session.device.label} firmware first."
            return
        }
        Sessions.markUsed(activity, session)
        activity.startActivity(
            settings.putInto(Intent(activity, FrondActivity::class.java))
                .putExtra(FrondActivity.EXTRA_SESSION, session.file.path)
                .putExtra(FrondActivity.EXTRA_DEVICE, session.device.id)
                .putExtra(FrondActivity.EXTRA_NOR, Firmware.nor(activity, session.device).path)
                .putExtra(FrondActivity.EXTRA_NAND, Firmware.nand(activity, session.device).path),
        )
    }

    private fun emulatorRunning(): Boolean {
        val manager = activity.getSystemService(android.app.ActivityManager::class.java) ?: return true
        return manager.runningAppProcesses.orEmpty().any { it.processName == activity.packageName + ":emulator" }
    }

    fun newSession(device: Device) = start(Sessions.create(activity, device))
}

class HomeActivity : ComponentActivity() {
    private val model = HomeModel(this)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            FrondTheme {
                Surface(Modifier.fillMaxSize()) {
                    BackHandler(enabled = model.firmwareOpen) { model.firmwareOpen = false }
                    BackHandler(enabled = model.settingsOpen) {
                        if (model.capturing != null) model.capturing = null else model.settingsOpen = false
                    }
                    when {
                        model.settingsOpen -> SettingsScreen(model)
                        model.firmwareOpen -> FirmwareScreen(model)
                        else -> HomeScreen(model)
                    }
                }
            }
        }
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
        // FrondActivity leaves a note when the core stopped without being asked to.
        val note = File(filesDir, FrondActivity.START_ERROR)
        if (note.isFile) {
            model.alert = "Emulator stopped" to note.readText()
            note.delete()
        }
    }
}
