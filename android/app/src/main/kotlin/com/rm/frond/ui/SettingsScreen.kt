package com.rm.frond.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.rm.frond.ControllerMap
import com.rm.frond.Device
import com.rm.frond.HomeModel
import com.rm.frond.NetRelay
import com.rm.frond.PalmButton
import com.rm.frond.Orientation
import com.rm.frond.ScreenSize
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(model: HomeModel) {
    val controls = model.controlsFor
    if (controls != null) ControlsScreen(model, controls) else SettingsList(model)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SettingsList(model: HomeModel) {
    val s = model.settings
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = { model.settingsOpen = false }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Header("Display")
            Choice("Size", s.size, ScreenSize.entries, { it.label }) { v -> model.update(s.copy(size = v)) }
            SwitchRow("Smooth pixels", s.smoothing) { v -> model.update(s.copy(smoothing = v)) }
            Choice("Orientation", s.orientation, Orientation.entries, { it.label }) { v ->
                model.update(s.copy(orientation = v))
            }
            Header("Audio", Modifier.padding(top = 16.dp))
            SwitchRow("Sound", s.sound) { v -> model.update(s.copy(sound = v)) }
            SwitchRow("Low-latency audio", s.fastAudio) { v -> model.update(s.copy(fastAudio = v)) }
            Header("Debugger", Modifier.padding(top = 16.dp))
            SwitchRow("gdb server", s.debugger) { v -> model.update(s.copy(debugger = v)) }
            if (s.debugger) DebuggerPort(s.debuggerPort) { v -> model.update(s.copy(debuggerPort = v)) }
            Header("Controller", Modifier.padding(top = 16.dp))
            val added = model.added
            if (added.isEmpty()) EmptyNote("Add a device to set its controls.")
            for (device in added) {
                Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(device.label, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
                    TextButton(onClick = { model.controlsFor = device }) { Text("Set controls") }
                }
            }
        }
    }
}

// The port gdb connects to on this device, saved once it's a valid port, with this device's addresses.
@Composable
private fun DebuggerPort(port: Int, onChange: (Int) -> Unit) {
    val context = LocalContext.current
    var text by remember { mutableStateOf(port.toString()) }
    val valid = NetRelay.port(text) != null
    OutlinedTextField(
        text,
        {
            text = it.filter(Char::isDigit).take(5)
            NetRelay.port(text)?.let(onChange)
        },
        label = { Text("Port") },
        singleLine = true,
        isError = !valid,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
    )
    val addresses = produceState<List<String>?>(null) { value = withContext(Dispatchers.IO) { NetRelay.addresses(context) } }.value
    if (addresses != null) {
        Text(
            if (addresses.isEmpty()) "No network address" else "This device: " + addresses.joinToString(", "),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// Each Palm button with the controller keys that press it. Tap one, then press a controller key.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ControlsScreen(model: HomeModel, device: Device) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("${device.label} controls") },
                navigationIcon = {
                    IconButton(onClick = { model.capturing = null; model.controlsFor = null }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = { TextButton(onClick = { model.resetController(device) }) { Text("Reset to defaults") } },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            ControllerButtons(model, device)
        }
    }
}

@Composable
private fun ControllerButtons(model: HomeModel, device: Device) {
    val map = model.controllers[device] ?: return
    for (button in PalmButton.on(device)) {
        val waiting = model.capturing == (device to button)
        Row(
            Modifier.fillMaxWidth()
                .clickableRow { model.capturing = if (waiting) null else device to button }
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(button.label(device), Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
            if (waiting) {
                Text("Press a controller button", color = MaterialTheme.colorScheme.primary)
                TextButton(onClick = { model.clearButton(device, button) }) { Text("Clear") }
            } else {
                val keys = map.keysFor(button)
                Text(
                    if (keys.isEmpty()) "None" else keys.joinToString(", ") { ControllerMap.keyName(it) },
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().toggleable(checked, role = Role.Switch, onValueChange = onChange).padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
        Switch(checked = checked, onCheckedChange = null)
    }
}

// A read-only field that opens a menu of choices.
@Composable
fun <T> Choice(label: String, value: T, options: List<T>, name: (T) -> String, onChange: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        OutlinedTextField(
            value = name(value),
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { Icon(Icons.Default.ArrowDropDown, contentDescription = null) },
            modifier = Modifier.fillMaxWidth(),
        )
        // Covers the field so a tap anywhere opens the menu.
        Box(
            Modifier.matchParentSize()
                .semantics { contentDescription = "$label, ${name(value)}"; role = Role.DropdownList }
                .clickableRow { open = true },
        )
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (o in options) {
                DropdownMenuItem(text = { Text(name(o)) }, onClick = {
                    open = false
                    onChange(o)
                })
            }
        }
    }
}
