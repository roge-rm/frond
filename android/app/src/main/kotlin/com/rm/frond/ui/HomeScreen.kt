package com.rm.frond.ui

import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rm.frond.Device
import com.rm.frond.HomeModel
import com.rm.frond.Session
import com.rm.frond.Sessions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(model: HomeModel) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var menu by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<Session?>(null) }
    var deleting by remember { mutableStateOf<Session?>(null) }
    var exporting by remember { mutableStateOf<Session?>(null) }
    var choosingDevice by remember { mutableStateOf(false) }

    val importSession = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        busy = true
        scope.launch {
            val error = withContext(Dispatchers.IO) { Sessions.import(context, uri) }
            busy = false
            model.reload()
            if (error != null) model.alert = "Couldn't import the session" to error
            else snackbar.showSnackbar("Session imported")
        }
    }
    val exportSession = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val s = exporting
        exporting = null
        if (uri == null || s == null) return@rememberLauncherForActivityResult
        busy = true
        scope.launch {
            val error = withContext(Dispatchers.IO) { Sessions.export(context, s, uri) }
            busy = false
            if (error != null) model.alert = "Couldn't export the session" to error
            else snackbar.showSnackbar("Session exported")
        }
    }

    val ready = Device.shown.filter { model.firmwareReady[it] == true }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Frond") },
                actions = {
                    Box {
                        IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            DropdownMenuItem(text = { Text("Import session") }, onClick = {
                                menu = false
                                importSession.launch(arrayOf("*/*"))
                            })
                            DropdownMenuItem(text = { Text("Settings") }, onClick = {
                                menu = false
                                model.settingsOpen = true
                            })
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        floatingActionButton = {
            if (ready.isNotEmpty()) {
                FloatingActionButton(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                    onClick = { if (ready.size == 1) model.newSession(ready[0]) else choosingDevice = true },
                ) {
                    Icon(Icons.Default.Add, contentDescription = "New session")
                }
            }
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            for (device in Device.shown) {
                deviceSection(model, device, onRename = { renaming = it }, onDelete = { deleting = it }, onExport = {
                    exporting = it
                    exportSession.launch("${it.name}.frond")
                })
            }
            item { Spacer(Modifier.size(80.dp)) }
        }
    }

    if (busy) {
        AlertDialog(onDismissRequest = {}, confirmButton = {}, title = { Text("Copying") }, text = { Progress() })
    }
    model.alert?.let { (title, text) ->
        AlertDialog(
            onDismissRequest = { model.alert = null },
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { model.alert = null }) { Text("OK") } },
        )
    }
    renaming?.let { s ->
        // Starts with the whole name selected, so typing replaces it.
        var name by remember(s.id) { mutableStateOf(TextFieldValue(s.name, TextRange(0, s.name.length))) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Rename") },
            text = {
                val focus = remember { FocusRequester() }
                OutlinedTextField(name, { name = it }, singleLine = true, modifier = Modifier.fillMaxWidth().focusRequester(focus))
                LaunchedEffect(Unit) { focus.requestFocus() }
            },
            confirmButton = {
                TextButton(onClick = {
                    Sessions.rename(context, s, name.text)
                    renaming = null
                    model.reload()
                }) { Text("Rename") }
            },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }
    deleting?.let { s ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete ${s.name}?") },
            confirmButton = {
                TextButton(onClick = {
                    Sessions.delete(context, s)
                    deleting = null
                    model.reload()
                }) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    if (choosingDevice) {
        AlertDialog(
            onDismissRequest = { choosingDevice = false },
            title = { Text("New session") },
            text = {
                LazyColumn {
                    items(ready) { d ->
                        ListItem(headlineContent = { Text(d.label) }, modifier = Modifier.clickableRow {
                            choosingDevice = false
                            model.newSession(d)
                        })
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { choosingDevice = false }) { Text("Cancel") } },
        )
    }
}

private fun LazyListScope.deviceSection(
    model: HomeModel,
    device: Device,
    onRename: (Session) -> Unit,
    onDelete: (Session) -> Unit,
    onExport: (Session) -> Unit,
) {
    item(key = "h" + device.id) {
        Row(
            Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Header(device.label, Modifier.weight(1f))
            TextButton(onClick = { model.firmwareOpen = true }) { Text("Firmware") }
        }
    }
    if (model.firmwareReady[device] != true) {
        item(key = "f" + device.id) {
            ListItem(
                headlineContent = { Text("No firmware") },
                leadingContent = { Marker(MaterialTheme.colorScheme.error) },
                trailingContent = { TextButton(onClick = { model.firmwareOpen = true }) { Text("Import") } },
            )
        }
    }
    val list = model.sessions.filter { it.device == device }
    items(list, key = { it.id }) { s -> SessionRow(s, onOpen = { model.start(s) }, onRename, onDelete, onExport) }
    if (list.isEmpty() && model.firmwareReady[device] == true) {
        item(key = "e" + device.id) { EmptyNote("No sessions") }
    }
}

@Composable
private fun SessionRow(
    s: Session,
    onOpen: () -> Unit,
    onRename: (Session) -> Unit,
    onDelete: (Session) -> Unit,
    onExport: (Session) -> Unit,
) {
    val context = LocalContext.current
    var menu by remember { mutableStateOf(false) }
    val detail = if (s.saved) {
        val at = DateUtils.formatDateTime(
            context, s.savedAt,
            DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME or DateUtils.FORMAT_ABBREV_MONTH,
        )
        "Saved $at · ${Formatter.formatShortFileSize(context, s.size)}"
    } else "Not saved"
    ListItem(
        headlineContent = { Text(s.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(detail, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { Marker(if (s.saved) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant) },
        trailingContent = {
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("Rename") }, onClick = { menu = false; onRename(s) })
                    DropdownMenuItem(text = { Text("Export") }, enabled = s.saved, onClick = { menu = false; onExport(s) })
                    DropdownMenuItem(text = { Text("Delete") }, onClick = { menu = false; onDelete(s) })
                }
            }
        },
        modifier = Modifier.clickableRow(onClick = onOpen, onLongClick = { onRename(s) }),
    )
}

// A thin upright bar at the start of a row, like Whelk's group colour.
@Composable
fun Marker(colour: androidx.compose.ui.graphics.Color) {
    Box(Modifier.size(width = 4.dp, height = 40.dp).background(colour))
}

@Composable
fun Header(text: String, modifier: Modifier = Modifier) {
    Text(
        text.uppercase(),
        style = MaterialTheme.typography.labelLarge.copy(letterSpacing = 1.5.sp),
        color = MaterialTheme.colorScheme.primary,
        modifier = modifier.padding(vertical = 8.dp),
    )
}

@Composable
fun EmptyNote(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(16.dp),
    )
}

@Composable
fun Progress() {
    Box(Modifier.fillMaxWidth().padding(8.dp), contentAlignment = Alignment.Center) {
        androidx.compose.material3.CircularProgressIndicator()
    }
}

@OptIn(ExperimentalFoundationApi::class)
fun Modifier.clickableRow(onLongClick: (() -> Unit)? = null, onClick: () -> Unit): Modifier =
    this.combinedClickable(onClick = onClick, onLongClick = onLongClick)
