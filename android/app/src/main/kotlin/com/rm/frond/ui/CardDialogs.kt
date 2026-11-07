package com.rm.frond.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import com.rm.frond.CardChoices
import com.rm.frond.CardsModel
import com.rm.frond.FolderCard
import com.rm.frond.Cards
import com.rm.frond.Device
import java.io.File

private val Faded @Composable get() = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)

// An exposed dropdown of labelled values; greyed values can't be picked.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun <T> Dropdown(label: String, values: List<T>, selected: T?, text: (T) -> String, enabled: (T) -> Boolean, onPick: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }) {
        OutlinedTextField(
            value = selected?.let(text) ?: "",
            onValueChange = {},
            readOnly = true,
            singleLine = true,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (v in values) {
                DropdownMenuItem(text = { Text(text(v)) }, enabled = enabled(v), onClick = { open = false; onPick(v) })
            }
        }
    }
}

// New card: Name, Type and Size. `device` limits the types to what its slot takes and greys sizes
// too big for it; null offers every type any device takes.
@Composable
fun NewCardDialog(library: File, device: Device?, onDismiss: () -> Unit, onMake: (name: String, kind: String, size: Int) -> Unit) {
    val kinds = remember(device) { CardChoices.kindsFor(device) }
    var kind by remember { mutableStateOf(kinds.firstOrNull() ?: "sd") }
    val options = remember(kind, device) { CardChoices.sizeOptions(kind, device) }
    var size by remember(kind) { mutableStateOf(CardChoices.defaultSize(options)) }
    var nameEdited by remember { mutableStateOf(false) }
    var name by remember { mutableStateOf("") }
    LaunchedEffect(kind) { if (!nameEdited) name = Cards.freeCardName(library, Cards.kindName(kind)) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("New card") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it; nameEdited = true }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                Dropdown("Type", kinds, kind, { Cards.kindName(it) }, { true }) { kind = it }
                Dropdown("Size", options, options.firstOrNull { it.size == size }, { it.label }, { it.enabled }) { size = it.size }
            }
        },
        confirmButton = {
            val s = size
            TextButton(enabled = name.isNotBlank() && s != null, onClick = { if (s != null) onMake(name.trim(), kind, s) }) { Text("Make") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// Import folder as card: Name, Type and Size. Sizes the folder doesn't fit are greyed "Too small";
// the default is the smallest it fits. Classic Memory Sticks aren't offered.
@Composable
fun FolderCardDialog(library: File, draft: CardsModel.FolderDraft, onDismiss: () -> Unit, onMake: (name: String, kind: String, size: Int) -> Unit) {
    val kinds = remember { FolderCard.kinds() }
    var kind by remember { mutableStateOf(kinds.firstOrNull() ?: "sd") }
    val options = remember(kind) {
        CardChoices.sizesFor(kind).map { size ->
            val fits = FolderCard.fits(draft.plan, size)
            CardChoices.SizeOption(size, Device.sizeLabel(size) + if (fits) "" else " · Too small", fits)
        }
    }
    var size by remember(kind) { mutableStateOf(options.firstOrNull { it.enabled }?.size) }
    var name by remember { mutableStateOf(Cards.freeCardName(library, draft.root.name)) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Import folder as card") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("${draft.root.name}: ${Device.sizeLabel(maxOf(1L, (draft.plan.bytes + (1L shl 20) - 1) shr 20).toInt())} in ${draft.plan.files} files")
                OutlinedTextField(name, { name = it }, label = { Text("Name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
                Dropdown("Type", kinds, kind, { Cards.kindName(it) }, { true }) { kind = it }
                Dropdown("Size", options, options.firstOrNull { it.size == size }, { it.label }, { it.enabled }) { size = it.size }
            }
        },
        confirmButton = {
            val s = size
            TextButton(enabled = name.isNotBlank() && s != null, onClick = { if (s != null) onMake(name.trim(), kind, s) }) { Text("Make") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// "Link Games to machines": a checklist of machines with their device; greyed ones keep what they have.
@Composable
fun LinkCardsDialog(cardName: String, options: List<CardChoices.LinkOption>, onDismiss: () -> Unit, onLink: (List<File>) -> Unit) {
    var ticked by remember(options) { mutableStateOf(options.filter { it.checked }.map { it.dir }.toSet()) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Link $cardName to machines") },
        text = {
            if (options.isEmpty()) Text("No machine takes this card.")
            else LazyColumn(Modifier.heightIn(max = 360.dp)) {
                items(options, key = { it.dir.path }) { o ->
                    val on = o.dir in ticked
                    ListItem(
                        headlineContent = { Text(o.text, color = if (o.enabled) Color.Unspecified else Faded) },
                        leadingContent = { Checkbox(checked = on, onCheckedChange = null, enabled = o.enabled) },
                        modifier = if (o.enabled) Modifier.clickableRow { ticked = if (on) ticked - o.dir else ticked + o.dir } else Modifier,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                // A greyed machine keeps the link it has.
                onLink(options.filter { if (it.enabled) it.dir in ticked else it.checked }.map { it.dir })
            }) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// "SD card for Palm TX": No card, then the cards the slot takes, with New card and Link to machines….
@Composable
fun CardPickerDialog(
    title: String,
    choices: List<CardChoices.Choice>,
    selected: String?,
    onSelect: (String?) -> Unit,
    onNewCard: () -> Unit,
    onLink: () -> Unit,
    onDismiss: () -> Unit,
    onOk: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                LazyColumn(Modifier.heightIn(max = 320.dp)) {
                    items(choices, key = { it.id ?: "" }) { c ->
                        ListItem(
                            headlineContent = { Text(c.text, color = if (c.enabled) Color.Unspecified else Faded) },
                            leadingContent = { RadioButton(selected = c.id == selected, onClick = null, enabled = c.enabled) },
                            modifier = if (c.enabled) Modifier.clickableRow { onSelect(c.id) } else Modifier,
                        )
                    }
                }
                Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onNewCard, shape = MaterialTheme.shapes.small) { Text("New card") }
                    OutlinedButton(onClick = onLink, enabled = selected != null, shape = MaterialTheme.shapes.small) { Text("Link to machines…") }
                }
            }
        },
        confirmButton = { TextButton(onClick = onOk) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// Rename for a card; starts with the whole name selected.
@Composable
fun RenameCardDialog(name: String, onDismiss: () -> Unit, onRename: (String) -> Unit) {
    var text by remember(name) { mutableStateOf(TextFieldValue(name, TextRange(0, name.length))) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Rename") },
        text = {
            val focus = remember { FocusRequester() }
            OutlinedTextField(text, { text = it }, singleLine = true, modifier = Modifier.fillMaxWidth().focusRequester(focus))
            LaunchedEffect(Unit) { focus.requestFocus() }
        },
        confirmButton = { TextButton(enabled = text.text.isNotBlank(), onClick = { onRename(text.text.trim()) }) { Text("Rename") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// One choice among labelled options (the type of an image that fits SD and Pro, or what a copy of
// a machine does with its card). `note` is a line under the list for the picked option.
@Composable
fun <T> ChoiceDialog(
    title: String,
    options: List<T>,
    label: (T) -> String,
    action: String,
    onDismiss: () -> Unit,
    note: ((T) -> String)? = null,
    onChoose: (T) -> Unit,
) {
    var picked by remember(options) { mutableStateOf(options.first()) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                for (o in options) {
                    ListItem(
                        headlineContent = { Text(label(o)) },
                        leadingContent = { RadioButton(selected = o == picked, onClick = null) },
                        modifier = Modifier.clickableRow { picked = o },
                    )
                }
                note?.let {
                    Text(it(picked), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 8.dp))
                }
            }
        },
        confirmButton = { TextButton(onClick = { onChoose(picked) }) { Text(action) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
