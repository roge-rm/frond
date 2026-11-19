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
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
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
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.BatteryInfo
import com.rm.frond.FrondActivity
import com.rm.frond.InfraredChoice
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
    // While the list says the Palm is asleep, ask again every 2 s (a plain list never wakes it).
    LaunchedEffect(Unit) {
        model.loadApps()
        while (true) {
            delay(2000)
            if (model.appsNote != null) model.loadApps()
        }
    }
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
    else if (apps.isEmpty()) EmptyNote(model.appsNote ?: "No applications")
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
    FrondBenchRows(enabled = model.canInstallFrondBench, onInstall = { model.installFrondBench() })
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
                else if (shown.isEmpty() && model.foundNote != null) EmptyNote(model.foundNote!!)
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
    for (n in 0 until if (model.twoSlots) 2 else 1) key(n) { CardSlotRows(model, n) }
    Header("Internal", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
    SheetAction("Files…", R.drawable.ic_folder) { model.openInternal() }
}

// One card slot's rows: the card, a linked card the slot is empty for, Files…, Eject or Insert,
// Export image and a stick's write protection. Slot `n` 1 is the Zodiac's or the Dana's second SD slot.
@Composable
private fun CardSlotRows(model: RunningModel, n: Int) {
    var confirmEject by remember { mutableStateOf(false) }
    var confirmFiles by remember { mutableStateOf(false) }
    val saveImage = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null && model.selectSlot(n)) model.job("Copying", "Couldn't export the image", cancellable = true) {
            it.exportImage(uri)
            model.note = "Card image exported"
        }
    }
    val shown = model.shownOf(n)
    val state = model.stateOf(n)
    val device = model.device
    val stick = device.memoryStick && n == 0
    val rows = RunningSheets.cardRows(device, state, shown.linked != null, shown.linked != null && shown.linkedHolder == null, n, shown.oldPlace)
    val heading = if (model.twoSlots) "Card ${n + 1}" else device.cardLabel
    if (rows.isNotEmpty()) Header(heading, Modifier.padding(start = 16.dp, end = 16.dp, top = if (n == 0) 8.dp else 16.dp))
    for (row in rows) when (row) {
        CardRow.SLOT -> {
            val usable = RunningSheets.slotUsable(state, model.busy != null, shown.pending) && !shown.oldPlace
            ListItem(
                headlineContent = {
                    Text(if (state == SdCard.State.UNKNOWN) "Not known" else shown.name,
                        color = if (usable || state == SdCard.State.UNKNOWN) Color.Unspecified else Faded)
                },
                supportingContent = if (state == SdCard.State.EJECTED) ({ Text("Ejected") }) else null,
                leadingContent = {
                    Marker(if (state == SdCard.State.INSERTED) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant)
                },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                modifier = if (usable) Modifier.clickableRow { model.openPicker(n = n) } else Modifier,
            )
        }
        CardRow.LINKED -> ListItem(
            headlineContent = { Text(if (shown.linkedHolder == null) "${shown.linked} is free" else "${shown.linked} is in ${shown.linkedHolder}") },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        )
        CardRow.INSERT_LINKED -> SheetAction("Insert", R.drawable.ic_card_in) { model.insertLinked(n) }
        CardRow.FILES -> SheetAction("Files…", R.drawable.ic_folder) {
            if (state == SdCard.State.INSERTED) confirmFiles = true else if (model.selectSlot(n)) model.openFiles()
        }
        CardRow.EJECT -> SheetAction("Eject", R.drawable.ic_card_ejected) { confirmEject = true }
        CardRow.INSERT -> SheetAction("Insert", R.drawable.ic_card_in) {
            if (model.selectSlot(n)) model.job("Inserting", "Couldn't insert the card") { it.reinsert() }
        }
        CardRow.EXPORT_IMAGE -> SheetAction("Export image", R.drawable.ic_export) {
            // frond-68k exports a card file.
            val ext = if (device.runs68k) "frondsd" else "img"
            saveImage.launch(if (n != 0) "SD card 2.$ext" else if (model.memoryStickPro) "Memory Stick Pro.img" else if (stick) "Memory Stick.frondms" else "SD card.$ext")
        }
        CardRow.TRY_AGAIN -> SheetAction("Try again", icon = Icons.Default.Refresh) { model.refresh() }
        CardRow.WRITE_PROTECT -> {
            val on = model.writeProtected
            val usable = on != null && model.busy == null && state == SdCard.State.EJECTED
            ListItem(
                headlineContent = { Text("Write protected") },
                leadingContent = { Icon(Icons.Default.Lock, contentDescription = null) },
                trailingContent = {
                    Switch(checked = on == true, enabled = usable, onCheckedChange = { value ->
                        if (model.selectSlot(n)) model.job("Changing protection", "Couldn't change write protection") { it.protect(value) }
                    })
                },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
            )
        }
    }
    if (confirmEject || confirmFiles) CardEjectDialog(model, confirmFiles, n) { confirmEject = false; confirmFiles = false }
}

