package com.rm.frond.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.rm.frond.BeamMode
import com.rm.frond.BluetoothChoice
import com.rm.frond.BluetoothLink
import com.rm.frond.InfraredChoice
import com.rm.frond.SyncProtocol
import com.rm.frond.LinkStore
import com.rm.frond.NetRelay
import com.rm.frond.RunningModel
import com.rm.frond.UsbLink

internal fun relayText(s: NetRelay.Status): String = when (s.phase) {
    NetRelay.Phase.OFF -> s.error?.let { "Off. $it" } ?: "Off"
    NetRelay.Phase.WAITING -> "Waiting for a peer at ${s.listenAddresses}" +
        if ((s.mode as? BeamMode.Listen)?.allowNetwork == false) ", network connections are off" else ""
    NetRelay.Phase.CONNECTING -> "Connecting to ${(s.mode as? BeamMode.Connect)?.let { NetRelay.hostPort(it.host, it.port) }}"
    NetRelay.Phase.CONNECTED -> "Connected to ${s.peer ?: "a peer"}"
    NetRelay.Phase.DISCONNECTED -> s.error ?: "Disconnected"
}

internal fun debuggerText(s: NetRelay.Status): String = when (s.phase) {
    NetRelay.Phase.CONNECTED -> "gdb connected from ${s.peer ?: "the network"}"
    NetRelay.Phase.WAITING -> "Waiting for gdb at ${s.listenAddresses}"
    else -> s.error?.let { "Off. $it" } ?: "Off"
}

internal fun bluetoothText(s: BluetoothLink.Status, radio: String?): String {
    if (!s.active) return s.text
    val link = s.text
    // The core's live observation of the Palm's radio and ACL links.
    val note = when {
        radio == null -> null
        radio.contains("turn Bluetooth off and on") -> "Turn Bluetooth off and on in Palm OS."
        radio.startsWith("disconnected: ") -> radio.removePrefix("disconnected: ")
        else -> radio.takeUnless { it.isBlank() || it == "off" }
    }
    return if (note == null) link else "$link. ${note.replaceFirstChar { it.uppercaseChar() }}"
}

// Off, wait for a peer on a port, or connect to an address, for one of the network relays.
@Composable
private fun RelayDialog(
    title: String,
    labels: List<String>,
    relay: NetRelay,
    current: BeamMode,
    onDismiss: () -> Unit,
    syncAttached: Boolean = false,
    useSync: (() -> Unit)? = null,
    onChoose: (BeamMode) -> Unit,
) {
    val context = LocalContext.current
    val last = remember { relay.last(context) }
    val start = if (current != BeamMode.Off) current else last ?: BeamMode.Off
    LinkDialog(
        title = title,
        labels = labels,
        defaultPort = relay.defaultPort,
        choice = when {
            syncAttached -> "sync"
            current == BeamMode.Off -> "off"
            current is BeamMode.Listen -> "listen"
            else -> "connect"
        },
        port = (start as? BeamMode.Listen)?.port,
        allowNetwork = (current as? BeamMode.Listen)?.allowNetwork == true,
        address = (last as? BeamMode.Connect ?: start as? BeamMode.Connect)?.let { NetRelay.hostPort(it.host, it.port) },
        addresses = produceState<List<String>?>(null) { value = withContext(Dispatchers.IO) { NetRelay.addresses(context) } }.value,
        onDismiss = onDismiss,
        toMode = { choice, port, address, allowNetwork ->
            when (choice) {
                "listen" -> NetRelay.port(port)?.let { BeamMode.Listen(it, allowNetwork) }
                "connect" -> relay.address(address)?.let { BeamMode.Connect(it.first, it.second) }
                else -> BeamMode.Off
            }
        },
        onChoose = onChoose,
        useSync = useSync,
    )
}

