package com.rm.frond.ui

import android.net.Uri
import android.text.format.DateFormat
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.CardFiles
import com.rm.frond.FatImage
import com.rm.frond.FileList
import com.rm.frond.FilesModel
import com.rm.frond.SdCard
import java.util.Date

// Entries cut or copied, waiting for Paste.
private class Clip(val paths: List<String>, val move: Boolean)

// The card's file view: full screen. Long-press selects; the bottom bar then has Delete, Cut, Copy,
// Rename and Export. Works for the running machine's card and for a card opened from Cards.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CardFilesScreen(model: FilesModel, snackbar: SnackbarHostState) {
    val context = LocalContext.current
    val path = model.path ?: return
    var naming by remember { mutableStateOf(false) }
    var exporting by remember { mutableStateOf<FatImage.Entry?>(null) }
    var deleting by remember { mutableStateOf<List<FatImage.Entry>>(emptyList()) }
    var renaming by remember { mutableStateOf<FatImage.Entry?>(null) }
    var sorting by remember { mutableStateOf(false) }
    var sort by rememberSaveable { mutableStateOf(FileList.Sort.NAME) }
    var selected by remember(path) { mutableStateOf(emptySet<String>()) }
    var clip by remember { mutableStateOf<Clip?>(null) }
    val ready = model.busy == null
    val writable = ready && model.canWrite
    val stick = model.memoryStick

    val pickFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) model.addFiles(uris)
    }
    val pickTree = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) model.addFolder(uri)
    }
    var copyOut by remember { mutableStateOf<List<FatImage.Entry>>(emptyList()) }
    val pickTarget = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        val list = copyOut
        copyOut = emptyList()
        if (uri != null && list.isNotEmpty()) {
            selected = emptySet()
            model.exportEntries(list, uri)
        }
    }
    val saveFile = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val entry = exporting
        exporting = null
        if (uri != null && entry != null) model.exportFile(entry, uri)
    }

    val shown = FileList.sorted(model.entries, sort)
    val picked = shown.filter { it.name in selected }
    val selecting = picked.isNotEmpty()
    fun fullPath(e: FatImage.Entry) = SdCard.joined(path, e.name)

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(if (selecting) "${picked.size} selected" else "${model.title} $path", maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
                navigationIcon = {
                    if (selecting) IconButton(onClick = { selected = emptySet() }) { Icon(Icons.Default.Close, contentDescription = "Clear selection") }
                    else IconButton(onClick = { if (path == "/") model.leaveFiles() else model.up() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    if (shown.isNotEmpty()) TextButton(onClick = { selected = shown.map { it.name }.toSet() }) { Text("Select all") }
                    Box {
                        IconButton(onClick = { sorting = true }) { Icon(Icons.Default.MoreVert, contentDescription = "Sort") }
                        DropdownMenu(expanded = sorting, onDismissRequest = { sorting = false }) {
                            for (s in FileList.Sort.entries) {
                                DropdownMenuItem(
                                    text = { Text((if (s == sort) "✓ " else "") + "Sort by ${s.label.lowercase()}") },
                                    onClick = { sorting = false; sort = s },
                                )
                            }
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            Surface(tonalElevation = 2.dp) {
                Row(
                    Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    val pad = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
                    Row(Modifier.weight(1f).horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (selecting) {
                            OutlinedButton(onClick = { deleting = picked }, enabled = writable, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                                Text("Delete", maxLines = 1, softWrap = false, color = MaterialTheme.colorScheme.error)
                            }
                            if (!stick) {
                                OutlinedButton(onClick = { clip = Clip(picked.map { fullPath(it) }, true); selected = emptySet() }, enabled = writable,
                                    shape = MaterialTheme.shapes.small, contentPadding = pad) { Text("Cut", maxLines = 1, softWrap = false) }
                                OutlinedButton(onClick = { clip = Clip(picked.map { fullPath(it) }, false); selected = emptySet() }, enabled = ready,
                                    shape = MaterialTheme.shapes.small, contentPadding = pad) { Text("Copy", maxLines = 1, softWrap = false) }
                            }
                            if (picked.size == 1) OutlinedButton(onClick = { renaming = picked.first() }, enabled = writable,
                                shape = MaterialTheme.shapes.small, contentPadding = pad) { Text("Rename", maxLines = 1, softWrap = false) }
                            OutlinedButton(onClick = { copyOut = picked; pickTarget.launch(null) },
                                enabled = ready && !(stick && picked.any { it.directory }),
                                shape = MaterialTheme.shapes.small, contentPadding = pad) { Text("Export", maxLines = 1, softWrap = false) }
                        } else {
                            clip?.let { c ->
                                OutlinedButton(onClick = { model.paste(c.paths, c.move); clip = null }, enabled = writable,
                                    shape = MaterialTheme.shapes.small, contentPadding = pad) {
                                    Text("Paste ${c.paths.size}", maxLines = 1, softWrap = false)
                                }
                            }
                            OutlinedButton(onClick = { naming = true }, enabled = writable, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                                Text("New folder", maxLines = 1, softWrap = false)
                            }
                            OutlinedButton(onClick = { pickFiles.launch(arrayOf("*/*")) }, enabled = writable, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                                Text("Add files", maxLines = 1, softWrap = false)
                            }
                            if (!stick) OutlinedButton(onClick = { pickTree.launch(null) }, enabled = writable, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                                Text("Add folder", maxLines = 1, softWrap = false)
                            }
                        }
                    }
                    Button(onClick = { model.insertEdited() }, enabled = ready, shape = MaterialTheme.shapes.small, contentPadding = pad) {
                        Text(model.leaveLabel, maxLines = 1, softWrap = false)
                    }
                }
            }
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            item {
                val size = Formatter.formatShortFileSize(context, model.total)
                Text(
                    "${Formatter.formatShortFileSize(context, model.free)} free of $size" + if (model.changed) " · Changed" else "",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
            if (model.shortcuts.isNotEmpty()) item {
                Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 8.dp)) {
                    for (s in model.shortcuts) TextButton(onClick = { model.open(s) }, enabled = ready) { Text(s, maxLines = 1, softWrap = false) }
                }
            }
            if (shown.isEmpty()) item { EmptyNote("Empty folder") }
            items(shown, key = { it.shortName }) { e ->
                val on = e.name in selected
                ListItem(
                    headlineContent = { Text(e.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    supportingContent = { Text(detail(context, e)) },
                    leadingContent = {
                        Marker(if (on) MaterialTheme.colorScheme.primary
                        else if (e.directory) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.outlineVariant)
                    },
                    trailingContent = if (on) ({ Text("✓", color = MaterialTheme.colorScheme.primary) }) else null,
                    colors = ListItemDefaults.colors(containerColor = if (on) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surface),
                    modifier = Modifier.clickableRow(onLongClick = { if (ready) selected = selected + e.name }) {
                        when {
                            !ready -> Unit
                            selecting -> selected = if (on) selected - e.name else selected + e.name
                            e.directory -> model.open(SdCard.joined(path, e.name))
                            else -> { exporting = e; saveFile.launch(e.name) }
                        }
                    },
                )
            }
            item { Spacer(Modifier.size(16.dp)) }
        }
    }

    renaming?.let { entry ->
        var name by remember(entry) { mutableStateOf(entry.name) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Rename") },
            text = { OutlinedTextField(name, { name = it }, singleLine = true, modifier = Modifier.fillMaxWidth()) },
            confirmButton = {
                TextButton(enabled = name.isNotBlank(), onClick = { renaming = null; selected = emptySet(); model.renameEntry(entry, name) }) { Text("Rename") }
            },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }
    if (deleting.isNotEmpty()) {
        val list = deleting
        AlertDialog(
            onDismissRequest = { deleting = emptyList() },
            title = { Text(if (list.size == 1) "Delete ${list.first().name}?" else "Delete ${list.size} items?") },
            text = if (list.any { it.directory }) ({ Text("Folders go with everything in them.") }) else null,
            confirmButton = {
                TextButton(onClick = { deleting = emptyList(); selected = emptySet(); model.deleteEntries(list) }) {
                    Text("Delete", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { deleting = emptyList() }) { Text("Cancel") } },
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
    model.prompt.asking?.let { ask ->
        var all by remember(ask) { mutableStateOf(false) }
        fun answer(c: CardFiles.Clash) = model.prompt.answer(CardFiles.Decision(c, all))
        AlertDialog(
            onDismissRequest = {},
            title = { Text("${ask.name} is already here") },
            text = {
                if (ask.several) Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(all, { all = it })
                    Text("For all")
                }
            },
            confirmButton = {
                Row {
                    TextButton(onClick = { answer(CardFiles.Clash.REPLACE) }) { Text("Replace") }
                    TextButton(onClick = { answer(CardFiles.Clash.SKIP) }) { Text("Skip") }
                    TextButton(onClick = { answer(CardFiles.Clash.KEEP_BOTH) }) { Text("Keep both") }
                }
            },
            dismissButton = { TextButton(onClick = { answer(CardFiles.Clash.CANCEL) }) { Text("Cancel") } },
        )
    }
}

// Size and date under a name; a Memory Stick doesn't give a date.
private fun detail(context: android.content.Context, e: FatImage.Entry): String {
    val size = if (e.directory) "Folder" else Formatter.formatShortFileSize(context, e.size)
    if (e.modified == 0L) return size
    return size + " · " + DateFormat.getMediumDateFormat(context).format(Date(e.modified))
}
