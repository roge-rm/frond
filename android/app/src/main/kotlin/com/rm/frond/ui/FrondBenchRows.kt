package com.rm.frond.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.rm.frond.FrondBench

/** FrondBench, the benchmark app: Install on This Device (when onInstall is given) and Save. */
@Composable
fun FrondBenchRows(enabled: Boolean, onInstall: (() -> Unit)?) {
    val context = LocalContext.current
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null && !FrondBench.save(context, uri)) Toast.makeText(context, "Couldn't save the file.", Toast.LENGTH_LONG).show()
    }
    HorizontalDivider(Modifier.padding(top = 8.dp))
    ListItem(headlineContent = { Text("FrondBench") })
    Row(Modifier.padding(horizontal = 8.dp)) {
        if (onInstall != null) TextButton(enabled = enabled, onClick = onInstall) { Text("Install on This Device") }
        TextButton(onClick = { save.launch(FrondBench.FILE_NAME) }) { Text("Save FrondBench.prc…") }
    }
}