// Off, take peers on a port (choice "listen") or reach an address ("connect").
@Composable
private fun <T> LinkDialog(
    title: String,
    // For off, listen and connect.
    labels: List<String>,
    defaultPort: Int,
    choice: String,
    port: Int?,
    address: String?,
    addresses: List<String>?,
    allowNetwork: Boolean,
    onDismiss: () -> Unit,
    toMode: (choice: String, port: String, address: String, allowNetwork: Boolean) -> T?,
    onChoose: (T) -> Unit,
    useSync: (() -> Unit)? = null,
) {
    var picked by remember { mutableStateOf(choice) }
    var portText by remember { mutableStateOf((port ?: defaultPort).toString()) }
    var addressText by remember { mutableStateOf(address ?: "") }
    var network by remember { mutableStateOf(allowNetwork) }
    val mode = toMode(picked, portText, addressText, network)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                val options = listOf("off", "listen", "connect").zip(labels)
                val choices = if (useSync != null) options + ("sync" to "Sync Manager") else options
                for ((id, label) in choices) {
                    ListItem(
                        headlineContent = { Text(label) },
                        leadingContent = { RadioButton(selected = picked == id, onClick = null) },
                        modifier = Modifier.clickableRow { picked = id },
                    )
                }
                if (picked == "listen") {
                    OutlinedTextField(
                        portText,
                        { portText = it.filter(Char::isDigit).take(5) },
                        label = { Text("Port") },
                        singleLine = true,
                        isError = mode == null,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    ListItem(
                        headlineContent = { Text("Allow network peers") },
                        trailingContent = { Switch(checked = network, onCheckedChange = { network = it }) },
                    )
                    Text(
                        when {
                            !network -> "Loopback (127.0.0.1, ::1)"
                            addresses == null -> "Looking up this device's address"
                            addresses.isEmpty() -> "All interfaces"
                            else -> "This device: " + addresses.joinToString(", ")
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
                if (picked == "connect") {
                    OutlinedTextField(
                        addressText,
                        { addressText = it.trim() },
                        label = { Text("Address and port") },
                        placeholder = { Text("192.168.1.20:$defaultPort") },
                        singleLine = true,
                        isError = addressText.isNotEmpty() && mode == null,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(enabled = mode != null, onClick = {
                if (picked == "sync") useSync?.invoke() else mode?.let(onChoose)
            }) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}


// Asks before ejecting the card, or before opening its files (which ejects it).
@Composable
fun CardEjectDialog(model: RunningModel, files: Boolean, n: Int = 0, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Eject the card?") },
        text = { Text(if (files) "Close any files open on the Palm first. Frond saves a recovery session before the exchange." else "Close any files open on the Palm first.") },
        confirmButton = {
            TextButton(onClick = {
                onDismiss()
                if (model.selectSlot(n)) {
                    if (files) model.openFiles() else model.job("Ejecting", "Couldn't eject the card") { it.eject() }
                }
            }) { Text("Eject") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// The card picker and the dialogs it leads to: new card, link to machines, and the confirm before a change.
@Composable
fun CardPickerDialogs(model: RunningModel) {
    val context = LocalContext.current
    model.picker?.let { p ->
        if (!model.pickerMaking && model.pickerLinking == null && !model.confirmingChange && model.busy == null) {
            CardPickerDialog(p.title, p.choices, p.selected, onSelect = { model.pick(it) }, onNewCard = { model.pickerMaking = true },
                onLink = { model.startPickerLink() }, onDismiss = { model.picker = null }, onOk = { model.pickerOk() })
        }
    }
    if (model.pickerMaking) {
        NewCardDialog(com.rm.frond.Cards.libraryFor(com.rm.frond.Sessions.folder(context)), model.device,
            onDismiss = { model.pickerMaking = false }) { name, kind, size -> model.makeCard(name, kind, size) }
    }
    model.pickerLinking?.let { (card, options) ->
        LinkCardsDialog(card.name, options, onDismiss = { model.pickerLinking = null }) { model.linkFromPicker(card, it) }
    }
    if (model.confirmingChange) {
        AlertDialog(
            onDismissRequest = { model.confirmChange(false) },
            title = { Text("Eject the card?") },
            text = { Text("Close any files open on the Palm first.") },
            confirmButton = { TextButton(onClick = { model.confirmChange(true) }) { Text("Eject") } },
            dismissButton = { TextButton(onClick = { model.confirmChange(false) }) { Text("Cancel") } },
        )
    }
}

enum class LinkKind { BEAM, BLUETOOTH, SERIAL, USB }

// The chooser for one of the links: a connection, or for USB off, wait for a peer or connect to an address.
@Composable
fun LinkChoiceDialog(kind: LinkKind, model: RunningModel, onDismiss: () -> Unit) {
    val context = LocalContext.current
    when (kind) {
        LinkKind.BEAM -> {
            val current = model.beam.choice
            val choices = remember { InfraredChoice.infraredChoices(current, LinkStore.choices(InfraredChoice.folder(context)), model.device.irPhone) }
            // Leaving Printer with output that isn't saved asks first.
            var leaving by remember { mutableStateOf<String?>(null) }
            // The notification for a print that arrives in the background needs this on Android 13 and later.
            val ask = androidx.activity.compose.rememberLauncherForActivityResult(androidx.activity.result.contract.ActivityResultContracts.RequestPermission()) { }
            ConnectionChoiceDialog("Infrared beaming", current, onDismiss, choices) {
                if (current == InfraredChoice.PRINTER && it != current && model.irPrinter?.unsaved == true) leaving = it
                else {
                    onDismiss(); model.changeBeam(it)
                    if (it == InfraredChoice.PRINTER && android.os.Build.VERSION.SDK_INT >= 33 &&
                        context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                        ask.launch(android.Manifest.permission.POST_NOTIFICATIONS)
                    }
                }
            }
            leaving?.let { next ->
                AlertDialog(
                    onDismissRequest = { leaving = null },
                    title = { Text("Discard the print?") },
                    text = { Text("The infrared printer has output you haven't saved or printed. Changing the choice discards it.") },
                    confirmButton = { TextButton(onClick = { leaving = null; onDismiss(); model.changeBeam(next) }) { Text("Discard") } },
                    dismissButton = { TextButton(onClick = { leaving = null }) { Text("Cancel") } },
                )
            }
        }
        LinkKind.SERIAL -> {
            val sync = SyncProtocol.forDevice(model.device) == SyncProtocol.HOT_SYNC
            val current = if (model.serialSync) InfraredChoice.SYNC_MANAGER else model.serial.choice
            val choices = remember(current, sync) {
                InfraredChoice.serialChoices(current, LinkStore.choices(InfraredChoice.folder(context)), model.device.modem, sync,
                    ppp = model.device.runs68k)
            }
            ConnectionChoiceDialog("Serial port", current, onDismiss, choices) { onDismiss(); model.changeSerial(it) }
        }
        LinkKind.USB -> if (model.device.usbNetwork)
            ConnectionChoiceDialog("USB", if (model.usbCable) "on" else "off", onDismiss, listOf("off" to "Off", "on" to "On")) {
                onDismiss(); model.changeUsbCable(it == "on")
            }
        else {
            val sync = SyncProtocol.forDevice(model.device) == SyncProtocol.HOT_SYNC
            RelayDialog("USB HotSync", listOf("Off", "Wait for a peer", "Connect to an address"), UsbLink,
                model.usb.mode, onDismiss, syncAttached = model.usbSync,
                useSync = if (sync) ({ onDismiss(); model.changeUsbSync() }) else null,
            ) { onDismiss(); model.changeUsb(it) }
        }
        LinkKind.BLUETOOTH -> {
            val current = model.bluetooth.choice
            val choices = remember { BluetoothChoice.choices(current, LinkStore.choices(InfraredChoice.folder(context))) }
            ConnectionChoiceDialog("Bluetooth", current, onDismiss, choices) { onDismiss(); model.changeBluetooth(it) }
        }
    }
}

// Off or one of the paired connections, for the running machine's infrared or serial port; Bluetooth passes
// its own choices.
@Composable
fun ConnectionChoiceDialog(
    title: String, current: String, onDismiss: () -> Unit, given: List<Pair<String, String>>? = null, onChoose: (String) -> Unit,
) {
    val context = LocalContext.current
    val choices = given ?: remember { InfraredChoice.choices(current, LinkStore.choices(InfraredChoice.folder(context))) }
    var picked by remember { mutableStateOf(current) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column {
                for ((value, label) in choices) {
                    ListItem(
                        headlineContent = { Text(label) },
                        leadingContent = { RadioButton(selected = picked == value, onClick = null) },
                        modifier = Modifier.clickableRow { picked = value },
                    )
                }
                if (given == null && choices.size == 1) EmptyNote("Pair with another Frond in Connections first.")
            }
        },
        confirmButton = { TextButton(onClick = { onChoose(picked) }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
