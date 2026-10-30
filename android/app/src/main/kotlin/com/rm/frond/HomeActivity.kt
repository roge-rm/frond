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
    var firmwareFor by mutableStateOf<Device?>(null)
    var settingsOpen by mutableStateOf(false)
    var settings by mutableStateOf(Settings())
        private set

    fun update(changed: Settings) {
        settings = changed
        Settings.save(activity, changed)
    }

    fun reload() {
        Firmware.recover(activity)
        sessions = Sessions.list(activity)
        firmwareReady = Device.shown.associateWith { Firmware.ready(activity, it) }
        settings = Settings.load(activity)
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
                    val device = model.firmwareFor
                    BackHandler(enabled = device != null) { model.firmwareFor = null }
                    BackHandler(enabled = model.settingsOpen) { model.settingsOpen = false }
                    when {
                        model.settingsOpen -> SettingsScreen(model)
                        device != null -> FirmwareScreen(model, device)
                        else -> HomeScreen(model)
                    }
                }
            }
        }
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
