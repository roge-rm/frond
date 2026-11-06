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
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.rm.frond.Device
import com.rm.frond.HomeModel
import com.rm.frond.MachineRows

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MachineSettingsScreen(model: HomeModel) {
    val draft = model.machineDraft ?: return
    val context = LocalContext.current
    val editing = draft.sessionId != null
    val ready = model.firmwareReady[draft.device] == true
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
            if (!editing) Choice("Device", draft.device, Device.shown.sortedBy { it.label.lowercase() }, { it.label }) {
                model.chooseMachine(it)
            }
            Header("Display")
            MachineDisplayRows(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            val rows = MachineRows.of(draft.device, context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0)
            Header("Audio", Modifier.padding(top = 16.dp))
            MachineSoundRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            if (rows.audioPreview) {
                MachineAudioPreviewRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            }
            if (rows.speed || rows.engine) Header("Speed", Modifier.padding(top = 16.dp))
            if (rows.speed) MachineSpeedRow(draft.settings, false) { model.changeMachineSettings(it) }
            if (rows.engine) MachineEngineRow(draft.settings, false) { model.changeMachineSettings(it) }
        }
    }
}
