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
import androidx.compose.ui.draw.alpha
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
import com.rm.frond.RunningSheets.ZaurusUsbMode
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
                    Sheet.CONNECTIONS -> ConnectionsSheet(model, state, actions)
                    Sheet.CONTROLS -> state.controls?.let { ControlsSheet(it) }
                }
            }
        }
    }
}

@Composable
private fun ApplicationsSheet(model: RunningModel, actions: RunningBarActions) {
    if (model.device.zaurus) { ZaurusApplications(model); return }
    if (!model.device.palmOs) { WindowsCeApplications(model); return }
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

@Composable
private fun ZaurusApplications(model: RunningModel) {
    LaunchedEffect(Unit) { model.readCard() }
    var slot by rememberSaveable { mutableStateOf(0) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        model.queueZaurusPackages(uris, slot)
    }
    val ready = (0..1).map { model.stateOf(it) == SdCard.State.EJECTED && model.protectionOf(it) == false }
    for ((n, label) in listOf("SD card", "CompactFlash").withIndex()) {
        SheetAction("Queue IPK on $label…", enabled = ready[n] && model.busy == null) {
            slot = n
            pick.launch(arrayOf("*/*"))
        }
    }
    if (ready.none { it }) EmptyNote("Eject a writable card to queue an IPK.")
}

// Windows CE: Install copies the picked files onto the SD card; the application list isn't done yet.
@Composable
private fun WindowsCeApplications(model: RunningModel) {
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> model.install(uris) }
    Row(Modifier.padding(horizontal = 8.dp)) {
        TextButton(enabled = model.device.installsToCard, onClick = { pick.launch(arrayOf("*/*")) }) { Text("Install…") }
        TextButton(enabled = false, onClick = {}) { Text("Export all…") }
        TextButton(enabled = false, onClick = {}) { Text("Other…") }
    }
    EmptyNote("Not available yet")
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
    LaunchedEffect(Unit) {
        model.readCard()
        model.readZaurusSdFamily()
        model.readZaurusCfNetwork()
        model.readZaurusUsbDrive()
        if (model.device.zaurus) model.loadUsbCable()
    }
    for (n in 0 until if (model.twoSlots) 2 else 1) key(n) { CardSlotRows(model, n) }
    if (model.device.zaurus) {
        val pickUsb = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) model.insertZaurusUsbDrive(uri)
        }
        Header("USB flash drive", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
        when (model.zaurusUsbDriveAttached) {
            true -> {
                ListItem(headlineContent = { Text("Read-only") },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent))
                SheetAction("Eject USB drive", enabled = model.busy == null) { model.ejectZaurusUsbDrive() }
            }
            false -> when (model.zaurusUsbCable) {
                ZaurusUsbMode.OFF -> SheetAction("Insert image…", enabled = model.busy == null) { pickUsb.launch(arrayOf("*/*")) }
                null -> SheetAction("Refresh USB cable", icon = Icons.Default.Refresh, enabled = model.busy == null) { model.loadUsbCable() }
                else -> ConnItem("Insert image…", false, "Disconnect the USB cable first") { }
            }
            null -> SheetAction("Try again", icon = Icons.Default.Refresh, enabled = model.busy == null) { model.readZaurusUsbDrive() }
        }
    } else {
        Header("Internal", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
        SheetAction("Files…", R.drawable.ic_folder) { model.openInternal() }
    }
}