@Composable
private fun DisplaySheet(model: RunningModel, state: RunningBarState, actions: RunningBarActions) {
    val device = model.device
    LaunchedEffect(Unit) { model.readMicrophone(); model.readCamera() }
    val rows = RunningSheets.displayRows(device)
    MachineDisplayRows(device, model.settings, true) { model.changeSettings(it) }
    if (DisplayRow.ROTATE in rows) SheetAction("Rotate") { actions.closeSheet(); model.rotate() }
    if (DisplayRow.SPEED in rows) MachineSpeedRow(device, model.settings, true) { model.changeSettings(it) }
    if (DisplayRow.CAMERA in rows) CameraRow(model)
    Header("Battery", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
    MachineBatteryRows(model.settings, true, FrondActivity.running?.battery?.info ?: BatteryInfo()) { model.changeSettings(it) }
    Header("Audio", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
    for (row in RunningSheets.audioRows(device)) when (row) {
        AudioRow.SOUND -> MachineSwitchRow("Sound", device.sound && state.sound, true, enabled = device.sound) { actions.toggleSound() }
        AudioRow.MICROPHONE -> MicrophoneRow(model)
        AudioRow.AUDIO_PREVIEW -> MachineAudioPreviewRow(device, model.settings, true, APPLIES_AT_NEXT_START) { model.changeSettings(it) }
        AudioRow.RUMBLE -> MachineRumbleRow(model.settings, true) { model.changeSettings(it) }
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
    // Prints waiting for a choice come first, one row each, so the prints notice's Show lands on them.
    val ready = listOfNotNull(model.printer?.jobs, model.irPrinter).filter { it.held != null }
    if (ready.isNotEmpty()) {
        Header("Prints", Modifier.padding(start = 16.dp, end = 16.dp, top = 8.dp))
        for (jobs in ready) HeldRow(jobs, ready.size > 1)
        Header("Connections", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
    }
    for (row in RunningSheets.connectionRows(device, model.usb.phase, debuggerShown)) {
        when (row) {
            ConnRow.BEAM -> {
                val usable = RunningSheets.beamUsable(device, model.keyboard)
                ListItem(
                    headlineContent = { Text("Infrared beaming", color = if (usable) Color.Unspecified else Faded) },
                    supportingContent = {
                        val jobs = model.irPrinter
                        Text(if (model.beam.choice == InfraredChoice.PRINTER && jobs != null) "Printer, ${jobs.bytes} bytes received" else model.beam.text,
                            color = if (usable) Color.Unspecified else Faded)
                    },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = if (usable) Modifier.clickableRow { choosing = LinkKind.BEAM } else Modifier,
                )
            }
            ConnRow.BLUETOOTH -> ConnItem("Bluetooth", device.bluetooth, if (device.bluetooth) bluetoothText(model.bluetooth, model.radio) else null) { choosing = LinkKind.BLUETOOTH }
            ConnRow.SERIAL -> ConnItem("Serial port", device.serialPort, if (device.serialPort) model.serial.text else null) { choosing = LinkKind.SERIAL }
            ConnRow.USB -> ConnItem("USB HotSync", device.usb, if (device.usb) relayText(model.usb) else null) { choosing = LinkKind.USB }
            ConnRow.USB_RETRY -> SheetAction("Connect again") { UsbLink.retry() }
            ConnRow.USB_PORT -> model.printer?.let { PrinterRow(it, device) } ?: ConnItem("USB port", false, null) { }
            ConnRow.WIFI_MODULE -> model.printer?.let { WifiModuleRow(it) } ?: ConnItem("Wi-Fi", false, null) { }
            ConnRow.DEBUGGER -> ConnItem("Debugger", true, debuggerText(model.debugger)) { }
            ConnRow.HOTSYNC -> SheetAction("HotSync") { actions.closeSheet(); model.hotSync() }
        }
    }
    if (model.device.pendingUsb.isNotEmpty()) ConnItem(model.device.pendingUsb, false, null) { }
    choosing?.let { LinkChoiceDialog(it, model) { choosing = null } }
}

@Composable
private fun SheetAction(label: String, iconRes: Int? = null, icon: androidx.compose.ui.graphics.vector.ImageVector? = null, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(label) },
        leadingContent = when {
            iconRes != null -> ({ Icon(painterResource(iconRes), contentDescription = null) })
            icon != null -> ({ Icon(icon, contentDescription = null) })
            else -> null
        },
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

