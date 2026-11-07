package com.rm.frond.ui

import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.BeamMode
import com.rm.frond.BtLink
import com.rm.frond.BtMode
import com.rm.frond.FatImage
import com.rm.frond.IrBeam
import com.rm.frond.NetRelay
import com.rm.frond.NetworkMode
import com.rm.frond.PalmDatabases
import com.rm.frond.PeripheralsModel
import com.rm.frond.SdCard
import com.rm.frond.SerialLink
import com.rm.frond.UsbLink

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeripheralsScreen(model: PeripheralsModel, onClose: () -> Unit) {
    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }
    var confirmEject by remember { mutableStateOf(false) }
    var confirmFiles by remember { mutableStateOf(false) }
    var confirmReset by remember { mutableStateOf(false) }
    var confirmHardReset by remember { mutableStateOf(false) }
    var choosingNetwork by remember { mutableStateOf(false) }
    var choosingBeam by remember { mutableStateOf(false) }
    var choosingBluetooth by remember { mutableStateOf(false) }
    var choosingSerial by remember { mutableStateOf(false) }
    var choosingUsb by remember { mutableStateOf(false) }
    val saveCopy = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) model.saveCopy(uri)
    }

    val saveImage = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) model.job("Copying", "Couldn't export the image", cancellable = true) {
            it.exportImage(uri)
            model.note = "Card image exported"
        }
    }
    val pickApp = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        model.install(uris)
    }
    // The database to export once a document is picked; null exports all RAM databases.
    var exportName by rememberSaveable { mutableStateOf<String?>(null) }
    val saveZip = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) model.export(uri, exportName)
    }

    LaunchedEffect(model.note) {
        model.note?.let {
            snackbar.showSnackbar(it)
            model.note = null
        }
    }

    val path = model.path
    if (path != null) {
        CardFilesScreen(model, snackbar)
    } else Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(model.device.label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = onClose) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
            LazyColumn(Modifier.padding(padding).fillMaxSize()) {
                fun action(label: String, onClick: () -> Unit) = item(key = label) {
                    ListItem(headlineContent = { Text(label) }, modifier = Modifier.clickableRow(onClick = onClick))
                }
                // A 68k device shows only the hardware it has; the others grey out what isn't done yet.
                val m68k = model.device.runs68k
                fun has(present: Boolean) = !m68k || present
                item { Header("Applications", Modifier.padding(horizontal = 16.dp)) }
                action("Install application") { pickApp.launch(arrayOf("*/*")) }
                action("Launch application") { model.askName("launch") }
                action("Export RAM databases") {
                    exportName = null
                    saveZip.launch("Palm databases.zip")
                }
                action("Export database") { model.askName("export") }
                if (model.device.storageSlot == "memory-stick" && !model.device.memoryStick) {
                    item(key = "memoryStick") {
                        ListItem(
                            headlineContent = { Text("Memory Stick", color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)) },
                            supportingContent = { Text("Not available yet") },
                        )
                    }
                }
                if (model.device.cardSupported) {
                item { Header(model.device.cardLabel, Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                // One row for the card in the slot: its name or "No card" (opens the picker), and its
                // state only when the actions below don't say it.
                item(key = "cardRow") {
                    val shown = model.slotShown
                    val colour = when (model.state) {
                        SdCard.State.INSERTED -> MaterialTheme.colorScheme.primary
                        SdCard.State.EJECTED -> MaterialTheme.colorScheme.secondary
                        SdCard.State.NONE -> MaterialTheme.colorScheme.outlineVariant
                        SdCard.State.UNKNOWN -> MaterialTheme.colorScheme.error
                    }
                    val usable = model.state != SdCard.State.UNKNOWN && model.busy == null && !shown.pending
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = {
                            Text(if (model.state == SdCard.State.UNKNOWN) "Not known" else shown.name,
                                color = if (usable || model.state == SdCard.State.UNKNOWN) Color.Unspecified else faded)
                        },
                        supportingContent = if (model.state == SdCard.State.EJECTED) ({ Text("Ejected") }) else null,
                        leadingContent = { Marker(colour) },
                        modifier = if (usable) Modifier.clickableRow { model.openPicker() } else Modifier,
                    )
                }
                if (model.state == SdCard.State.NONE) model.slotShown.linked?.let { linked ->
                    val holder = model.slotShown.linkedHolder
                    item(key = "linked") {
                        ListItem(headlineContent = { Text(if (holder == null) "$linked is free" else "$linked is in $holder") })
                    }
                    if (holder == null) action("Insert") { model.insertLinked() }
                }
                when (model.state) {
                    SdCard.State.INSERTED -> {
                        action("Files") { confirmFiles = true }
                        action("Eject") { confirmEject = true }
                    }
                    SdCard.State.EJECTED -> {
                        action("Insert") { model.job("Inserting", "Couldn't insert the card") { it.reinsert() } }
                        if (model.device.sdSlot) action("Files") { model.openFiles() }
                        action("Export image") { saveImage.launch(if (model.memoryStickPro) "Memory Stick Pro.img" else if (model.device.memoryStick) "Memory Stick.frondms" else "SD card.img") }
                    }
                    SdCard.State.NONE -> Unit
                    SdCard.State.UNKNOWN -> action("Try again") { model.refresh() }
                }
                if (model.device.memoryStick && model.state != SdCard.State.NONE && model.state != SdCard.State.UNKNOWN) item {
                    val on = model.writeProtected
                    val usable = on != null && model.busy == null && model.state == SdCard.State.EJECTED
                    ListItem(
                        headlineContent = { Text("Write protected") },
                        trailingContent = { Switch(checked = on == true, enabled = usable, onCheckedChange = { value ->
                            model.job("Changing protection", "Couldn't change write protection") { it.protect(value) }
                        }) },
                    )
                }
                }
                item { Header("Display", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                item(key = "display") {
                    Column {
                        MachineDisplayRows(model.device, model.settings, true) { model.changeSettings(it) }
                    }
                }
                item { Header("Audio", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                item(key = "sound") {
                    MachineSoundRow(model.device, model.settings, true) { model.changeSettings(it) }
                }
                if (has(model.device.microphone)) item(key = "microphone") { MicrophoneRow(model) }
                if (!m68k) item(key = "audioPreview") {
                    MachineAudioPreviewRow(model.device, model.settings, true, APPLIES_AT_NEXT_START) {
                        model.changeSettings(it)
                    }
                }
                item { Header("Device", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                if (m68k) item(key = "speed") { MachineSpeedRow(model.settings, true) { model.changeSettings(it) } }
                if ("power" in model.device.buttons) action("Power") { model.power() }
                if (model.device.holdPresent) item(key = "hold") {
                    val on = model.hold
                    val usable = model.device.holdSupported && on != null && model.busy == null
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Hold", color = if (usable) Color.Unspecified else faded) },
                        trailingContent = { Switch(checked = on == true, enabled = usable, onCheckedChange = { model.changeHold(it) }) },
                        modifier = if (usable) Modifier.clickableRow { model.changeHold(on != true) } else Modifier,
                    )
                }
                if (has(model.device.keyboard)) item(key = "keyboard") {
                    val attached = model.keyboard
                    // The keyboard and beaming share the Palm's infrared port.
                    val usable = model.device.keyboard && attached != null && model.beam.phase == NetRelay.Phase.OFF
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Infrared keyboard", color = if (usable) Color.Unspecified else faded) },
                        trailingContent = {
                            Switch(checked = attached == true, enabled = usable, onCheckedChange = { model.attachKeyboard(it) })
                        },
                        modifier = if (usable) Modifier.clickableRow { model.attachKeyboard(attached != true) } else Modifier,
                    )
                }
                item(key = "beam") {
                    val beam = model.beam
                    val usable = model.device.infrared && model.keyboard == false
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Infrared beaming", color = if (usable) Color.Unspecified else faded) },
                        supportingContent = { Text(relayText(beam), color = if (usable) Color.Unspecified else faded) },
                        modifier = if (usable) Modifier.clickableRow { choosingBeam = true } else Modifier,
                    )
                }
                if (model.beam.phase == NetRelay.Phase.DISCONNECTED) action("Connect again") { model.retryBeam() }
                if (has(model.device.bluetooth)) item(key = "bluetooth") {
                    val usable = model.device.bluetooth
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Bluetooth", color = if (usable) Color.Unspecified else faded) },
                        supportingContent = if (usable) ({ Text(bluetoothText(model.bluetooth, model.radio)) }) else null,
                        modifier = if (usable) Modifier.clickableRow { choosingBluetooth = true } else Modifier,
                    )
                }
                if (!model.device.serialAbsent) item(key = "serial") {
                    val usable = model.device.serialPort
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Serial port", color = if (usable) Color.Unspecified else faded) },
                        supportingContent = if (usable) ({ Text(relayText(model.serial)) }) else null,
                        modifier = if (usable) Modifier.clickableRow { choosingSerial = true } else Modifier,
                    )
                }
                if (model.device.serialPort && model.serial.phase == NetRelay.Phase.DISCONNECTED) action("Connect again") { SerialLink.retry() }
                if (has(model.device.usb)) item(key = "usb") {
                    val usable = model.device.usb
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("USB HotSync", color = if (usable) Color.Unspecified else faded) },
                        supportingContent = if (usable) ({ Text(relayText(model.usb)) }) else null,
                        modifier = if (usable) Modifier.clickableRow { choosingUsb = true } else Modifier,
                    )
                }
                if (model.device.usb && model.usb.phase == NetRelay.Phase.DISCONNECTED) action("Connect again") { UsbLink.retry() }
                // The core doesn't emulate the camera yet.
                if (model.device.camera) item(key = "camera") {
                    ListItem(headlineContent = { Text("Camera", color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)) })
                }
                if (model.debugger.phase != NetRelay.Phase.OFF || model.debugger.error != null) {
                    item(key = "debugger") {
                        ListItem(
                            headlineContent = { Text("Debugger") },
                            supportingContent = { Text(debuggerText(model.debugger)) },
                        )
                    }
                }
                if (has(model.device.network)) item(key = "network") {
                    val mode = model.network
                    val usable = model.device.network && mode != null
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Network", color = if (model.device.network) Color.Unspecified else faded) },
                        supportingContent = { Text(mode?.label ?: "", color = if (model.device.network) Color.Unspecified else faded) },
                        modifier = if (usable) Modifier.clickableRow { choosingNetwork = true } else Modifier,
                    )
                }
                action("Reset") { confirmReset = true }
                if (!m68k) action("Rotate") { model.rotate() }
                // The connector's HotSync button: the IIIx cradle, the T|X and the E2.
                if (model.device.hotSyncButton) action("HotSync") { model.hotSync() }
                item { Header("Session", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                action("Save a copy") { saveCopy.launch("${model.name}.zip") }
                action("Emulator log") { model.showLog() }
                action("Hard reset") { confirmHardReset = true }
                item { Spacer(Modifier.size(16.dp)) }
            }
    }

    BusyDialog(model)
    AlertBox(model)
    if (choosingNetwork) {
        NetworkDialog(model.network, onDismiss = { choosingNetwork = false }) { mode ->
            choosingNetwork = false
            model.changeNetwork(mode)
        }
    }
    if (choosingBeam) LinkChoiceDialog(LinkKind.BEAM, model) { choosingBeam = false }
    if (choosingSerial) LinkChoiceDialog(LinkKind.SERIAL, model) { choosingSerial = false }
    if (choosingUsb) LinkChoiceDialog(LinkKind.USB, model) { choosingUsb = false }
    if (choosingBluetooth) LinkChoiceDialog(LinkKind.BLUETOOTH, model) { choosingBluetooth = false }
    CardPickerDialogs(model)
    if (confirmEject || confirmFiles) CardEjectDialog(model, confirmFiles) { confirmEject = false; confirmFiles = false }
    if (confirmReset) {
        ResetDialog(model.device, onDismiss = { confirmReset = false }) { confirmReset = false; model.reset() }
    }
    if (confirmHardReset) {
        HardResetDialog(model.device, onDismiss = { confirmHardReset = false }) { saveFirst ->
            confirmHardReset = false
            model.hardReset(saveFirst)
        }
    }
    model.naming?.let { what ->
        DatabaseNameDialog(
            title = if (what == "launch") "Launch application" else "Export database",
            action = if (what == "launch") "Launch" else "Export",
            found = model.found,
            listing = model.listing,
            onDismiss = { model.naming = null },
            onChoose = { name ->
                model.naming = null
                if (what == "launch") model.launch(name)
                else {
                    exportName = name
                    saveZip.launch("$name.zip")
                }
            },
        )
    }
}

