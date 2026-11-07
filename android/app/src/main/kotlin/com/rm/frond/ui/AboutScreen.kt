package com.rm.frond.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rm.frond.HomeModel
import com.rm.frond.LicenceEntry
import com.rm.frond.Licences

private const val SOURCE = "https://github.com/roge-rm/frond"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AboutScreen(model: HomeModel) {
    val context = LocalContext.current
    val version = remember {
        runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull() ?: ""
    }
    val entries = remember { runCatching { Licences.entries(context) }.getOrDefault(emptyList()) }
    val uris = LocalUriHandler.current
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("About") },
                navigationIcon = {
                    IconButton(onClick = { model.aboutOpen = false }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("Frond $version", style = MaterialTheme.typography.titleLarge)
            Text("A Palm OS emulator.", Modifier.padding(top = 4.dp))
            Text("Copyright Frond, CloudpilotEmu and uARM authors.", Modifier.padding(top = 12.dp))
            Text("GPL-3.0-or-later, with no warranty.")
            TextButton(onClick = { uris.openUri(SOURCE) }) { Text("github.com/roge-rm/frond") }
            Header("Licences", Modifier.padding(top = 8.dp))
            LicenceRow("GNU General Public License 3", "Frond", listOf(Licences.GPL))
            for (e in entries) LicenceRow(e.name, e.licence, e.files)
        }
    }
}

/** A name and licence that opens to the full notice. */
@Composable
private fun LicenceRow(name: String, licence: String, files: List<String>) {
    val context = LocalContext.current
    var open by remember { mutableStateOf(false) }
    HorizontalDivider()
    ListItem(
        headlineContent = { Text(name) },
        supportingContent = { Text(licence) },
        modifier = Modifier.clickable { open = !open },
    )
    if (open) {
        val text = remember(files) {
            runCatching { Licences.text(context, LicenceEntry(files, name, licence)) }.getOrDefault("Notice missing.")
        }
        SelectionContainer {
            Text(text, Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                fontFamily = FontFamily.Monospace, fontSize = 11.sp, lineHeight = 14.sp)
        }
    }
}
