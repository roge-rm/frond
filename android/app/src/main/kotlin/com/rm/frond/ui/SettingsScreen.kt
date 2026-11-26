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
import androidx.compose.material.icons.filled.Close
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
    val controls = model.controls
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
            Choice("Orientation", s.orientation, Orientation.entries, { it.label }) { v ->
                model.update(s.copy(orientation = v))
            }
            SwitchRow("Immersive mode", s.immersive) { v -> model.update(s.copy(immersive = v)) }
            Header("Connections", Modifier.padding(top = 16.dp))
            SwitchRow("Allow incoming Frond links over the network", s.linkNetwork) { v -> model.update(s.copy(linkNetwork = v)) }
            Text("Applies when the link service next starts.", style = MaterialTheme.typography.bodyMedium)
            Header("Debugger", Modifier.padding(top = 16.dp))
            SwitchRow("gdb server", s.debugger) { v -> model.update(s.copy(debugger = v)) }
            if (s.debugger) {
                SwitchRow("Allow gdb over the network", s.debuggerNetwork) { v -> model.update(s.copy(debuggerNetwork = v)) }
                Text("Applies when a machine starts.", style = MaterialTheme.typography.bodyMedium)
                DebuggerPort(s.debuggerPort, s.debuggerNetwork) { v -> model.update(s.copy(debuggerPort = v)) }
            }
            Header("Controller", Modifier.padding(top = 16.dp))
            SwitchRow("Hide touch controls with a controller", s.hideTouchWithController) { v -> model.update(s.copy(hideTouchWithController = v)) }
            val added = model.added
            if (added.isEmpty()) EmptyNote("Add a device to set its controls.")
            for (device in added) {
                Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(device.label, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
                    TextButton(onClick = { model.openControls(device) }) { Text("Set controls") }
                }
            }
        }
    }
}

// The port gdb connects to on this device, saved once it's a valid port, with this device's addresses.
@Composable
private fun DebuggerPort(port: Int, allowNetwork: Boolean, onChange: (Int) -> Unit) {
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
    val addresses = produceState<List<String>?>(null, allowNetwork) {
        value = if (allowNetwork) withContext(Dispatchers.IO) { NetRelay.addresses(context) } else listOf("127.0.0.1", "::1")
    }.value
    if (addresses != null) {
        Text(
            if (addresses.isEmpty()) "All interfaces" else "Listening addresses: " + addresses.joinToString(", ") { NetRelay.hostPort(it, port) },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// Each Palm button with the controller keys that press it. Tap one, then press a controller key or push
// a stick to add it; its keys can be taken off one at a time there.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ControlsScreen(model: HomeModel, editor: ControlsEditor) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("${editor.device.label} controls") },
                navigationIcon = {
                    IconButton(onClick = { model.closeControls() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = { TextButton(onClick = { editor.reset() }) { Text("Reset to defaults") } },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            ControllerButtons(editor)
        }
    }
}

@Composable
fun SwitchRow(label: String, checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().toggleable(checked, enabled = enabled, role = Role.Switch, onValueChange = onChange).padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
        Switch(checked = checked, enabled = enabled, onCheckedChange = null)
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

// Like Choice, with the grouped device menu.
@Composable
fun DeviceChoice(label: String, value: com.rm.frond.Device, onChange: (com.rm.frond.Device) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        OutlinedTextField(
            value = value.listLabel,
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = { Icon(Icons.Default.ArrowDropDown, contentDescription = null) },
            modifier = Modifier.fillMaxWidth(),
        )
        Box(
            Modifier.matchParentSize()
                .semantics { contentDescription = "$label, ${value.listLabel}"; role = Role.DropdownList }
                .clickableRow { open = true },
        )
        DeviceMenu(open, { open = false }, onPick = onChange)
    }
}
