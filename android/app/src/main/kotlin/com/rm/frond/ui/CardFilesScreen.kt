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
import com.rm.frond.RunningModel
import com.rm.frond.SdCard
import com.rm.frond.SerialLink
import com.rm.frond.UsbLink
// The card's file view: full screen, with the card ejected while it's open. snackbar shows the model's notes where the caller wants them.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CardFilesScreen(model: RunningModel, snackbar: SnackbarHostState) {
    val context = LocalContext.current
    var naming by remember { mutableStateOf(false) }
    var exporting by remember { mutableStateOf<FatImage.Entry?>(null) }
    var renaming by remember { mutableStateOf<FatImage.Entry?>(null) }
    var deleting by remember { mutableStateOf<FatImage.Entry?>(null) }
    val pickFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.addFile(uri)
    }
    val saveFile = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val entry = exporting
        exporting = null
        if (uri != null && entry != null) model.exportFile(entry, uri)
    }
    val path = model.path ?: return
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("${model.device.cardLabel} $path", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = { if (path == "/") model.leaveFiles() else model.up() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            run {
                Surface(tonalElevation = 2.dp) {
                    Row(
                        Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        val pad = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
                        OutlinedButton(onClick = { naming = true }, enabled = !model.device.memoryStick || model.writeProtected == false, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text("New folder", maxLines = 1, softWrap = false)
                        }
                        OutlinedButton(onClick = { pickFile.launch(arrayOf("*/*")) }, enabled = !model.device.memoryStick || model.writeProtected == false, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text("Add file", maxLines = 1, softWrap = false)
                        }
                        Spacer(Modifier.weight(1f))
                        Button(onClick = { model.insertEdited() }, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                            Text(if (model.device.memoryStick) "Done" else "Insert", maxLines = 1, softWrap = false)
                        }
                    }
                }
            }
        },
    ) { padding ->
        run {
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
                    var menu by remember { mutableStateOf(false) }
                    ListItem(
                        headlineContent = { Text(e.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        supportingContent = { Text(if (e.directory) "Folder" else Formatter.formatShortFileSize(context, e.size)) },
                        leadingContent = {
                            Marker(if (e.directory) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.outlineVariant)
                        },
                        trailingContent = if (model.device.memoryStick) ({
                            Box {
                                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                                    DropdownMenuItem(text = { Text("Rename") }, enabled = model.writeProtected == false && model.busy == null,
                                        onClick = { menu = false; renaming = e })
                                    DropdownMenuItem(text = { Text("Delete") }, enabled = model.writeProtected == false && model.busy == null,
                                        onClick = { menu = false; deleting = e })
                                }
                            }
                        }) else null,
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
        }
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
    renaming?.let { entry ->
        var name by remember(entry) { mutableStateOf(entry.name) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Rename card entry") },
            text = { OutlinedTextField(name, { name = it }, singleLine = true, modifier = Modifier.fillMaxWidth()) },
            confirmButton = { TextButton(onClick = { renaming = null; model.renameEntry(entry, name) }) { Text("Rename") } },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }
    deleting?.let { entry ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete ${entry.name}?") },
            confirmButton = { TextButton(onClick = { deleting = null; model.deleteEntry(entry) }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
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
