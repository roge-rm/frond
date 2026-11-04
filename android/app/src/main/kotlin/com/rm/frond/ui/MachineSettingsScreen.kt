package com.rm.frond.ui

import android.content.pm.ApplicationInfo
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
import com.rm.frond.HomeModel
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
            audioPreviewQualified(context, draft.device, session)
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
            MachineDisplayRows(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            Header("Audio", Modifier.padding(top = 16.dp))
            MachineSoundRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            MachineAudioPreviewRow(draft.device, draft.settings, qualified, false) { model.changeMachineSettings(it) }
            if (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
                Header("Speed", Modifier.padding(top = 16.dp))
                MachineEngineRow(draft.settings, false) { model.changeMachineSettings(it) }
            }
        }
    }
}
