package com.rm.frond.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

// The core's output for the running session, like the desktop's View > Emulator Log.
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogScreen(log: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val vertical = rememberScrollState()
    // Opens at the newest lines.
    LaunchedEffect(log) { vertical.scrollTo(vertical.maxValue) }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Emulator log") },
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    TextButton(onClick = {
                        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                        clipboard.setPrimaryClip(ClipData.newPlainText("Emulator log", log))
                        scope.launch { snackbar.showSnackbar("Log copied") }
                    }) { Text("Copy") }
                    IconButton(onClick = {
                        val send = Intent(Intent.ACTION_SEND).setType("text/plain")
                            .putExtra(Intent.EXTRA_SUBJECT, "Frond emulator log")
                            .putExtra(Intent.EXTRA_TEXT, log)
                        context.startActivity(Intent.createChooser(send, null))
                    }) {
                        Icon(Icons.Default.Share, contentDescription = "Share")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        if (log.isEmpty()) {
            Text("Nothing logged yet", Modifier.padding(padding).padding(16.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            SelectionContainer(Modifier.padding(padding).fillMaxSize()) {
                Text(
                    log,
                    Modifier.verticalScroll(vertical).horizontalScroll(rememberScrollState()).padding(12.dp),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    lineHeight = 16.sp,
                )
            }
        }
    }
}
