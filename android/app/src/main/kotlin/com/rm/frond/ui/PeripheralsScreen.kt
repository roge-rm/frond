package com.rm.frond.ui

import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
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
import com.rm.frond.FatImage
import com.rm.frond.IrBeam
import com.rm.frond.NetworkMode
import com.rm.frond.PalmDatabases
import com.rm.frond.Peripheral
import com.rm.frond.PeripheralsModel
import com.rm.frond.SdCard

private val SIZES = listOf(32, 64, 128)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeripheralsScreen(model: PeripheralsModel, onClose: () -> Unit) {
    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }
    var choosingSize by remember { mutableStateOf(false) }
    var confirmEject by remember { mutableStateOf(false) }
    var confirmFiles by remember { mutableStateOf(false) }
    // What to do once the user agrees to lose the ejected card: "create" or "image".
    var confirmReplace by remember { mutableStateOf<String?>(null) }
    var naming by remember { mutableStateOf(false) }
    var exporting by remember { mutableStateOf<FatImage.Entry?>(null) }
    var confirmReset by remember { mutableStateOf(false) }
    var choosingNetwork by remember { mutableStateOf(false) }
    var choosingBeam by remember { mutableStateOf(false) }
    val saveCopy = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) model.saveCopy(uri)
    }

    val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.job("Inserting", "Couldn't insert the image") { it.insertImage(uri) }
    }
    val saveImage = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) model.job("Copying", "Couldn't export the image") {
            it.exportImage(uri)
            model.note = "Card image exported"
        }
    }
    val pickApp = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.install(uri)
    }
    // The database to export once a document is picked; null exports all RAM databases.
    var exportName by rememberSaveable { mutableStateOf<String?>(null) }
    val saveZip = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) model.export(uri, exportName)
    }
    val pickFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.addFile(uri)
    }
    val saveFile = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val entry = exporting
        exporting = null
        if (uri != null && entry != null) model.exportFile(entry, uri)
    }

    LaunchedEffect(model.note) {
        model.note?.let {
            snackbar.showSnackbar(it)
            model.note = null
        }
    }

    val path = model.path
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(if (path == null) model.device.label else "SD card $path", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = { if (path == null) onClose() else if (path == "/") model.leaveFiles() else model.up() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            if (path != null) {
                Surface(tonalElevation = 2.dp) {
                    Row(
                        Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        val pad = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
                        OutlinedButton(onClick = { naming = true }, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text("New folder", maxLines = 1, softWrap = false)
                        }
                        OutlinedButton(onClick = { pickFile.launch(arrayOf("*/*")) }, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text("Add file", maxLines = 1, softWrap = false)
                        }
                        Spacer(Modifier.weight(1f))
                        Button(onClick = { model.insertEdited() }, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text("Insert", maxLines = 1, softWrap = false)
                        }
                    }
                }
            }
        },
    ) { padding ->
        if (path != null) {
            LazyColumn(Modifier.padding(padding).fillMaxSize()) {
                item {
                    Text(
                        "${Formatter.formatShortFileSize(context, model.free)} free" + if (model.changed) " · Changed" else "",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                    )
                }
                if (model.entries.isEmpty()) item { EmptyNote("Empty folder") }
                items(model.entries, key = { it.shortName }) { e ->
                    ListItem(
                        headlineContent = { Text(e.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        supportingContent = { Text(if (e.directory) "Folder" else Formatter.formatShortFileSize(context, e.size)) },
                        leadingContent = {
                            Marker(if (e.directory) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.outlineVariant)
                        },
                        modifier = Modifier.clickableRow {
                            if (e.directory) model.open(SdCard.joined(path, e.name))
                            else {
                                exporting = e
                                saveFile.launch(e.name)
                            }
                        },
                    )
                }
                item { Spacer(Modifier.size(16.dp)) }
            }
        } else {
            LazyColumn(Modifier.padding(padding).fillMaxSize()) {
                fun action(label: String, onClick: () -> Unit) = item(key = label) {
                    ListItem(headlineContent = { Text(label) }, modifier = Modifier.clickableRow(onClick = onClick))
                }
                item { Header("Applications", Modifier.padding(horizontal = 16.dp)) }
                action("Install application") { pickApp.launch(arrayOf("*/*")) }
                action("Launch application") { model.askName("launch") }
                action("Export RAM databases") {
                    exportName = null
                    saveZip.launch("Palm databases.zip")
                }
                action("Export database") { model.askName("export") }
                item { Header("SD card", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                item {
                    val (label, colour) = when (model.state) {
                        SdCard.State.INSERTED -> "Inserted" to MaterialTheme.colorScheme.primary
                        SdCard.State.EJECTED -> "Ejected" to MaterialTheme.colorScheme.secondary
                        SdCard.State.NONE -> "No card" to MaterialTheme.colorScheme.outlineVariant
                        SdCard.State.UNKNOWN -> "Not known" to MaterialTheme.colorScheme.error
                    }
                    ListItem(headlineContent = { Text(label) }, leadingContent = { Marker(colour) })
                }
                when (model.state) {
                    SdCard.State.INSERTED -> {
                        action("Files") { confirmFiles = true }
                        action("Eject") { confirmEject = true }
                    }
                    SdCard.State.EJECTED -> {
                        action("Insert") { model.job("Inserting", "Couldn't insert the card") { it.reinsert() } }
                        action("Files") { model.openFiles() }
                        action("Export image") { saveImage.launch("SD card.img") }
                        action("New card") { confirmReplace = "create" }
                        action("Insert image") { confirmReplace = "image" }
                    }
                    SdCard.State.NONE -> {
                        action("New card") { choosingSize = true }
                        action("Insert image") { pickImage.launch(arrayOf("*/*")) }
                    }
                    SdCard.State.UNKNOWN -> action("Try again") { model.refresh() }
                }
                item { Header("Device", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                item(key = "keyboard") {
                    val attached = model.keyboard
                    // The keyboard and beaming share the Palm's infrared port.
                    val usable = attached != null && model.beam.phase == IrBeam.Phase.OFF
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
                    val usable = model.keyboard == false
                    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                    ListItem(
                        headlineContent = { Text("Infrared beaming", color = if (usable) Color.Unspecified else faded) },
                        supportingContent = { Text(beamText(beam), color = if (usable) Color.Unspecified else faded) },
                        modifier = if (usable) Modifier.clickableRow { choosingBeam = true } else Modifier,
                    )
                }
                if (model.beam.phase == IrBeam.Phase.DISCONNECTED) action("Connect again") { model.retryBeam() }
                item(key = "network") {
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
                action("Rotate") { model.rotate() }
                item { Header("Session", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                action("Save a copy") { saveCopy.launch("${model.name}.frond") }
                action("Emulator log") { model.showLog() }
                item { Header("Not on Android yet", Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp)) }
                items(Peripheral.entries) { p ->
                    ListItem(
                        headlineContent = {
                            Text(p.label, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f))
                        },
                    )
                }
                item { Spacer(Modifier.size(16.dp)) }
            }
        }
    }

    model.busy?.let { label ->
        AlertDialog(onDismissRequest = {}, confirmButton = {}, title = { Text(label) }, text = { Progress() })
    }
    model.alert?.let { (title, text) ->
        AlertDialog(
            onDismissRequest = { model.alert = null },
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { model.alert = null }) { Text("OK") } },
        )
    }
    if (choosingNetwork) {
        AlertDialog(
            onDismissRequest = { choosingNetwork = false },
            title = { Text("Network") },
            text = {
                LazyColumn {
                    items(NetworkMode.entries) { mode ->
                        ListItem(
                            headlineContent = { Text(mode.label) },
                            leadingContent = { RadioButton(selected = mode == model.network, onClick = null) },
                            modifier = Modifier.clickableRow {
                                choosingNetwork = false
                                if (mode != model.network) model.changeNetwork(mode)
                            },
                        )
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { choosingNetwork = false }) { Text("Cancel") } },
        )
    }
    if (choosingBeam) {
        BeamDialog(
            current = model.beam.mode,
            last = remember { IrBeam.last(context) },
            addresses = produceState<List<String>?>(null) { value = withContext(Dispatchers.IO) { IrBeam.addresses(context) } }.value,
            onDismiss = { choosingBeam = false },
            onChoose = { mode ->
                choosingBeam = false
                model.changeBeam(mode)
            },
        )
    }
    if (choosingSize) {
        AlertDialog(
            onDismissRequest = { choosingSize = false },
            title = { Text("New card") },
            text = {
                LazyColumn {
                    items(SIZES) { mib ->
                        ListItem(headlineContent = { Text("$mib MB") }, modifier = Modifier.clickableRow {
                            choosingSize = false
                            model.job("Making the card", "Couldn't make the card") { it.create(mib) }
                        })
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { choosingSize = false }) { Text("Cancel") } },
        )
    }
    if (confirmEject || confirmFiles) {
        val files = confirmFiles
        AlertDialog(
            onDismissRequest = { confirmEject = false; confirmFiles = false },
            title = { Text("Eject the card?") },
            text = { Text(if (files) "Close any files open on the Palm first. Frond saves a recovery session before the exchange." else "Close any files open on the Palm first.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmEject = false
                    confirmFiles = false
                    if (files) model.openFiles() else model.job("Ejecting", "Couldn't eject the card") { it.eject() }
                }) { Text("Eject") }
            },
            dismissButton = { TextButton(onClick = { confirmEject = false; confirmFiles = false }) { Text("Cancel") } },
        )
    }
    confirmReplace?.let { what ->
        AlertDialog(
            onDismissRequest = { confirmReplace = null },
            title = { Text("Replace the ejected card?") },
            text = { Text("Its files are lost unless you export the image first.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmReplace = null
                    if (what == "create") choosingSize = true else pickImage.launch(arrayOf("*/*"))
                }) { Text("Replace") }
            },
            dismissButton = { TextButton(onClick = { confirmReplace = null }) { Text("Cancel") } },
        )
    }
    if (confirmReset) {
        AlertDialog(
            onDismissRequest = { confirmReset = false },
            title = { Text("Reset the ${model.device.label} now?") },
            confirmButton = {
                TextButton(onClick = { confirmReset = false; model.reset() }) { Text("Reset") }
            },
            dismissButton = { TextButton(onClick = { confirmReset = false }) { Text("Cancel") } },
        )
    }
    if (model.askingLeave) {
        AlertDialog(
            onDismissRequest = { model.askingLeave = false },
            title = { Text("Insert the changed card?") },
            confirmButton = {
                TextButton(onClick = { model.askingLeave = false; model.insertEdited() }) { Text("Insert") }
            },
            dismissButton = {
                TextButton(onClick = { model.askingLeave = false; model.discard() }) { Text("Discard changes") }
            },
        )
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
    if (naming) {
        var name by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { naming = false },
            title = { Text("New folder") },
            text = {
                val focus = remember { FocusRequester() }
                OutlinedTextField(name, { name = it }, singleLine = true, modifier = Modifier.fillMaxWidth().focusRequester(focus))
                LaunchedEffect(Unit) { focus.requestFocus() }
            },
            confirmButton = {
                TextButton(enabled = name.isNotBlank(), onClick = {
                    naming = false
                    model.makeFolder(name)
                }) { Text("Make") }
            },
            dismissButton = { TextButton(onClick = { naming = false }) { Text("Cancel") } },
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

private fun beamText(s: IrBeam.Status): String = when (s.phase) {
    IrBeam.Phase.OFF -> s.error?.let { "Off. $it" } ?: "Off"
    IrBeam.Phase.WAITING -> "Waiting for a peer on port ${(s.mode as? BeamMode.Listen)?.port}"
    IrBeam.Phase.CONNECTING -> "Connecting to ${(s.mode as? BeamMode.Connect)?.let { IrBeam.hostPort(it.host, it.port) }}"
    IrBeam.Phase.CONNECTED -> "Connected to ${s.peer ?: "a peer"}"
    IrBeam.Phase.DISCONNECTED -> s.error ?: "Disconnected"
}

// Off, wait for a peer on a port, or connect to another Frond's address.
@Composable
private fun BeamDialog(
    current: BeamMode,
    last: BeamMode?,
    addresses: List<String>?,
    onDismiss: () -> Unit,
    onChoose: (BeamMode) -> Unit,
) {
    val start = if (current != BeamMode.Off) current else last ?: BeamMode.Off
    var choice by remember { mutableStateOf(if (current == BeamMode.Off) "off" else start.encode().substringBefore(':')) }
    var port by remember { mutableStateOf(((start as? BeamMode.Listen)?.port ?: IrBeam.DEFAULT_PORT).toString()) }
    var address by remember {
        mutableStateOf((last as? BeamMode.Connect ?: start as? BeamMode.Connect)?.let { IrBeam.hostPort(it.host, it.port) } ?: "")
    }
    val mode: BeamMode? = when (choice) {
        "listen" -> IrBeam.port(port)?.let { BeamMode.Listen(it) }
        "connect" -> IrBeam.address(address)?.let { BeamMode.Connect(it.first, it.second) }
        else -> BeamMode.Off
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Infrared beaming") },
        text = {
            Column {
                for ((id, label) in listOf("off" to "Off", "listen" to "Wait for a peer", "connect" to "Connect to another Frond")) {
                    ListItem(
                        headlineContent = { Text(label) },
                        leadingContent = { RadioButton(selected = choice == id, onClick = null) },
                        modifier = Modifier.clickableRow { choice = id },
                    )
                }
                if (choice == "listen") {
                    OutlinedTextField(
                        port,
                        { port = it.filter(Char::isDigit).take(5) },
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
                if (choice == "connect") {
                    OutlinedTextField(
                        address,
                        { address = it.trim() },
                        label = { Text("Address and port") },
                        placeholder = { Text("192.168.1.20:${IrBeam.DEFAULT_PORT}") },
                        singleLine = true,
                        isError = address.isNotEmpty() && mode == null,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        },
        confirmButton = { TextButton(enabled = mode != null, onClick = { mode?.let(onChoose) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
