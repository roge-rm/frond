package com.rm.frond.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.NetRelay
import com.rm.frond.RunningModel
import com.rm.frond.R
import com.rm.frond.RunningBar
import com.rm.frond.RunningSheets
import com.rm.frond.RunningSheets.AppAction
import com.rm.frond.RunningSheets.AudioRow
import com.rm.frond.RunningSheets.CardRow
import com.rm.frond.RunningSheets.ConnRow
import com.rm.frond.RunningSheets.DisplayRow
import com.rm.frond.RunningSheets.Sheet
import com.rm.frond.SdCard
import com.rm.frond.SerialLink
import com.rm.frond.UsbLink

private val Faded @Composable get() = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)

// The dimmed scrim and a bottom sheet (side sheet in landscape) over the Palm. A tap on the scrim closes the sheet and goes no further.
@Composable
fun BoxScope.SheetLayer(sheet: Sheet, state: RunningBarState, actions: RunningBarActions) {
    val device = state.device ?: return
    val model = state.model ?: return
    androidx.compose.foundation.layout.Box(
        Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.45f)).pointerInput(Unit) { detectTapGestures { actions.closeSheet() } },
    )
    val side = RunningBar.sideSheet(state.portrait)
    BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = if (side) Alignment.CenterEnd else Alignment.BottomCenter) {
        val shape = if (side) RoundedCornerShape(topStart = 16.dp, bottomStart = 16.dp) else RoundedCornerShape(topStart = 16.dp, topEnd = 16.dp)
        val size = if (side) Modifier.width(RunningBar.sideSheetWidthDp(maxWidth.value.toInt()).dp).fillMaxHeight()
        else Modifier.fillMaxWidth().heightIn(max = maxHeight * 0.65f)
        Surface(size.pointerInput(Unit) { detectTapGestures { } }, shape = shape, color = MaterialTheme.colorScheme.surfaceContainer) {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(RunningSheets.title(sheet, device), style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp))
                when (sheet) {
                    Sheet.APPLICATIONS -> ApplicationsSheet(model, actions)
                    Sheet.CARD -> CardSheet(model, actions)
                    Sheet.DISPLAY -> DisplaySheet(model, state, actions)
                    Sheet.CONNECTIONS -> ConnectionsSheet(model, actions)
                }
            }
        }
    }
}

