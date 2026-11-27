package com.rm.frond.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.R
import com.rm.frond.RunningModel
import com.rm.frond.ZaurusPcLink

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ZaurusPcLinkScreen(model: RunningModel) {
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) model.pcLink?.send(uri)
    }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        model.pcLink?.finishSave(uri)
    }
    val pcLink = model.pcLink ?: return
    LaunchedEffect(pcLink.saveName) { pcLink.startSave()?.let { save.launch(it) } }
    Scaffold(
        modifier = Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures {} },
        topBar = {
            TopAppBar(
                title = { Text("PC Link") },
                navigationIcon = {
                    if (pcLink.folder.isNotEmpty()) IconButton(pcLink::back, enabled = pcLink.idle) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Parent folder")
                    }
                },
                actions = {
                    TextButton(enabled = pcLink.idle && pcLink.entries != null, onClick = { pick.launch(arrayOf("*/*")) }) {
                        Text("Send file…")
                    }
                    IconButton(pcLink::refresh, enabled = pcLink.idle) {
                        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                    }
                    IconButton(pcLink::close, enabled = !pcLink.closing) {
                        Icon(Icons.Default.Close, contentDescription = "Close PC Link")
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding)) {
            val folder = pcLink.folder
            Text(if (folder.isEmpty()) "Internal Disk" else "Internal Disk / $folder",
                style = MaterialTheme.typography.titleSmall,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            (if (pcLink.closing) "Disconnecting" else pcLink.operation)?.let { operation ->
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(operation, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }
            pcLink.error?.let {
                Text(it, color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }
            val entries = pcLink.entries
            if (entries?.isEmpty() == true) EmptyNote("No files")
            if (entries != null) LazyColumn(Modifier.fillMaxSize()) {
                items(entries) { entry ->
                    PcLinkEntry(entry, pcLink)
                }
            }
        }
    }
}

@Composable
private fun PcLinkEntry(entry: ZaurusPcLink.Entry, pcLink: ZaurusPcLink) {
    ListItem(
        headlineContent = { Text(entry.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = if (entry.directory) {
            { Icon(painterResource(R.drawable.ic_folder), contentDescription = null) }
        } else null,
        trailingContent = if (entry.directory) null else {
            {
                TextButton({ pcLink.receive(entry.name) }, enabled = pcLink.idle) {
                    Text("Receive…")
                }
            }
        },
        modifier = if (entry.directory) Modifier.clickable(enabled = pcLink.idle) {
            pcLink.openFolder(if (pcLink.folder.isEmpty()) entry.name else "${pcLink.folder}/${entry.name}")
        } else Modifier,
    )
}
