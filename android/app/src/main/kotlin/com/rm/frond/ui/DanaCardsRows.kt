package com.rm.frond.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.rm.frond.CardFamily
import com.rm.frond.RunningModel
import com.rm.frond.StorageSlot

@Composable
fun DanaCardsRows(model: RunningModel) {
    val cards = model.danaCards ?: return
    var selected by remember { mutableStateOf<StorageSlot?>(null) }
    var family by remember { mutableStateOf<CardFamily?>(null) }
    var writeProtected by remember { mutableStateOf(false) }
    var exporting by remember { mutableStateOf<StorageSlot?>(null) }
    var ejecting by remember { mutableStateOf<StorageSlot?>(null) }
    var openingFiles by remember { mutableStateOf(false) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val slot = selected
        val type = family
        if (uri != null && slot != null && type != null) model.danaJob { it.import(slot, type, uri, writeProtected) }
        selected = null
    }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val slot = exporting
        if (uri != null && slot != null) model.danaJob { it.export(slot, uri) }
        exporting = null
    }
    Column {
        for (state in cards.states) {
            val slot = state.slot
            val card = state.card
            val label = when {
                !state.known -> "Not known"
                card == null -> "No card"
                else -> "${if (slot.stick) "Memory Stick" else if (card.protocol == "mmc") "MMC" else "SD"} · ${if (card.inserted) "Inserted" else "Ejected"}" +
                    if (card.failed) " · I/O error" else if (card.writeProtected) " · Write protected" else ""
            }
            ListItem(headlineContent = { Text(if (cards.states.size == 1 && !slot.stick) "Card" else slot.title) }, supportingContent = { Text(label) })
            Row {
                val usable = state.known && model.busy == null
                if (card == null) TextButton(enabled = usable, onClick = {
                    selected = slot; family = cards.device.familiesOf(slot).firstOrNull()
                }) { Text("Choose card") }
                else {
                    TextButton(enabled = usable, onClick = {
                        if (card.inserted) ejecting = slot else model.danaJob { it.command(slot, "insert") }
                    }) {
                        Text(if (card.inserted) "Eject" else "Insert")
                    }
                    if (!card.inserted) TextButton(enabled = usable && !card.failed, onClick = { model.danaJob { it.command(slot, "take-out") } }) { Text("Remove") }
                    // A stick's files open as the TG50's do: take-out, Files on its file, put-in.
                    if (slot.stick) TextButton(enabled = usable && !card.failed, onClick = {
                        if (card.inserted) openingFiles = true else model.openFiles()
                    }) { Text("Files…") }
                    TextButton(enabled = usable && !card.failed, onClick = { exporting = slot; export.launch("${cards.device.label} ${if (slot.stick) "stick.frondms" else "card ${slot.label}.frondsd"}") }) { Text("Export") }
                }
                if (!state.known) TextButton(enabled = model.busy == null, onClick = { model.danaJob() }) { Text("Try again") }
            }
            if (slot.stick && card != null) ListItem(
                headlineContent = { Text("Write protected") },
                trailingContent = {
                    Switch(checked = card.writeProtected, enabled = state.known && model.busy == null && !card.inserted && !card.failed,
                        onCheckedChange = { value -> model.danaJob { it.protect(slot, value) } })
                },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
        }
    }
    selected?.let { slot ->
        AlertDialog(onDismissRequest = { selected = null }, title = { Text(slot.title) }, text = {
            Column(Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState())) {
                Row { for (type in cards.device.familiesOf(slot)) TextButton(onClick = { family = type }) {
                    Text(type.label + if (family == type) " ✓" else "")
                } }
                Row { Checkbox(writeProtected, { writeProtected = it }); Text("Write protected") }
                val type = family
                if (type != null) {
                    for (size in type.sizes) TextButton(onClick = {
                        model.danaJob { it.make(slot, type, size, "${type.label} $size MB", writeProtected) }; selected = null
                    }) { Text("New ${type.label} $size MB") }
                    TextButton(onClick = { pick.launch(arrayOf("*/*")) }) { Text("Import ${type.label} image") }
                    for (entry in cards.entries.filter { it.family == type.family }) TextButton(onClick = {
                        model.danaJob { it.attach(slot, entry, writeProtected) }; selected = null
                    }) { Text(entry.name) }
                }
            }
        }, confirmButton = { TextButton(onClick = { selected = null }) { Text("Close") } })
    }
    if (openingFiles) AlertDialog(onDismissRequest = { openingFiles = false }, title = { Text("Open the stick's files?") },
        text = { Text("Close files open on the Palm first. The stick is ejected while Files is open and goes back in when you close it.") },
        confirmButton = { TextButton(onClick = { model.openFiles(); openingFiles = false }) { Text("Open") } },
        dismissButton = { TextButton(onClick = { openingFiles = false }) { Text("Cancel") } })
    ejecting?.let { slot ->
        AlertDialog(onDismissRequest = { ejecting = null }, title = { Text("Eject ${if (slot.stick) slot.title else slot.title.lowercase()}?") },
            text = { Text("Close files open on the Palm first.") },
            confirmButton = { TextButton(onClick = { model.danaJob { it.command(slot, "eject") }; ejecting = null }) { Text("Eject") } },
            dismissButton = { TextButton(onClick = { ejecting = null }) { Text("Cancel") } })
    }
}
