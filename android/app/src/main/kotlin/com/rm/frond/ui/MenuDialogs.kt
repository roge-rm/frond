package com.rm.frond.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.rm.frond.Device
import com.rm.frond.NetworkMode
import com.rm.frond.PeripheralsModel

// Dialogs the Peripherals screen and the running bar's menu share.

// The job running now, with Cancel when it can be cancelled.
@Composable
fun BusyDialog(model: PeripheralsModel) {
    model.busy?.let { label ->
        AlertDialog(onDismissRequest = {}, confirmButton = {
            if (model.canCancelFiles) TextButton(onClick = { model.cancelFiles() }, enabled = !model.cancellingFiles) {
                Text(if (model.cancellingFiles) "Cancelling" else "Cancel")
            }
        }, title = { Text(label) }, text = {
            val done = model.progress
            if (done == null) Progress()
            else androidx.compose.material3.LinearProgressIndicator(progress = { done },
                modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp))
        })
    }
}

@Composable
fun AlertBox(model: PeripheralsModel) {
    model.alert?.let { (title, text) ->
        AlertDialog(
            onDismissRequest = { model.alert = null },
            title = { Text(title) },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { model.alert = null }) { Text("OK") } },
        )
    }
}

@Composable
fun NetworkDialog(current: NetworkMode?, onDismiss: () -> Unit, onChoose: (NetworkMode) -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Network") },
        text = {
            LazyColumn {
                items(NetworkMode.entries) { mode ->
                    ListItem(
                        headlineContent = { Text(mode.label) },
                        leadingContent = { RadioButton(selected = mode == current, onClick = null) },
                        modifier = Modifier.clickableRow {
                            if (mode != current) onChoose(mode) else onDismiss()
                        },
                    )
                }
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
fun ResetDialog(device: Device, onDismiss: () -> Unit, onReset: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Reset the ${device.label} now?") },
        confirmButton = { TextButton(onClick = onReset) { Text("Reset") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
fun HardResetDialog(device: Device, onDismiss: () -> Unit, onHardReset: (saveFirst: Boolean) -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Hard reset") },
        text = { Text("Erase everything on the ${device.label}?") },
        confirmButton = {
            Row {
                TextButton(onClick = { onHardReset(true) }) { Text("Save first") }
                TextButton(onClick = { onHardReset(false) }) { Text("Erase") }
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
