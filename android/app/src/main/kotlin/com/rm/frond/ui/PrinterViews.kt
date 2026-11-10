package com.rm.frond.ui

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.RadioButton
import androidx.compose.material3.TextButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarResult
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.rm.frond.Device
import com.rm.frond.PrintJob
import com.rm.frond.PrintJobs
import com.rm.frond.PrintNotices
import com.rm.frond.PrinterModel
import com.rm.frond.UsbPort

private fun Context.activity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.activity()
    else -> null
}

// The Connections row: the USB port choice with its readout, and a way back to a held job.
@Composable
fun PrinterRow(printer: PrinterModel, device: Device) {
    val context = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    var choosing by remember { mutableStateOf(false) }
    val text = when (printer.port) {
        UsbPort.PRINTER -> "Printer, ${printer.jobs.bytes} bytes received"
        UsbPort.WIFI -> "Wi-Fi adapter" + (printer.wifiText?.let { ", $it" } ?: "")
        else -> "Off"
    }
    ListItem(
        headlineContent = { Text("USB port") },
        supportingContent = { Text(text) },
        colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.clickableRow { choosing = true },
    )
    if (choosing) AlertDialog(
        onDismissRequest = { choosing = false },
        title = { Text("USB port") },
        text = {
            Column {
                for (choice in UsbPort.choices(device)) ListItem(
                    headlineContent = { Text(UsbPort.label(choice)) },
                    leadingContent = { RadioButton(selected = choice == printer.port, onClick = null) },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickableRow {
                        choosing = false
                        printer.choose(choice)
                        // The notification for a print that arrives in the background needs this on Android 13 and later.
                        if (choice == UsbPort.PRINTER && Build.VERSION.SDK_INT >= 33 &&
                            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                            ask.launch(Manifest.permission.POST_NOTIFICATIONS)
                        }
                    },
                )
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = { choosing = false }) { Text("Cancel") } },
    )
}

// A way back to a held job, in the Connections sheet's prints. named: listed with another printer's job.
@Composable
fun HeldRow(jobs: PrintJobs, named: Boolean) {
    jobs.held?.let { held ->
        ListItem(
            headlineContent = { Text(if (named) held.namedTitle else held.title) },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.clickableRow { jobs.choosing = true },
        )
    }
}

// The Connections row for the Dana Wireless's built-in Wi-Fi: on or off, with the link readout.
@Composable
fun WifiModuleRow(printer: PrinterModel) {
    var choosing by remember { mutableStateOf(false) }
    val text = if (printer.moduleOn) (printer.moduleText ?: "On") else "Off"
    ListItem(
        headlineContent = { Text("Wi-Fi") },
        supportingContent = { Text(text) },
        colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.clickableRow { choosing = true },
    )
    if (choosing) AlertDialog(
        onDismissRequest = { choosing = false },
        title = { Text("Wi-Fi") },
        text = {
            Column {
                for (on in listOf(true, false)) ListItem(
                    headlineContent = { Text(if (on) "On" else "Off") },
                    leadingContent = { RadioButton(selected = on == printer.moduleOn, onClick = null) },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickableRow { choosing = false; printer.chooseModule(on) },
                )
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = { choosing = false }) { Text("Cancel") } },
    )
}

// One snackbar for received prints, below the bar, and each printer's choices sheet. One held job gets its own
// notice, whose Options opens its choices; jobs on more than one printer get "N prints ready", whose Show opens the
// Connections sheet with a row for each.
@Composable
fun PrinterPrompt(printers: List<PrintJobs>, top: Dp, show: () -> Unit) {
    val snackbar = remember { SnackbarHostState() }
    val versions = printers.map { it.held?.version }
    LaunchedEffect(versions) {
        val ready = printers.filter { it.held != null }
        val (text, action) = PrintNotices.prompt(ready.mapNotNull { it.held }) ?: return@LaunchedEffect
        val result = snackbar.showSnackbar(text, actionLabel = action, withDismissAction = true, duration = SnackbarDuration.Indefinite)
        if (result == SnackbarResult.ActionPerformed) ready.singleOrNull()?.let { it.choosing = true } ?: show()
    }
    SnackbarHost(snackbar, Modifier.padding(top = top))
    for (printer in printers) key(printer) {
        // Registered here, which stays composed while the choices sheet closes to open the file picker.
        val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { printer.saveRaw(it) }
        val held = printer.held
        if (printer.choosing && held != null) PrinterChoices(printer, held) { save.launch("print.raw") }
    }
}

@Composable
private fun PrinterChoices(printer: PrintJobs, held: PrintJobs.Held, saveRaw: () -> Unit) {
    val activity = LocalContext.current.activity()
    val pages = held.pages
    Dialog({ printer.choosing = false }, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth().padding(24.dp), shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Column {
                Text(held.title, style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp))
                if (!pages.renderable) Text(pages.reason, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                ListItem(
                    headlineContent = { Text("Print", color = if (pages.renderable) Color.Unspecified else faded) },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = if (pages.renderable && activity != null) Modifier.clickableRow { printer.choosing = false; printer.print(activity) } else Modifier,
                )
                ListItem(
                    headlineContent = { Text("Save raw file") },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickableRow { printer.choosing = false; saveRaw() },
                )
                ListItem(
                    headlineContent = { Text("Discard", color = MaterialTheme.colorScheme.error) },
                    colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickableRow { printer.discard() },
                )
            }
        }
    }
}
