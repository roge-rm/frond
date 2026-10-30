package com.rm.frond.ui

import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
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

// Picks a device's NOR and NAND images with the system file picker, checks them and copies them in.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FirmwareScreen(model: HomeModel, device: Device) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var nor by remember { mutableStateOf<Uri?>(null) }
    var nand by remember { mutableStateOf<Uri?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val installed = model.firmwareReady[device] == true

    val pickNor = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) { nor = uri; error = null }
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
                title = { Text("${device.label} firmware") },
                navigationIcon = {
                    IconButton(onClick = { model.firmwareFor = null }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            val current = if (installed) "Installed" else "Not chosen"
            ListItem(
                headlineContent = { Text("NOR image") },
                supportingContent = { Text(name(nor) ?: current, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingContent = { TextButton(onClick = { pickNor.launch(arrayOf("*/*")) }, enabled = !busy) { Text("Choose") } },
            )
            val nandNow = if (!device.nandRequired && installed && !Firmware.nand(context, device).isFile) "None" else current
            ListItem(
                headlineContent = { Text(if (device.nandRequired) "NAND image" else "NAND image (optional)") },
                supportingContent = { Text(name(nand) ?: nandNow, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                trailingContent = { TextButton(onClick = { pickNand.launch(arrayOf("*/*")) }, enabled = !busy) { Text("Choose") } },
            )
            error?.let {
                Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }
            Button(
                enabled = !busy && nor != null && (nand != null || !device.nandRequired),
                shape = MaterialTheme.shapes.small,
                onClick = {
                    val n = nor ?: return@Button
                    busy = true
                    error = null
                    scope.launch {
                        val result = withContext(Dispatchers.IO) { Firmware.import(context, device, n, nand) }
                        busy = false
                        model.reload()
                        if (result == null) model.firmwareFor = null else error = result
                    }
                },
                modifier = Modifier.fillMaxWidth().padding(16.dp),
            ) { Text(if (busy) "Copying" else "Import") }
            if (busy) Progress()
        }
    }
}
