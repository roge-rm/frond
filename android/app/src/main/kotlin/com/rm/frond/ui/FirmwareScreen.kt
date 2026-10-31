package com.rm.frond.ui

import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.Device
import com.rm.frond.Firmware
import com.rm.frond.HomeModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Picks a NOR and NAND image with the system file picker, finds the device from the NOR like the
// desktop (or takes the chosen one), checks them and copies them in.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FirmwareScreen(model: HomeModel) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var nor by remember { mutableStateOf<Uri?>(null) }
    var nand by remember { mutableStateOf<Uri?>(null) }
    // The device picked from the menu, or null to detect it from the NOR.
    var chosen by remember { mutableStateOf(model.startAfterImport) }
    var detected by remember { mutableStateOf<Device?>(null) }
    var choosing by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val device = chosen ?: detected

    DisposableEffect(Unit) { onDispose { Firmware.picked(context).delete() } }

    val pickNor = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        nor = null
        detected = null
        error = null
        busy = true
        scope.launch {
            val (failed, found) = withContext(Dispatchers.IO) {
                val e = Firmware.pick(context, uri)
                e to if (e == null) Firmware.detect(Firmware.picked(context)) else null
            }
            busy = false
            if (failed != null) {
                error = failed
                return@launch
            }
            nor = uri
            detected = found
            if (found == null && chosen == null) error = Firmware.NOT_DETECTED
        }
    }
    val pickNand = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) { nand = uri; error = null }
    }

    fun name(uri: Uri?): String? = uri?.let {
        try {
            context.contentResolver.query(it, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0) else null
            }
        } catch (e: Exception) {
            null
        } ?: it.lastPathSegment
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Import firmware") },
                navigationIcon = {
                    IconButton(onClick = { model.closeFirmware() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            val shown = when {
                chosen != null -> chosen!!.label
                detected != null -> "${detected!!.label}, from the NOR image"
                else -> "Detect from the NOR image"
            }
            ListItem(
                headlineContent = { Text("Device") },
                supportingContent = { Text(shown, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingContent = {
                    Box {
                        TextButton(onClick = { choosing = true }, enabled = !busy) { Text("Change") }
                        DropdownMenu(expanded = choosing, onDismissRequest = { choosing = false }) {
                            DropdownMenuItem(text = { Text("Detect from the NOR image") }, onClick = {
                                choosing = false
                                chosen = null
                                error = if (nor != null && detected == null) Firmware.NOT_DETECTED else null
                            })
                            for (d in Device.shown) {
                                DropdownMenuItem(text = { Text(d.label) }, onClick = {
                                    choosing = false
                                    chosen = d
                                    error = null
                                })
                            }
                        }
                    }
                },
            )
            ListItem(
                headlineContent = { Text("NOR image") },
                supportingContent = { Text(name(nor) ?: "Not chosen", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingContent = { TextButton(onClick = { pickNor.launch(arrayOf("*/*")) }, enabled = !busy) { Text("Choose") } },
            )
            ListItem(
                headlineContent = { Text(if (device?.nandRequired == false) "NAND image (optional)" else "NAND image") },
                supportingContent = { Text(name(nand) ?: "Not chosen", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingContent = { TextButton(onClick = { pickNand.launch(arrayOf("*/*")) }, enabled = !busy) { Text("Choose") } },
            )
            error?.let {
                Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }
            Button(
                enabled = !busy && nor != null && device != null && (nand != null || !device.nandRequired),
                shape = MaterialTheme.shapes.small,
                onClick = {
                    val d = device ?: return@Button
                    busy = true
                    error = null
                    scope.launch {
                        val result = withContext(Dispatchers.IO) { Firmware.import(context, d, nand) }
                        busy = false
                        model.reload()
                        if (result != null) {
                            error = result
                            return@launch
                        }
                        val thenStart = model.startAfterImport != null
                        model.closeFirmware()
                        if (thenStart) model.newSession(d)
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(16.dp),
            ) { Text(if (busy) "Copying" else "Import") }
            if (busy) Progress()
        }
    }
}
