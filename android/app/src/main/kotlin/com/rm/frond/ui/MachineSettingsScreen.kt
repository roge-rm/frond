package com.rm.frond.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.rm.frond.Device
import com.rm.frond.Firmware
import com.rm.frond.HomeModel
import com.rm.frond.ScreenSize
import com.rm.frond.Sessions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MachineSettingsScreen(model: HomeModel) {
    val draft = model.machineDraft ?: return
    val context = LocalContext.current
    val editing = draft.sessionId != null
    val ready = model.firmwareReady[draft.device] == true
    val qualified by produceState<Boolean?>(null, draft.device, draft.sessionId, ready) {
        value = null
        value = withContext(Dispatchers.IO) {
            val session = draft.sessionId?.let { Sessions.read(context, it)?.file }
            if (draft.device.id == "sony-clie-tg50")
                Firmware.sonyPreviewEligible(draft.device, session, Firmware.nor(context, draft.device))
            else Firmware.fastAudioQualified(draft.device, session, Firmware.nor(context, draft.device))
        }
    }
    model.alert?.let { (title, text) ->
        AlertDialog(onDismissRequest = { model.alert = null }, title = { Text(title) }, text = { Text(text) },
            confirmButton = { TextButton(onClick = { model.alert = null }) { Text("OK") } })
    }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(if (editing) "Edit settings" else "Add device") },
                navigationIcon = {
                    TextButton(onClick = { model.cancelMachine() }) { Text("Cancel") }
                },
                actions = {
                    TextButton(onClick = { model.saveMachine() }, enabled = !model.copying && !model.running) {
                        Text(if (editing) "Save" else if (ready) "Add" else "Import firmware")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            if (!editing) Choice("Device", draft.device, Device.shown, { it.label }) {
                model.chooseMachine(it)
            }
            Header("Display")
            Choice("Size", draft.settings.size, ScreenSize.entries, { it.label }) {
                model.changeMachineSettings(draft.settings.copy(size = it))
            }
            SwitchRow("Smooth pixels", draft.settings.smoothing) {
                model.changeMachineSettings(draft.settings.copy(smoothing = it))
            }
            if (draft.device.id == "sony-clie-tg50") {
                SwitchRow("Show keyboard", draft.settings.showKeyboard) {
                    model.changeMachineSettings(draft.settings.copy(showKeyboard = it))
                }
                SwitchRow("Show jog controls", draft.settings.showJogControls) {
                    model.changeMachineSettings(draft.settings.copy(showJogControls = it))
                }
            }
            Header("Audio", Modifier.padding(top = 16.dp))
            SwitchRow("Sound", draft.device.sound && draft.settings.sound, enabled = draft.device.sound) {
                model.changeMachineSettings(draft.settings.copy(sound = it))
            }
            if (draft.device.id == "sony-clie-tg50") {
                SwitchRow("Sony audio preview", qualified == true && draft.settings.sonyAudioPreview, enabled = qualified == true) {
                    model.changeMachineSettings(draft.settings.copy(sonyAudioPreview = it))
                }
            } else {
                SwitchRow("Low-latency audio", qualified == true && draft.settings.fastAudio, enabled = qualified == true) {
                    model.changeMachineSettings(draft.settings.copy(fastAudio = it))
                }
            }
        }
    }
}
