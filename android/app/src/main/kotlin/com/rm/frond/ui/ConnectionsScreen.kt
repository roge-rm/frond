package com.rm.frond.ui

import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
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
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.ConnectionsModel
import com.rm.frond.HomeModel
import com.rm.frond.LinkWire
import com.rm.frond.Links

// Connections: this Frond's name, the paired installs with the marker lit while linked, and the Fronds
// nearby to pair with. HomeActivity has the link engine watch while the screen is in front.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConnectionsScreen(home: HomeModel, model: ConnectionsModel) {
    val context = LocalContext.current
    // Android 17 asks before Frond reaches the local network.
    val askNetwork = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    LaunchedEffect(Unit) {
        if (Build.VERSION.SDK_INT >= 37 && Links.needsPermission(context)) askNetwork.launch(Links.LOCAL_NETWORK)
    }
    val snackbar = remember { SnackbarHostState() }
    val notice = model.notice
    LaunchedEffect(notice) {
        if (notice == null) return@LaunchedEffect
        try {
            snackbar.showSnackbar(notice)
        } finally {
            if (model.notice == notice) model.notice = null
        }
    }
    var menu by remember { mutableStateOf(false) }
    var choosing by remember { mutableStateOf(false) }
    var adding by remember { mutableStateOf(false) }
    var renamingSelf by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<LinkWire.ConnectionRow?>(null) }
    var addressing by remember { mutableStateOf<LinkWire.ConnectionRow?>(null) }
    var forgetting by remember { mutableStateOf<LinkWire.ConnectionRow?>(null) }
    var radioFor by remember { mutableStateOf<LinkWire.ConnectionRow?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Connections") },
                navigationIcon = {
                    IconButton(onClick = { home.closeConnections() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
                actions = {
                    Box {
                        IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            DropdownMenuItem(text = { Text("Add by address") }, onClick = { menu = false; adding = true })
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                text = { Text("Pair") },
                icon = { Icon(Icons.Default.Add, contentDescription = null) },
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary,
                onClick = { choosing = true },
            )
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            item(key = "self") {
                ListItem(
                    headlineContent = { Text("This Frond: ${model.name}", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    trailingContent = { TextButton(onClick = { renamingSelf = true }, enabled = model.loaded) { Text("Rename") } },
                )
            }
            item(key = "hc") { Header("Connections", Modifier.padding(start = 16.dp, top = 8.dp)) }
            if (!model.loaded) item(key = "loading") { Progress() }
            else if (model.rows.isEmpty()) item(key = "ec") { EmptyNote("No connections.") }
            items(model.rows, key = { "c" + it.id }) { row ->
                ConnectionRow(row, onRename = { renaming = it }, onAddress = { addressing = it }, onRadio = { radioFor = it },
                    onForget = { forgetting = it })
            }
            item(key = "hn") { Header("Nearby", Modifier.padding(start = 16.dp, top = 16.dp)) }
            if (model.loaded && model.nearby.isEmpty()) item(key = "en") { EmptyNote("No other Frond found") }
            items(model.nearby, key = { "n" + it.id }) { n ->
                ListItem(
                    headlineContent = { Text(n.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    supportingContent = { Text(n.address) },
                    leadingContent = { Marker(MaterialTheme.colorScheme.outlineVariant) },
                    trailingContent = { TextButton(onClick = { model.pairNearby(n) }) { Text("Pair") } },
                    modifier = Modifier.clickableRow { model.pairNearby(n) },
                )
            }
            item { Spacer(Modifier.size(80.dp)) }
        }
    }

    if (choosing) {
        AlertDialog(
            onDismissRequest = { choosing = false },
            title = { Text("Pair") },
            text = {
                Column {
                    if (model.nearby.isEmpty()) EmptyNote("No other Frond found")
                    for (n in model.nearby) {
                        ListItem(
                            headlineContent = { Text(n.name) },
                            supportingContent = { Text(n.address) },
                            modifier = Modifier.clickableRow { choosing = false; model.pairNearby(n) },
                        )
                    }
                }
            },
            confirmButton = { TextButton(onClick = { choosing = false; adding = true }) { Text("Add by address") } },
            dismissButton = { TextButton(onClick = { choosing = false }) { Text("Cancel") } },
        )
    }
    if (adding) {
        TextDialog("Add by address", "Address", "", placeholder = "192.168.1.20", confirm = "Pair", onDismiss = { adding = false }) {
            adding = false
            model.pairAddress(it)
        }
    }
    if (renamingSelf) {
        TextDialog("Rename", "Name", model.name, onDismiss = { renamingSelf = false }) {
            renamingSelf = false
            model.rename(it)
        }
    }
    renaming?.let { row ->
        TextDialog("Rename", "Name", row.name, onDismiss = { renaming = null }) {
            renaming = null
            model.renamePeer(row.id, it)
        }
    }
    addressing?.let { row ->
        TextDialog("Address", "Address", row.address, placeholder = "192.168.1.20", allowEmpty = true, onDismiss = { addressing = null }) {
            addressing = null
            model.setAddress(row.id, it)
        }
    }
    // Bluetooth on this connection uses one side's radio.
    radioFor?.let { row ->
        AlertDialog(
            onDismissRequest = { radioFor = null },
            title = { Text("Bluetooth radio") },
            text = {
                Column {
                    for ((value, label) in listOf("here" to "This Frond's radio", "there" to "${row.name}'s radio")) {
                        ListItem(
                            headlineContent = { Text(label) },
                            leadingContent = { RadioButton(selected = row.bluetoothRadio == value, onClick = null) },
                            modifier = Modifier.clickableRow { radioFor = null; if (row.bluetoothRadio != value) model.setRadio(row.id, value) },
                        )
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { radioFor = null }) { Text("Cancel") } },
        )
    }
    forgetting?.let { row ->
        AlertDialog(
            onDismissRequest = { forgetting = null },
            title = { Text("Forget") },
            text = { Text("Forget ${row.name}?") },
            confirmButton = { TextButton(onClick = { forgetting = null; model.forget(row.id) }) { Text("Forget") } },
            dismissButton = { TextButton(onClick = { forgetting = null }) { Text("Cancel") } },
        )
    }
    model.pairing?.let { PairCodeDialog(it, onPair = { code -> model.enterCode(code) }, onCancel = { model.cancelPairing() }) }
    model.request?.let { (who, code) ->
        AlertDialog(
            onDismissRequest = {},
            title = { Text("Pair") },
            text = { Text("$who wants to pair. Code $code", style = MaterialTheme.typography.headlineSmall) },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { model.dismissRequest() }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun ConnectionRow(
    row: LinkWire.ConnectionRow,
    onRename: (LinkWire.ConnectionRow) -> Unit,
    onAddress: (LinkWire.ConnectionRow) -> Unit,
    onRadio: (LinkWire.ConnectionRow) -> Unit,
    onForget: (LinkWire.ConnectionRow) -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    ListItem(
        headlineContent = { Text(row.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(LinkWire.connectionStatus(row)) },
        leadingContent = { Marker(if (row.up) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant) },
        trailingContent = {
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("Rename…") }, onClick = { menu = false; onRename(row) })
                    DropdownMenuItem(text = { Text("Address…") }, onClick = { menu = false; onAddress(row) })
                    DropdownMenuItem(text = { Text("Bluetooth radio") }, onClick = { menu = false; onRadio(row) })
                    DropdownMenuItem(text = { Text("Forget") }, onClick = { menu = false; onForget(row) })
                }
            }
        },
    )
}

// "Enter the code shown on Pixel 8", as the desktop's pair dialog.
@Composable
private fun PairCodeDialog(p: ConnectionsModel.PairOut, onPair: (String) -> Unit, onCancel: () -> Unit) {
    var code by remember { mutableStateOf("") }
    val digits = code.filter { it.isDigit() }
    AlertDialog(
        onDismissRequest = {},
        title = { Text("Pair") },
        text = {
            Column {
                Text(if (p.answered) "Enter the code shown on ${p.name}" else "Connecting to ${p.name}…")
                OutlinedTextField(
                    code,
                    { text -> code = text.filter { it.isDigit() || it == ' ' }.take(7) },
                    placeholder = { Text("000 000") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                    isError = p.problem != null,
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                )
                p.problem?.let {
                    Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.padding(top = 8.dp))
                }
            }
        },
        confirmButton = {
            TextButton(enabled = digits.length == 6 && !p.working && p.canRetry, onClick = { onPair(digits) }) { Text("Pair") }
        },
        dismissButton = { TextButton(onClick = onCancel) { Text("Cancel") } },
    )
}

@Composable
private fun TextDialog(
    title: String, label: String, initial: String, placeholder: String? = null, confirm: String = "OK",
    allowEmpty: Boolean = false, onDismiss: () -> Unit, onOk: (String) -> Unit,
) {
    var text by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            OutlinedTextField(
                text, { text = it },
                label = { Text(label) },
                placeholder = placeholder?.let { { Text(it) } },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
        },
        confirmButton = { TextButton(enabled = allowEmpty || text.isNotBlank(), onClick = { onOk(text.trim()) }) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
