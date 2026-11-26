package com.rm.frond.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import com.rm.frond.Device
import com.rm.frond.DevicePicker

// The device menu: Detect (when given), Recent, then the groups. A group shows its devices under a back row.
@Composable
fun DeviceMenu(expanded: Boolean, onDismiss: () -> Unit, detect: String? = null, onDetect: () -> Unit = {}, onPick: (Device) -> Unit) {
    val context = LocalContext.current
    var group by remember { mutableStateOf<String?>(null) }
    val groups = remember { DevicePicker.groups() }
    val recent = remember(expanded) { DevicePicker.recent(context) }
    DropdownMenu(expanded = expanded, onDismissRequest = { group = null; onDismiss() }) {
        val open = groups.firstOrNull { it.first == group }
        if (open == null) {
            if (detect != null) DropdownMenuItem(text = { Text(detect) }, onClick = { onDetect(); onDismiss() })
            if (recent.isNotEmpty()) {
                DropdownMenuItem(text = { Text("Recent") }, enabled = false, onClick = {})
                for (d in recent) DropdownMenuItem(text = { Text(d.listLabel) }, onClick = { onPick(d); onDismiss() })
            }
            for ((name, _) in groups) DropdownMenuItem(text = { Text(name) }, onClick = { group = name })
        } else {
            DropdownMenuItem(
                text = { Text(open.first) },
                leadingIcon = { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") },
                onClick = { group = null },
            )
            for (d in open.second) DropdownMenuItem(text = { Text(d.listLabel) }, onClick = {
                group = null
                onPick(d)
                onDismiss()
            })
        }
    }
}