@Composable
private fun ApplicationsSheet(model: RunningModel, actions: RunningBarActions) {
    LaunchedEffect(Unit) { model.loadApps() }
    var exportName by rememberSaveable { mutableStateOf<String?>(null) }
    var deleting by rememberSaveable { mutableStateOf<String?>(null) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> model.install(uris) }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) model.export(uri, exportName)
    }
    Row(Modifier.padding(horizontal = 8.dp)) {
        TextButton(onClick = { pick.launch(arrayOf("*/*")) }) { Text("Install…") }
        TextButton(onClick = { exportName = null; save.launch("Palm databases.zip") }) { Text("Export all…") }
        TextButton(onClick = { model.askName("other") }) { Text("Other…") }
    }
    val apps = model.apps
    if (apps == null) Progress()
    else if (apps.isEmpty()) EmptyNote("No applications")
    for (app in apps.orEmpty()) {
        ListItem(
            headlineContent = { Text(app.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
            supportingContent = { Text(app.creator) },
            trailingContent = {
                Row {
                    for (action in RunningSheets.appActions) when (action) {
                        AppAction.LAUNCH -> IconButton({ actions.closeSheet(); model.launch(app.name) }) {
                            Icon(Icons.Default.PlayArrow, contentDescription = "Launch ${app.name}")
                        }
                        AppAction.EXPORT -> IconButton({ exportName = app.name; save.launch("${app.name}.zip") }) {
                            Icon(painterResource(R.drawable.ic_export), contentDescription = "Export ${app.name}")
                        }
                        AppAction.DELETE -> IconButton({ deleting = app.name }) {
                            Icon(Icons.Default.Delete, contentDescription = "Delete ${app.name}")
                        }
                    }
                }
            },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        )
    }
    deleting?.let { name ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete $name?") },
            text = { Text("Its data goes too.") },
            confirmButton = { TextButton(onClick = { deleting = null; model.delete(name) }) { Text("Delete", color = MaterialTheme.colorScheme.error) } },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    if (model.naming == "other") {
        OtherDatabaseDialog(
            model,
            onLaunch = { name -> model.naming = null; actions.closeSheet(); model.launch(name) },
            onExport = { name -> model.naming = null; exportName = name; save.launch("$name.zip") },
        )
    }
}

// Launch or export any RAM database by name, picked from the list or typed.
@Composable
private fun OtherDatabaseDialog(model: RunningModel, onLaunch: (String) -> Unit, onExport: (String) -> Unit) {
    var name by rememberSaveable { mutableStateOf("") }
    val shown = RunningSheets.matching(model.found, name)
    val valid = RunningSheets.validDatabaseName(name)
    AlertDialog(
        onDismissRequest = { model.naming = null },
        title = { Text("Other database") },
        text = {
            Column {
                OutlinedTextField(name, { name = it }, label = { Text("Database name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                if (model.listing) Progress()
                else if (shown.isNotEmpty()) {
                    LazyColumn(Modifier.padding(top = 8.dp).heightIn(max = 240.dp)) {
                        items(shown, key = { it.name }) { db ->
                            ListItem(
                                headlineContent = { Text(db.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                                supportingContent = { Text(db.creator) },
                                modifier = Modifier.clickableRow { name = db.name },
                                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            Row {
                TextButton(enabled = valid, onClick = { onLaunch(name.trim()) }) { Text("Launch") }
                TextButton(enabled = valid, onClick = { onExport(name.trim()) }) { Text("Export…") }
            }
        },
        dismissButton = { TextButton(onClick = { model.naming = null }) { Text("Cancel") } },
    )
}

@Composable
private fun CardSheet(model: RunningModel, actions: RunningBarActions) {
    LaunchedEffect(Unit) { model.readCard() }
    var confirmEject by remember { mutableStateOf(false) }
    var confirmFiles by remember { mutableStateOf(false) }
    val saveImage = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) model.job("Copying", "Couldn't export the image", cancellable = true) {
            it.exportImage(uri)
            model.note = "Card image exported"
        }
    }
    val shown = model.slotShown
    val device = model.device
    val rows = RunningSheets.cardRows(device, model.state, shown.linked != null, shown.linked != null && shown.linkedHolder == null)
    for (row in rows) when (row) {
        CardRow.SLOT -> {
            val usable = RunningSheets.slotUsable(model.state, model.busy != null, shown.pending)
            ListItem(
                headlineContent = {
                    Text(if (model.state == SdCard.State.UNKNOWN) "Not known" else shown.name,
                        color = if (usable || model.state == SdCard.State.UNKNOWN) Color.Unspecified else Faded)
                },
                supportingContent = if (model.state == SdCard.State.EJECTED) ({ Text("Ejected") }) else null,
                leadingContent = {
                    Icon(painterResource(cardIconRes(RunningSheets.cardIcon(model.state, shown.linked != null))), contentDescription = null)
                },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                modifier = if (usable) Modifier.clickableRow { model.openPicker() } else Modifier,
            )
        }
        CardRow.LINKED -> ListItem(
            headlineContent = { Text(if (shown.linkedHolder == null) "${shown.linked} is free" else "${shown.linked} is in ${shown.linkedHolder}") },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        )
        CardRow.INSERT_LINKED -> SheetAction("Insert") { model.insertLinked() }
        CardRow.FILES -> SheetAction("Files…") { if (model.state == SdCard.State.INSERTED) confirmFiles = true else model.openFiles() }
        CardRow.EJECT -> SheetAction("Eject") { confirmEject = true }
        CardRow.INSERT -> SheetAction("Insert") { model.job("Inserting", "Couldn't insert the card") { it.reinsert() } }
        CardRow.EXPORT_IMAGE -> SheetAction("Export image") {
            saveImage.launch(if (model.memoryStickPro) "Memory Stick Pro.img" else if (device.memoryStick) "Memory Stick.frondms" else "SD card.img")
        }
        CardRow.TRY_AGAIN -> SheetAction("Try again") { model.refresh() }
        CardRow.WRITE_PROTECT -> {
            val on = model.writeProtected
            val usable = on != null && model.busy == null && model.state == SdCard.State.EJECTED
            ListItem(
                headlineContent = { Text("Write protected") },
                trailingContent = {
                    Switch(checked = on == true, enabled = usable, onCheckedChange = { value ->
                        model.job("Changing protection", "Couldn't change write protection") { it.protect(value) }
                    })
                },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
            )
        }
    }
    if (confirmEject || confirmFiles) CardEjectDialog(model, confirmFiles) { confirmEject = false; confirmFiles = false }
}

@Composable
private fun DisplaySheet(model: RunningModel, state: RunningBarState, actions: RunningBarActions) {
    val device = model.device
    LaunchedEffect(Unit) { model.readMicrophone() }
    val rows = RunningSheets.displayRows(device)
    MachineDisplayRows(device, model.settings, true) { model.changeSettings(it) }
    if (DisplayRow.ROTATE in rows) SheetAction("Rotate") { actions.closeSheet(); model.rotate() }
    if (DisplayRow.SPEED in rows) MachineSpeedRow(model.settings, true) { model.changeSettings(it) }
    Header("Audio", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
    for (row in RunningSheets.audioRows(device)) when (row) {
        AudioRow.SOUND -> MachineSwitchRow("Sound", device.sound && state.sound, true, enabled = device.sound) { actions.toggleSound() }
        AudioRow.MICROPHONE -> MicrophoneRow(model)
        AudioRow.AUDIO_PREVIEW -> MachineAudioPreviewRow(device, model.settings, true, APPLIES_AT_NEXT_START) { model.changeSettings(it) }
    }
}

@Composable
private fun ConnectionsSheet(model: RunningModel, actions: RunningBarActions) {
    DisposableEffect(Unit) {
        model.startBluetoothRefresh()
        onDispose { model.stopBluetoothRefresh() }
    }
    var choosing by remember { mutableStateOf<LinkKind?>(null) }
    val device = model.device
    val debuggerShown = model.debugger.phase != NetRelay.Phase.OFF || model.debugger.error != null
    for (row in RunningSheets.connectionRows(device, model.beam.phase, model.serial.phase, model.usb.phase, debuggerShown)) {
        when (row) {
            ConnRow.BEAM -> {
                val usable = RunningSheets.beamUsable(device, model.keyboard)
                ListItem(
                    headlineContent = { Text("Infrared beaming", color = if (usable) Color.Unspecified else Faded) },
                    supportingContent = { Text(relayText(model.beam), color = if (usable) Color.Unspecified else Faded) },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = if (usable) Modifier.clickableRow { choosing = LinkKind.BEAM } else Modifier,
                )
            }
            ConnRow.BEAM_RETRY -> SheetAction("Connect again") { model.retryBeam() }
            ConnRow.BLUETOOTH -> ConnItem("Bluetooth", device.bluetooth, if (device.bluetooth) bluetoothText(model.bluetooth, model.radio) else null) { choosing = LinkKind.BLUETOOTH }
            ConnRow.SERIAL -> ConnItem("Serial port", device.serialPort, if (device.serialPort) relayText(model.serial) else null) { choosing = LinkKind.SERIAL }
            ConnRow.SERIAL_RETRY -> SheetAction("Connect again") { SerialLink.retry() }
            ConnRow.USB -> ConnItem("USB HotSync", device.usb, if (device.usb) relayText(model.usb) else null) { choosing = LinkKind.USB }
            ConnRow.USB_RETRY -> SheetAction("Connect again") { UsbLink.retry() }
            ConnRow.CAMERA -> ConnItem("Camera", false, null) { }
            ConnRow.DEBUGGER -> ConnItem("Debugger", true, debuggerText(model.debugger)) { }
            ConnRow.HOTSYNC -> SheetAction("HotSync") { actions.closeSheet(); model.hotSync() }
        }
    }
    choosing?.let { LinkChoiceDialog(it, model) { choosing = null } }
}

@Composable
private fun SheetAction(label: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(label) },
        colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.clickableRow(onClick = onClick),
    )
}

fun cardIconRes(icon: RunningSheets.CardIcon) = when (icon) {
    RunningSheets.CardIcon.IN -> R.drawable.ic_card_in
    RunningSheets.CardIcon.OUT -> R.drawable.ic_card_out
    RunningSheets.CardIcon.EJECTED -> R.drawable.ic_card_ejected
    RunningSheets.CardIcon.NONE -> R.drawable.ic_card_none
}

@Composable
private fun ConnItem(title: String, usable: Boolean, text: String?, onClick: () -> Unit) = ListItem(
    headlineContent = { Text(title, color = if (usable) Color.Unspecified else Faded) },
    supportingContent = text?.let { { Text(it, color = if (usable) Color.Unspecified else Faded) } },
    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
    modifier = if (usable) Modifier.clickableRow(onClick = onClick) else Modifier,
)
