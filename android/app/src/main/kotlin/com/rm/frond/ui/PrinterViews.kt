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
        UsbPort.PRINTER -> "Printer, ${printer.bytes} bytes received"
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
    printer.held?.let { held ->
        ListItem(
            headlineContent = { Text("Print received (${held.bytes} bytes)") },
            colors = androidx.compose.material3.ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.clickableRow { printer.choosing = true },
        )
    }
}

// The snackbar for a received print, below the bar, and the choices sheet it opens.
@Composable
fun PrinterPrompt(printer: PrinterModel, top: Dp) {
    val snackbar = remember { SnackbarHostState() }
    // Registered here, which stays composed while the choices sheet closes to open the file picker.
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { printer.saveRaw(it) }
    val version = printer.held?.version
    LaunchedEffect(version) {
        val held = printer.held ?: return@LaunchedEffect
        val result = snackbar.showSnackbar(
            "Print received (${held.bytes} bytes)", actionLabel = "Options", withDismissAction = true, duration = SnackbarDuration.Indefinite,
        )
        if (result == SnackbarResult.ActionPerformed) printer.choosing = true
    }
    SnackbarHost(snackbar, Modifier.padding(top = top))
    val held = printer.held
    if (printer.choosing && held != null) PrinterChoices(printer, held) { save.launch("print.raw") }
}

@Composable
private fun PrinterChoices(printer: PrinterModel, held: PrinterModel.Held, saveRaw: () -> Unit) {
    val activity = LocalContext.current.activity()
    val pages = held.pages
    Dialog({ printer.choosing = false }, DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth().padding(24.dp), shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Column {
                Text("Print received (${held.bytes} bytes)", style = MaterialTheme.typography.titleMedium,
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