// One card slot's rows: the card, a linked card the slot is empty for, Files…, Eject or Insert,
// Export image and a stick's write protection. Slot `n` 1 is the Zodiac's or the Dana's second SD slot.
@Composable
private fun CardSlotRows(model: RunningModel, n: Int) {
    var confirmEject by remember { mutableStateOf(false) }
    var confirmFiles by remember { mutableStateOf(false) }
    var confirmReplace by remember { mutableStateOf(false) }
    val cf = model.device.zaurus && n == 1
    val pickCf = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.insertCompactFlash(uri)
    }
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
    val rows = RunningSheets.cardRows(device, state, shown.linked != null, shown.linked != null && shown.linkedHolder == null, n, shown.oldPlace, shown.linkId != null)
    val heading = when {
        cf -> "CompactFlash"
        model.twoSlots && !device.zaurus -> "Card ${n + 1}"
        else -> device.cardLabel
    }
    if (cf) {
        val network = model.zaurusCfNetwork
        val mode = network?.substringBefore(' ')
        val adapter = when (mode) {
            "wifi" -> "D-Link DCF-660W Wi-Fi"
            "ethernet" -> "Billionton BIL-10BT Ethernet"
            else -> null
        }
        if (adapter != null) {
            Header(heading, Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp))
            ListItem(headlineContent = { Text(adapter) },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent))
            if (network?.endsWith(" offline") == true) {
                SheetAction("Reconnect CF network card", enabled = model.busy == null) {
                    model.changeZaurusCfNetwork(network.substringBefore(' '))
                }
            }
            SheetAction("Eject network card", R.drawable.ic_card_ejected, enabled = model.busy == null) {
                model.changeZaurusCfNetwork("off")
            }
            return
        }
    }
    if (rows.isNotEmpty()) Header(heading, Modifier.padding(start = 16.dp, end = 16.dp, top = if (n == 0) 8.dp else 16.dp))
    for (row in rows) when (row) {
        CardRow.SLOT -> {
            val usable = !cf && RunningSheets.slotUsable(state, model.busy != null, shown.pending) && !shown.oldPlace
            ListItem(
                headlineContent = {
                    Text(if (state == SdCard.State.UNKNOWN) "Not known" else shown.name,
                        color = if (usable || state == SdCard.State.UNKNOWN || state == SdCard.State.PALM_WIFI) Color.Unspecified else Faded)
                },
                supportingContent = if (state == SdCard.State.EJECTED) ({ Text("Ejected") }) else null,
                leadingContent = {
                    Marker(if (RunningSheets.cardIcon(state, false) == RunningSheets.CardIcon.IN) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant)
                },
                colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                modifier = if (usable) Modifier.clickableRow { model.openPicker(n = n) } else Modifier,
            )
        }
        CardRow.LINKED -> ListItem(
            headlineContent = { Text(if (shown.linkedHolder == null) "${shown.linked} is free" else "${shown.linked} is in ${shown.linkedHolder}") },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        )
        CardRow.UNLINK -> SheetAction("Unlink") { model.unlink(n) }
        CardRow.INSERT_LINKED -> SheetAction("Insert", R.drawable.ic_card_in) { model.insertLinked(n) }
        CardRow.FILES -> SheetAction("Files…", R.drawable.ic_folder) {
            if (state == SdCard.State.INSERTED) confirmFiles = true else if (model.selectSlot(n)) model.openFiles()
        }
        CardRow.EJECT -> SheetAction("Eject", R.drawable.ic_card_ejected) { confirmEject = true }
        CardRow.INSERT -> SheetAction("Insert", R.drawable.ic_card_in) {
            if (model.selectSlot(n)) model.job("Inserting", "Couldn't insert the card") { it.reinsert() }
        }
        CardRow.IMPORT_CF_IMAGE -> SheetAction(if (state == SdCard.State.NONE) "Insert image…" else "Replace image…", enabled = model.busy == null) {
            if (state == SdCard.State.NONE) pickCf.launch(arrayOf("*/*")) else confirmReplace = true
        }
        CardRow.EXPORT_IMAGE -> SheetAction("Export image", R.drawable.ic_export) {
            // frond-68k exports a card file.
            val ext = if (device.runs68k) "frondsd" else "img"
            saveImage.launch(if (cf) "CompactFlash.img" else if (n != 0) "SD card 2.$ext" else if (model.memoryStickPro) "Memory Stick Pro.img" else if (stick) "Memory Stick.frondms" else "SD card.$ext")
        }
        CardRow.INSERT_PALM_WIFI -> SheetAction("Insert Palm Wi-Fi Card", R.drawable.ic_card_in) {
            if (model.selectSlot(n)) model.job("Inserting", "Couldn't insert the Palm Wi-Fi Card") { it.insertPalmWifi() }
        }
        CardRow.EJECT_PALM_WIFI -> SheetAction("Eject Palm Wi-Fi Card", R.drawable.ic_card_ejected) {
            if (model.selectSlot(n)) model.job("Ejecting", "Couldn't eject the Palm Wi-Fi Card") { it.ejectPalmWifi() }
        }
        CardRow.TRY_AGAIN -> SheetAction("Try again", icon = Icons.Default.Refresh) {
            model.refresh()
            if (cf) model.readZaurusCfNetwork()
        }
        CardRow.WRITE_PROTECT -> {
            val on = model.protectionOf(n)
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
    if (cf && model.zaurusCfNetwork == null && state != SdCard.State.UNKNOWN) {
        SheetAction("Refresh CF card", icon = Icons.Default.Refresh) { model.readZaurusCfNetwork() }
    }
    if (cf && model.zaurusCfNetwork == "off" && (state == SdCard.State.NONE || state == SdCard.State.EJECTED)) {
        SheetAction("Insert Billionton BIL-10BT Ethernet", R.drawable.ic_card_in, enabled = model.busy == null) {
            model.changeZaurusCfNetwork("ethernet")
        }
        SheetAction("Insert D-Link DCF-660W Wi-Fi", R.drawable.ic_card_in, enabled = model.busy == null) {
            model.changeZaurusCfNetwork("wifi")
        }
    }
    if (device.zaurus && n == 0) {
        val mmc = model.zaurusMmc
        val canChange = mmc != null && model.busy == null &&
            (state == SdCard.State.NONE || state == SdCard.State.EJECTED)
        MachineSwitchRow("Use MMC", mmc == true, listStyle = true, enabled = canChange, onChange = model::changeZaurusSdFamily)
    }
    if (confirmReplace) AlertDialog(
        onDismissRequest = { confirmReplace = false },
        title = { Text("Replace the CompactFlash card?") },
        text = { Text("Export the current image first to keep its files.") },
        confirmButton = { TextButton(onClick = { confirmReplace = false; pickCf.launch(arrayOf("*/*")) }) { Text("Choose image") } },
        dismissButton = { TextButton(onClick = { confirmReplace = false }) { Text("Cancel") } },
    )
    if (confirmEject || confirmFiles) CardEjectDialog(model, confirmFiles, n) { confirmEject = false; confirmFiles = false }
}

@Composable
private fun DisplaySheet(model: RunningModel, state: RunningBarState, actions: RunningBarActions) {
    val device = model.device
    LaunchedEffect(Unit) {
        model.readMicrophone()
        model.readCamera()
        model.readZaurusHardware()
        model.readZaurusRemote()
    }
    val groups = RunningSheets.groups(device)
    var picked by androidx.compose.runtime.saveable.rememberSaveable { mutableStateOf(0) }
    val index = picked.coerceIn(0, groups.size - 1)
    androidx.compose.material3.ScrollableTabRow(selectedTabIndex = index, edgePadding = 8.dp, containerColor = Color.Transparent) {
        groups.forEachIndexed { i, group ->
            androidx.compose.material3.Tab(selected = i == index, onClick = { picked = i }, text = { Text(group.label) })
        }
    }
    val rows = RunningSheets.displayRows(device)
    when (groups[index]) {
        RunningSheets.Group.DISPLAY -> {
            MachineDisplayRows(device, model.settings, true) { model.changeSettings(it) }
            if (DisplayRow.CAMERA in rows) CameraRow(model)
            if (device.zaurus) {
                for ((mode, title) in listOf("open" to "Open lid", "closed" to "Close lid", "folded" to "Fold to portrait")) {
                    SheetAction(title, enabled = model.zaurusLid != null && model.zaurusLid != mode && model.busy == null) {
                        model.changeZaurusLid(mode)
                    }
                }
            }
        }
        RunningSheets.Group.AUDIO -> for (row in RunningSheets.audioRows(device)) when (row) {
            AudioRow.SOUND -> MachineSwitchRow("Sound", device.sound && state.sound, true, enabled = device.sound) { actions.toggleSound() }
            AudioRow.HEADPHONES -> if (device.zaurus) ZaurusAudioJack(model)
                else MachineHeadphonesRow(model.settings, true) { model.changeSettings(it) }
            AudioRow.MICROPHONE -> MicrophoneRow(model)
            AudioRow.AUDIO_PREVIEW -> MachineAudioPreviewRow(device, model.settings, true, APPLIES_AT_NEXT_START) { model.changeSettings(it) }
            AudioRow.RUMBLE -> MachineRumbleRow(device, model.settings, true) { model.changeSettings(it) }
        }
        RunningSheets.Group.GAME_CONTROLLER -> {
            val context = androidx.compose.ui.platform.LocalContext.current
            SheetAction("Set controls") {
                state.controls = ControlsEditor(context, device)
                actions.openSheet(Sheet.CONTROLS)
            }
        }
        RunningSheets.Group.BATTERY -> {
            MachineBatteryRows(model.settings, true, FrondActivity.running?.battery?.info ?: BatteryInfo()) { model.changeSettings(it) }
            if (device.zaurus)
                MachineSwitchRow("Battery door locked", model.zaurusDoorLocked == true, true,
                    enabled = model.zaurusDoorLocked != null && model.busy == null, onChange = model::changeZaurusDoor)
        }
        RunningSheets.Group.SPEED -> MachineSpeedRow(device, model.settings, true) { model.changeSettings(it) }
    }
}

@Composable
private fun ZaurusAudioJack(model: RunningModel) {
    val remote = model.zaurusRemoteAttached
    val ready = remote != null && model.busy == null
    MachineSwitchRow("Headphones in", remote == false && model.settings.headphonesIn, true,
        enabled = ready, onChange = model::changeZaurusHeadphones)
    MachineSwitchRow("CE-RH2 remote in", remote == true, true,
        enabled = ready, onChange = model::changeZaurusRemote)
    if (remote == true) {
        for ((key, title) in listOf(
            "play" to "Play / pause", "stop" to "Stop", "previous" to "Previous", "next" to "Next",
            "volume-down" to "Volume down", "volume-up" to "Volume up", "mute" to "Mute",
        )) {
            SheetAction(title, enabled = ready) { model.tapZaurusRemote(key) }
        }
    }
}

// The controller keys for each Palm button. The session sends the next controller key to the editor while a button waits for one.
@Composable
private fun ControlsSheet(editor: ControlsEditor) {
    TextButton({ editor.reset() }, Modifier.padding(horizontal = 8.dp)) { Text("Reset to defaults") }
    Column(Modifier.padding(horizontal = 16.dp).padding(bottom = 16.dp)) { ControllerButtons(editor) }
}

@Composable
private fun ConnectionsSheet(model: RunningModel, state: RunningBarState, actions: RunningBarActions) {
    DisposableEffect(Unit) {
        model.startBluetoothRefresh()
        model.loadUsbCable()
        model.readZaurusUsbDrive()
        model.readCradle()
        model.readSyncPorts()
        onDispose { model.stopBluetoothRefresh() }
    }
    var choosing by remember { mutableStateOf<LinkKind?>(null) }
    val device = model.device
    if (state.syncProtocol != null) SheetAction("Sync settings") { actions.closeSheet(); state.syncSettings = true }
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
            ConnRow.SERIAL -> ConnItem("Serial port", device.serialPort, if (device.serialPort) (if (model.serialSync) "Sync Manager" else model.serial.text) else null) { choosing = LinkKind.SERIAL }
            ConnRow.USB -> if (device.usbNetwork) ConnItem("USB", device.usb, if (device.usb) (if (model.usbCable) "On" else "Off") else null) { choosing = LinkKind.USB }
                else ConnItem("USB HotSync", device.usb, if (device.usb) (if (model.usbSync) "Sync Manager" else relayText(model.usb)) else null) { choosing = LinkKind.USB }
            ConnRow.ZAURUS_USB -> {
                val cable = model.zaurusUsbCable
                val drive = model.zaurusUsbDriveAttached
                val ready = model.busy == null && !model.waitingForGdb
                when {
                    drive == true -> ConnItem("USB cable", false, "USB flash drive inserted") { }
                    drive == null || cable == null -> SheetAction("Refresh USB cable", icon = Icons.Default.Refresh, enabled = ready) {
                        model.readZaurusUsbDrive()
                        model.loadUsbCable()
                    }
                    else -> ConnItem("USB cable", ready, cable.label) { choosing = LinkKind.USB }
                }
            }
            ConnRow.USB_RETRY -> SheetAction("Connect again") { UsbLink.retry() }
            ConnRow.USB_PORT -> model.printer?.let { PrinterRow(it, device) } ?: ConnItem("USB port", false, null) { }
            ConnRow.WIFI_MODULE -> model.printer?.let { WifiModuleRow(it) } ?: ConnItem("Wi-Fi", false, null) { }
            ConnRow.DEBUGGER -> ConnItem("Debugger", true, debuggerText(model.debugger)) { }
            ConnRow.CRADLE -> MachineSwitchRow("Cradle attached", model.cradleAttached == true, true,
                enabled = model.cradleAttached != null && model.busy == null && !model.waitingForGdb,
                onChange = model::changeCradle)
            ConnRow.HOTSYNC -> SheetAction("HotSync", enabled = !model.outOfCradle) { actions.closeSheet(); model.hotSync() }
        }
    }
    if (model.device.pendingUsb.isNotEmpty()) ConnItem(model.device.pendingUsb, false, null) { }
    choosing?.let { LinkChoiceDialog(it, model) { choosing = null } }
}

@Composable
private fun SheetAction(label: String, iconRes: Int? = null, icon: androidx.compose.ui.graphics.vector.ImageVector? = null, enabled: Boolean = true, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(label) },
        leadingContent = when {
            iconRes != null -> ({ Icon(painterResource(iconRes), contentDescription = null) })
            icon != null -> ({ Icon(icon, contentDescription = null) })
            else -> null
        },
        colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = if (enabled) Modifier.clickableRow(onClick = onClick) else Modifier.alpha(0.38f),
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