// Asks for a Palm database name (up to 31 bytes), with the matching databases found in RAM to pick from.
@Composable
private fun DatabaseNameDialog(
    title: String,
    action: String,
    found: List<PalmDatabases.Info>,
    listing: Boolean,
    onDismiss: () -> Unit,
    onChoose: (String) -> Unit,
) {
    var name by remember { mutableStateOf("") }
    val shown = found.filter { it.name.contains(name.trim(), ignoreCase = true) }
    val valid = name.isNotBlank() && name.toByteArray(Charsets.ISO_8859_1).size <= 31
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                OutlinedTextField(
                    name,
                    { name = it },
                    label = { Text("Palm database name") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                if (listing) Progress()
                else if (shown.isNotEmpty()) {
                    LazyColumn(Modifier.padding(top = 8.dp).heightIn(max = 280.dp)) {
                        items(shown, key = { it.name }) { db ->
                            ListItem(
                                headlineContent = { Text(db.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                                supportingContent = { Text(db.creator) },
                                modifier = Modifier.clickableRow { onChoose(db.name) },
                            )
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(enabled = valid, onClick = { onChoose(name.trim()) }) { Text(action) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

internal fun relayText(s: NetRelay.Status): String = when (s.phase) {
    NetRelay.Phase.OFF -> s.error?.let { "Off. $it" } ?: "Off"
    NetRelay.Phase.WAITING -> "Waiting for a peer on port ${(s.mode as? BeamMode.Listen)?.port}"
    NetRelay.Phase.CONNECTING -> "Connecting to ${(s.mode as? BeamMode.Connect)?.let { NetRelay.hostPort(it.host, it.port) }}"
    NetRelay.Phase.CONNECTED -> "Connected to ${s.peer ?: "a peer"}"
    NetRelay.Phase.DISCONNECTED -> s.error ?: "Disconnected"
}

internal fun debuggerText(s: NetRelay.Status): String = when (s.phase) {
    NetRelay.Phase.CONNECTED -> "gdb connected from ${s.peer ?: "the network"}"
    NetRelay.Phase.WAITING -> "Waiting for gdb on port ${(s.mode as? BeamMode.Listen)?.port}"
    else -> s.error?.let { "Off. $it" } ?: "Off"
}

internal fun bluetoothText(s: BtLink.Status, radio: String?): String {
    val link = when (val mode = s.mode) {
        BtMode.Off -> return s.error?.let { "Off. $it" } ?: "Off"
        is BtMode.Host -> "Hosting on port ${mode.port}" + when (s.links) {
            0 -> ""
            1 -> ", 1 Frond connected"
            else -> ", ${s.links} Fronds connected"
        }
        is BtMode.Join -> if (s.links > 0) "Connected to ${NetRelay.hostPort(mode.host, mode.port)}"
        else s.error ?: "Joined to ${NetRelay.hostPort(mode.host, mode.port)}"
    }
    // The core's live observation of the Palm's radio and ACL links.
    val note = when {
        radio == null -> null
        radio.contains("turn Bluetooth off and on") -> "Turn Bluetooth off and on in Palm OS."
        radio.startsWith("disconnected: ") -> radio.removePrefix("disconnected: ")
        else -> radio.takeUnless { it.isBlank() || it == "off" }
    }
    return if (note == null) link else "$link. $note"
}

// Off, wait for a peer on a port, or connect to an address, for one of the network relays.
@Composable
private fun RelayDialog(
    title: String,
    labels: List<String>,
    relay: NetRelay,
    current: BeamMode,
    onDismiss: () -> Unit,
    onChoose: (BeamMode) -> Unit,
) {
    val context = LocalContext.current
    val last = remember { relay.last(context) }
    val start = if (current != BeamMode.Off) current else last ?: BeamMode.Off
    LinkDialog(
        title = title,
        labels = labels,
        defaultPort = relay.defaultPort,
        choice = if (current == BeamMode.Off) "off" else start.encode().substringBefore(':'),
        port = (start as? BeamMode.Listen)?.port,
        address = (last as? BeamMode.Connect ?: start as? BeamMode.Connect)?.let { NetRelay.hostPort(it.host, it.port) },
        addresses = produceState<List<String>?>(null) { value = withContext(Dispatchers.IO) { NetRelay.addresses(context) } }.value,
        onDismiss = onDismiss,
        toMode = { choice, port, address ->
            when (choice) {
                "listen" -> NetRelay.port(port)?.let { BeamMode.Listen(it) }
                "connect" -> relay.address(address)?.let { BeamMode.Connect(it.first, it.second) }
                else -> BeamMode.Off
            }
        },
        onChoose = onChoose,
    )
}

// Off, take peers on a port (choice "listen") or reach an address ("connect").
@Composable
private fun <T> LinkDialog(
    title: String,
    // For off, listen and connect.
    labels: List<String>,
    defaultPort: Int,
    choice: String,
    port: Int?,
    address: String?,
    addresses: List<String>?,
    onDismiss: () -> Unit,
    toMode: (choice: String, port: String, address: String) -> T?,
    onChoose: (T) -> Unit,
) {
    var picked by remember { mutableStateOf(choice) }
    var portText by remember { mutableStateOf((port ?: defaultPort).toString()) }
    var addressText by remember { mutableStateOf(address ?: "") }
    val mode = toMode(picked, portText, addressText)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                for ((id, label) in listOf("off", "listen", "connect").zip(labels)) {
                    ListItem(
                        headlineContent = { Text(label) },
                        leadingContent = { RadioButton(selected = picked == id, onClick = null) },
                        modifier = Modifier.clickableRow { picked = id },
                    )
                }
                if (picked == "listen") {
                    OutlinedTextField(
                        portText,
                        { portText = it.filter(Char::isDigit).take(5) },
                        label = { Text("Port") },
                        singleLine = true,
                        isError = mode == null,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text(
                        if (addresses == null) "Looking up this device's address" else if (addresses.isEmpty()) "No network address" else "This device: " + addresses.joinToString(", "),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
                if (picked == "connect") {
                    OutlinedTextField(
                        addressText,
                        { addressText = it.trim() },
                        label = { Text("Address and port") },
                        placeholder = { Text("192.168.1.20:$defaultPort") },
                        singleLine = true,
                        isError = addressText.isNotEmpty() && mode == null,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        },
        confirmButton = { TextButton(enabled = mode != null, onClick = { mode?.let(onChoose) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}


// Asks before ejecting the card, or before opening its files (which ejects it).
@Composable
fun CardEjectDialog(model: PeripheralsModel, files: Boolean, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (files && model.device.memoryStick) "Open Memory Stick files?" else "Eject the card?") },
        text = { Text(if (files && model.device.memoryStick) "Close files on the Palm first; changes are saved immediately."
            else if (files) "Close any files open on the Palm first. Frond saves a recovery session before the exchange." else "Close any files open on the Palm first.") },
        confirmButton = {
            TextButton(onClick = {
                onDismiss()
                if (files) model.openFiles() else model.job("Ejecting", "Couldn't eject the card") { it.eject() }
            }) { Text(if (files && model.device.memoryStick) "Open" else "Eject") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// The card picker and the dialogs it leads to: new card, link to machines, and the confirm before a change.
@Composable
fun CardPickerDialogs(model: PeripheralsModel) {
    val context = LocalContext.current
    model.picker?.let { p ->
        if (!model.pickerMaking && model.pickerLinking == null && !model.confirmingChange && model.busy == null) {
            CardPickerDialog(p.title, p.choices, p.selected, onSelect = { model.pick(it) }, onNewCard = { model.pickerMaking = true },
                onLink = { model.startPickerLink() }, onDismiss = { model.picker = null }, onOk = { model.pickerOk() })
        }
    }
    if (model.pickerMaking) {
        NewCardDialog(com.rm.frond.Cards.libraryFor(com.rm.frond.Sessions.folder(context)), model.device,
            onDismiss = { model.pickerMaking = false }) { name, kind, size -> model.makeCard(name, kind, size) }
    }
    model.pickerLinking?.let { (card, options) ->
        LinkCardsDialog(card.name, options, onDismiss = { model.pickerLinking = null }) { model.linkFromPicker(card, it) }
    }
    if (model.confirmingChange) {
        AlertDialog(
            onDismissRequest = { model.confirmChange(false) },
            title = { Text("Eject the card?") },
            text = { Text("Close any files open on the Palm first.") },
            confirmButton = { TextButton(onClick = { model.confirmChange(true) }) { Text("Eject") } },
            dismissButton = { TextButton(onClick = { model.confirmChange(false) }) { Text("Cancel") } },
        )
    }
}

enum class LinkKind { BEAM, BLUETOOTH, SERIAL, USB }

// The chooser for one of the network links: off, wait for a peer, or connect to an address.
@Composable
fun LinkChoiceDialog(kind: LinkKind, model: PeripheralsModel, onDismiss: () -> Unit) {
    val context = LocalContext.current
    when (kind) {
        LinkKind.BEAM -> RelayDialog("Infrared beaming", listOf("Off", "Wait for a peer", "Connect to another Frond"), IrBeam,
            model.beam.mode, onDismiss) { onDismiss(); model.changeBeam(it) }
        LinkKind.SERIAL -> RelayDialog("Serial port", listOf("Off", "Wait for a peer", "Connect to an address"), SerialLink,
            model.serial.mode, onDismiss) { onDismiss(); model.changeSerial(it) }
        LinkKind.USB -> RelayDialog("USB HotSync", listOf("Off", "Wait for a peer", "Connect to an address"), UsbLink,
            model.usb.mode, onDismiss) { onDismiss(); model.changeUsb(it) }
        LinkKind.BLUETOOTH -> {
            val current = model.bluetooth.mode
            val last = remember { BtLink.last(context) }
            val start = if (current != BtMode.Off) current else last ?: BtMode.Off
            LinkDialog(
                title = "Bluetooth",
                labels = listOf("Off", "Host other Fronds", "Join another Frond"),
                defaultPort = BtLink.DEFAULT_PORT,
                choice = when (current) {
                    is BtMode.Host -> "listen"
                    is BtMode.Join -> "connect"
                    BtMode.Off -> "off"
                },
                port = (start as? BtMode.Host)?.port,
                address = (last as? BtMode.Join ?: start as? BtMode.Join)?.let { NetRelay.hostPort(it.host, it.port) },
                addresses = produceState<List<String>?>(null) { value = withContext(Dispatchers.IO) { NetRelay.addresses(context) } }.value,
                onDismiss = onDismiss,
                toMode = { choice, port, address ->
                    when (choice) {
                        "listen" -> NetRelay.port(port)?.let { BtMode.Host(it) }
                        "connect" -> BtLink.address(address)?.let { BtMode.Join(it.first, it.second) }
                        else -> BtMode.Off
                    }
                },
                onChoose = { onDismiss(); model.changeBluetooth(it) },
            )
        }
    }
}
