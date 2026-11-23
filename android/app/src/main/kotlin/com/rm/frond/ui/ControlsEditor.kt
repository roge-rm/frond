package com.rm.frond.ui

import android.content.Context
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.rm.frond.ControllerMap
import com.rm.frond.Device
import com.rm.frond.PalmButton

// Edits one device's controller map and saves each change. `device` is the one shown; `mapDevice` is the
// one whose buttons and defaults apply once its firmware is known. Used by Settings and the running sheet.
// While `capturing` is set, the host sends the next controller key or stick direction to assign().
class ControlsEditor(private val context: Context, val device: Device, val mapDevice: Device = device) {
    var map by mutableStateOf(ControllerMap.load(context, mapDevice))
        private set
    var capturing by mutableStateOf<PalmButton?>(null)

    fun assign(key: Int) {
        val button = capturing ?: return
        capturing = null
        change(map.assign(button, key))
    }

    // Reads the saved map again, after the session changed it.
    fun reload() { map = ControllerMap.load(context, mapDevice) }

    fun removeKey(key: Int) = change(map.remove(key))

    fun clearButton(button: PalmButton) {
        capturing = null
        change(map.clear(button))
    }

    fun reset() {
        capturing = null
        ControllerMap.reset(context, device)
        map = ControllerMap.defaults(mapDevice)
    }

    private fun change(changed: ControllerMap) {
        ControllerMap.save(context, device, changed)
        map = changed
    }
}

// Each Palm button with the controller keys that press it. Tap one, then press a controller key or push
// a stick to add it; its keys can be taken off one at a time there.
@Composable
fun ControllerButtons(editor: ControlsEditor) {
    val device = editor.device
    for (button in PalmButton.listed(editor.mapDevice)) {
        val waiting = editor.capturing == button
        Row(
            Modifier.fillMaxWidth()
                .clickableRow { editor.capturing = if (waiting) null else button }
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(button.label(device), Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
            if (waiting) {
                Text("Press a controller button", color = MaterialTheme.colorScheme.primary)
                TextButton(onClick = { editor.clearButton(button) }) { Text("Clear") }
            } else {
                val keys = editor.map.keysFor(button)
                Text(
                    if (keys.isEmpty()) "None" else keys.joinToString(", ") { ControllerMap.keyName(it) },
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (waiting) {
            for (key in editor.map.keysFor(button)) {
                val name = ControllerMap.keyName(key)
                Row(Modifier.fillMaxWidth().padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(name, Modifier.weight(1f), color = MaterialTheme.colorScheme.onSurfaceVariant)
                    IconButton(onClick = { editor.removeKey(key) }) {
                        Icon(Icons.Default.Close, contentDescription = "Remove $name")
                    }
                }
            }
        }
    }
}
