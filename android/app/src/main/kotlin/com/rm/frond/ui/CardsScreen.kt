package com.rm.frond.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import android.widget.Toast
import androidx.compose.material3.Surface
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.CardChoices
import com.rm.frond.Cards
import com.rm.frond.CardsModel
import com.rm.frond.HomeModel

// The drawer of SD cards and Memory Sticks: grouped rows with the marker lit while a card is in a machine.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CardsScreen(home: HomeModel, model: CardsModel) {
    val snackbar = remember { SnackbarHostState() }
    var menu by remember { mutableStateOf(false) }
    var making by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<CardChoices.Row?>(null) }
    var deleting by remember { mutableStateOf<CardChoices.Row?>(null) }
    var discarding by remember { mutableStateOf<CardChoices.Row?>(null) }
    var exporting by remember { mutableStateOf<CardChoices.Row?>(null) }
    val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.importImage(uri)
    }
    val pickFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) model.startFolder(uri)
    }
    val saveImage = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val row = exporting
        exporting = null
        if (uri != null && row != null) model.export(row, uri)
    }
    home.notice?.let { text ->
        LaunchedEffect(text) {
            home.notice = null
            snackbar.showSnackbar(text)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Cards") },
                navigationIcon = {
                    IconButton(onClick = { home.closeCards() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
                actions = {
                    Box {
                        IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            DropdownMenuItem(text = { Text("Import card image") }, enabled = !home.copying, onClick = {
                                menu = false
                                pickImage.launch(arrayOf("*/*"))
                            })
                            DropdownMenuItem(text = { Text("Import folder as card…") }, enabled = !home.copying, onClick = {
                                menu = false
                                pickFolder.launch(null)
                            })
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                text = { Text("New card") },
                icon = { Icon(Icons.Default.Add, contentDescription = null) },
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary,
                onClick = { if (!home.copying) making = true },
            )
        },
    ) { padding ->
        val rows = model.rows
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            if (rows == null) item(key = "loading") { Progress() }
            else for ((title, section) in CardChoices.sections(rows)) {
                item(key = "h$title") { Header(title, Modifier.padding(start = 16.dp, top = 8.dp)) }
                if (section.isEmpty()) item(key = "e$title") { EmptyNote(if (title == "SD cards") "No SD cards." else "No Memory Sticks.") }
                items(section, key = { it.card.id }) { row ->
                    CardRow(row, exchange = model.exchangeOpen(row) || home.copying, running = row.holderRunning,
                        onRename = { renaming = it }, onDuplicate = { model.duplicate(it) },
                        onExport = {
                            exporting = it
                            saveImage.launch(it.card.name + if (it.card.kind == "ms") ".frondms" else ".img")
                        },
                        onFiles = { model.files.start(it) },
                        onLink = { model.startLink(it) }, onTakeOut = { model.takeOut(it) }, onDelete = { deleting = it })
                }
            }
            item { Spacer(Modifier.size(80.dp)) }
        }
    }

    CopyingDialog(home)
    val files = model.files
    if (files.path != null) {
        val filesSnackbar = remember { SnackbarHostState() }
        val context = LocalContext.current
        LaunchedEffect(files.note) {
            files.note?.let {
                Toast.makeText(context, it, Toast.LENGTH_SHORT).show()
                files.note = null
            }
        }
        Dialog({ if (files.path == "/") files.leaveFiles() else files.up() }, DialogProperties(usePlatformDefaultWidth = false)) {
            Surface(Modifier.fillMaxSize()) { CardFilesScreen(files, filesSnackbar) }
        }
    }
    files.mismatched?.let { row ->
        AlertDialog(
            onDismissRequest = { files.mismatched = null },
            title = { Text("Files") },
            text = { Text(Cards.MISMATCH) },
            confirmButton = { TextButton(onClick = { files.mismatched = null; discarding = row }) { Text("Discard unfinished change") } },
            dismissButton = { TextButton(onClick = { files.mismatched = null }) { Text("Cancel") } },
        )
    }
    discarding?.let { row ->
        AlertDialog(
            onDismissRequest = { discarding = null },
            title = { Text("Discard unfinished change") },
            text = { Text("Discard the unfinished change? This can't be undone.") },
            confirmButton = { TextButton(onClick = { discarding = null; files.discard(row) }) { Text("Discard") } },
            dismissButton = { TextButton(onClick = { discarding = null }) { Text("Cancel") } },
        )
    }
    home.alert?.let { (title, text) ->
        AlertDialog(
            onDismissRequest = { home.alert = null },
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { home.alert = null }) { Text("OK") } },
        )
    }
    if (making) {
        NewCardDialog(model.library, null, onDismiss = { making = false }) { name, kind, size ->
            making = false
            model.make(name, kind, size)
        }
    }
    model.folder?.let { draft ->
        FolderCardDialog(model.library, draft, onDismiss = { model.folder = null }) { name, kind, size ->
            model.folder = null
            model.makeFromFolder(draft, name, kind, size)
        }
    }
    renaming?.let { row ->
        RenameCardDialog(row.card.name, onDismiss = { renaming = null }) { name ->
            renaming = null
            model.rename(row, name)
        }
    }
    deleting?.let { row ->
        val n = row.links.size
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete") },
            text = {
                Text(when (n) {
                    0 -> "Delete ${row.card.name}? Its files are lost."
                    1 -> "Delete ${row.card.name}? It is linked to 1 machine, and its files are lost."
                    else -> "Delete ${row.card.name}? It is linked to $n machines, and its files are lost."
                })
            },
            confirmButton = { TextButton(onClick = { deleting = null; model.delete(row) }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    model.linking?.let { (card, options) ->
        LinkCardsDialog(card.name, options, onDismiss = { model.linking = null }) { model.link(card, it) }
    }
    model.importing?.let { (_, kinds) ->
        ChoiceDialog("Import card image", kinds, { Cards.kindName(it) }, "Import", onDismiss = { model.importAs(null) }) { model.importAs(it) }
    }
}

@Composable
private fun CardRow(
    row: CardChoices.Row,
    // An exchange is open for its machine (or a job runs): the whole menu is greyed.
    exchange: Boolean,
    // Its machine runs: only Rename and Link to machines… work.
    running: Boolean,
    onRename: (CardChoices.Row) -> Unit,
    onDuplicate: (CardChoices.Row) -> Unit,
    onExport: (CardChoices.Row) -> Unit,
    onFiles: (CardChoices.Row) -> Unit,
    onLink: (CardChoices.Row) -> Unit,
    onTakeOut: (CardChoices.Row) -> Unit,
    onDelete: (CardChoices.Row) -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    ListItem(
        headlineContent = { Text(row.card.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text("${CardChoices.sizeText(row.card)} · ${CardChoices.statusText(row)}") },
        leadingContent = { Marker(if (row.inMachine) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant) },
        trailingContent = {
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    val stopped = !exchange && !running
                    // A classic Memory Stick can't be edited in place, so it has no Files….
                    if (row.card.kind != "ms") DropdownMenuItem(text = { Text("Files…") }, enabled = stopped,
                        trailingIcon = if (running) ({ Text("In use", style = MaterialTheme.typography.labelMedium) }) else null,
                        onClick = { menu = false; onFiles(row) })
                    DropdownMenuItem(text = { Text("Rename") }, enabled = !exchange, onClick = { menu = false; onRename(row) })
                    DropdownMenuItem(text = { Text("Duplicate") }, enabled = stopped, onClick = { menu = false; onDuplicate(row) })
                    DropdownMenuItem(text = { Text("Export image") }, enabled = stopped, onClick = { menu = false; onExport(row) })
                    DropdownMenuItem(text = { Text("Link to machines…") }, enabled = !exchange, onClick = { menu = false; onLink(row) })
                    row.holder?.let { holder ->
                        DropdownMenuItem(text = { Text("Take out of $holder") }, enabled = stopped, onClick = { menu = false; onTakeOut(row) })
                    }
                    DropdownMenuItem(text = { Text("Delete") }, enabled = stopped && row.inDrawer, onClick = { menu = false; onDelete(row) })
                }
            }
        },
    )
}

// The home screen's job dialog: its label, a bar once the job reports progress, and Cancel.
@Composable
fun CopyingDialog(home: HomeModel) {
    if (!home.copying) return
    AlertDialog(
        onDismissRequest = {},
        confirmButton = {},
        dismissButton = if (home.cancellable) ({ TextButton(onClick = { home.cancelCopy() }) { Text("Cancel") } }) else null,
        title = { Text(home.copyLabel) },
        text = {
            val done = home.copyProgress
            if (done == null) Progress()
            else androidx.compose.material3.LinearProgressIndicator(progress = { done },
                modifier = androidx.compose.ui.Modifier.padding(vertical = 16.dp))
        },
    )
}
