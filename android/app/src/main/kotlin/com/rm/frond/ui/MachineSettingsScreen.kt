package com.rm.frond.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.text.style.TextOverflow
import com.rm.frond.Device
import com.rm.frond.HomeModel
import com.rm.frond.HostFiles
import com.rm.frond.BluetoothChoice
import com.rm.frond.InfraredChoice
import com.rm.frond.SyncProtocol
import com.rm.frond.supportsNetworkHotSync
import com.rm.frond.LinkStore
import com.rm.frond.MachineRows

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MachineSettingsScreen(model: HomeModel) {
    val draft = model.machineDraft ?: return
    val context = LocalContext.current
    val editing = draft.sessionId != null
    var syncOpen by rememberSaveable { mutableStateOf(false) }
    val syncId = draft.sessionId?.takeIf { SyncProtocol.forDevice(draft.device) != null }
    if (syncOpen && syncId != null) SyncDeviceDialog(syncId, onClose = { syncOpen = false },
        protocol = SyncProtocol.forDevice(draft.device), networkHotSync = draft.device.supportsNetworkHotSync())
    val zSetup = !editing && draft.device.zaurus
    val ready = if (zSetup) draft.zaurus.ready else model.firmwareReady[draft.device] == true
    val pickNand = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.chooseZaurusImages { it.copy(nand = uri) }
    }
    val pickCf = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.chooseZaurusImages { it.copy(cf = uri) }
    }
    val pickProm = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.chooseZaurusImages { it.copy(prom = uri) }
    }
    if (zSetup) CopyingDialog(model)
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
                    TextButton(onClick = { model.saveMachine() },
                        enabled = !model.copying && !model.running && (!zSetup || ready)) {
                        Text(if (editing) "Save" else if (ready || zSetup) "Add" else "Import firmware")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            if (!editing) DeviceChoice("Device", draft.device) { model.chooseMachine(it) }
            if (zSetup) {
                Header("Zaurus images")
                ZaurusImagePicker("NAND backup", draft.zaurus.nand) { pickNand.launch(arrayOf("*/*")) }
                ZaurusImagePicker("Existing CF image", draft.zaurus.cf) { pickCf.launch(arrayOf("*/*")) }
                ZaurusImagePicker("CS0 boot PROM (optional)", draft.zaurus.prom) { pickProm.launch(arrayOf("*/*")) }
                if (draft.zaurus.prom != null)
                    TextButton(onClick = { model.chooseZaurusImages { it.copy(prom = null) } }) { Text("Remove boot PROM") }
            }
            // A card for each slot of a two-slot machine.
            if (!editing && com.rm.frond.Cards.hasSlot(draft.device, 1)) {
                Header("Cards")
                for (n in 0 until 2) {
                    val choices = remember(draft.device, n) {
                        com.rm.frond.CardChoices.startCardChoices(com.rm.frond.Sessions.folder(context), draft.device, n)
                    }
                    val value = draft.cards[n].takeIf { id -> choices.any { it.first == id } } ?: ""
                    Choice("Card ${n + 1}", value, choices.map { it.first }, { v -> choices.first { it.first == v }.second }) {
                        model.chooseStartCard(n, it)
                    }
                }
            }
            Header("Display", if (!editing && com.rm.frond.Cards.hasSlot(draft.device, 1)) Modifier.padding(top = 16.dp) else Modifier)
            MachineDisplayRows(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            val rows = MachineRows.of(draft.device)
            Header("Audio", Modifier.padding(top = 16.dp))
            MachineSoundRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            if (draft.device.headphonesSupported) MachineHeadphonesRow(draft.settings, false) { model.changeMachineSettings(it) }
            if (rows.audioPreview) {
                MachineAudioPreviewRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            }
            if (draft.device.vibrate) MachineRumbleRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            if (draft.device.voiceKey) MachineRecordKeyRow(draft.settings, false) { model.changeMachineSettings(it) }
            if (draft.device.zodiac) MachineMusicRows(draft.settings, { model.changeMachineSettings(it) }) { model.importSoundFont(it) }
            Header("Battery", Modifier.padding(top = 16.dp))
            MachineBatteryRows(draft.settings, false, null) { model.changeMachineSettings(it) }
            if (rows.speed || rows.engine) Header("Speed", Modifier.padding(top = 16.dp))
            if (rows.speed) MachineSpeedRow(draft.device, draft.settings, false) { model.changeMachineSettings(it) }
            if (rows.engine) MachineEngineRow(draft.settings, false) { model.changeMachineSettings(it) }
            if (rows.engine && draft.device.zodiac) MachineDirectStickRow(draft.settings, false) { model.changeMachineSettings(it) }
            if (draft.device.infrared || draft.device.serialPort || draft.device.bluetooth) Header("Connections", Modifier.padding(top = 16.dp))
            if (draft.device.infrared) {
                val choices = remember(draft.device, draft.settings.infrared, draft.settings.irSync) {
                    InfraredChoice.infraredChoices(draft.settings.infrared, LinkStore.choices(InfraredChoice.folder(context)), draft.device.irPhone,
                        sync = InfraredChoice.offerIrSync(draft.device, draft.settings))
                }
                val current = draft.settings.infrared.takeIf { value -> choices.any { it.first == value } } ?: InfraredChoice.OFF
                Choice("Infrared", current, choices.map { it.first }, { v -> choices.first { it.first == v }.second }) {
                    model.changeMachineSettings(draft.settings.copy(infrared = it))
                }
            }
            if (draft.device.serialPort) {
                val choices = remember(draft.device, draft.settings.serial) {
                    InfraredChoice.serialChoices(draft.settings.serial, LinkStore.choices(InfraredChoice.folder(context)),
                        draft.device.modem, SyncProtocol.cradleHotSync(draft.device), ppp = draft.device.runs68k,
                        keyboard = draft.device.serialKeyboard)
                }
                val current = draft.settings.serial.takeIf { value -> choices.any { it.first == value } } ?: InfraredChoice.OFF
                Choice("Serial port", current, choices.map { it.first }, { v -> choices.first { it.first == v }.second }) {
                    model.changeMachineSettings(draft.settings.copy(serial = it))
                }
            }
            if (syncId != null) TextButton(onClick = { syncOpen = true }) { Text("Sync settings") }
            if (draft.device.bluetooth) {
                val choices = remember(draft.device, draft.settings.bluetooth) {
                    BluetoothChoice.choices(draft.settings.bluetooth, LinkStore.choices(InfraredChoice.folder(context)))
                }
                Choice("Bluetooth", draft.settings.bluetooth, choices.map { it.first }, { v -> choices.first { it.first == v }.second }) {
                    model.changeMachineSettings(draft.settings.copy(bluetooth = it))
                }
            }
        }
    }
}

@Composable
private fun ZaurusImagePicker(label: String, uri: android.net.Uri?, pick: () -> Unit) {
    val resolver = LocalContext.current.contentResolver
    val name = remember(resolver, uri) { uri?.let { HostFiles.displayName(resolver, it) } }
    TextButton(onClick = pick) {
        Text(if (name == null) label else "$label ($name)", maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}
