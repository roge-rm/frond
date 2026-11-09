package com.rm.frond.ui

import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import kotlinx.coroutines.delay
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.rm.frond.R
import com.rm.frond.BarMode
import com.rm.frond.Device
import com.rm.frond.NetRelay
import com.rm.frond.RunningModel
import com.rm.frond.RunningBar
import com.rm.frond.RunningMenu
import com.rm.frond.RunningSheets
import com.rm.frond.SdCard

// What the bar shows. The activity owns one and changes it on the UI thread.
class RunningBarState {
    var shown by mutableStateOf(true)
    var mode by mutableStateOf(BarMode.AUTO_HIDE)
    var portrait by mutableStateOf(true)
    // Counts touches on the bar, so Auto-hide waits from the last one.
    var touches by mutableStateOf(0)
    var name by mutableStateOf("")
    var subtitle by mutableStateOf("")
    var sound by mutableStateOf(true)
    var soundKey by mutableStateOf(false)
    var powerKey by mutableStateOf(false)
    var keyboardKey by mutableStateOf(false)
    var menuOpen by mutableStateOf(false)
    // The device and model behind the menu; null until the firmware is resolved.
    var device by mutableStateOf<Device?>(null)
    var model by mutableStateOf<RunningModel?>(null)
    // The confirm or chooser dialog open over the Palm: RESET, HARD_RESET or NETWORK.
    var dialog by mutableStateOf<RunningMenu.Id?>(null)
    // The sheet open over the Palm, if any.
    var sheet by mutableStateOf<RunningSheets.Sheet?>(null)
}

class RunningBarActions(
    val back: () -> Unit,
    val toggleSound: () -> Unit,
    val power: () -> Unit,
    val keyboard: () -> Unit,
    val show: () -> Unit,
    val hide: () -> Unit,
    val autoHide: () -> Unit,
    val openMenu: () -> Unit,
    val menuRow: (RunningMenu.Id) -> Unit,
    val openSheet: (RunningSheets.Sheet) -> Unit,
    val closeSheet: () -> Unit,
    val filesOpen: (Boolean) -> Unit,
)

@Composable
fun RunningBarView(state: RunningBarState, actions: RunningBarActions) {
    FrondTheme(dark = true) {
        // The view is the whole screen while a sheet is open, so its scrim catches every touch.
        val menuOpen = state.menuOpen
        val sheetOpen = state.sheet != null
        val dialogOpen = state.dialog != null || state.model?.let { it.busy != null || it.path != null || it.log != null } == true
        if (RunningBar.autoHides(state.mode, state.shown, menuOpen, sheetOpen, dialogOpen)) {
            LaunchedEffect(state.touches, menuOpen, sheetOpen, dialogOpen) {
                delay(RunningBar.AUTO_HIDE_MS)
                actions.autoHide()
            }
        }
        Box(if (state.sheet != null) Modifier.fillMaxSize() else Modifier) {
            if (state.shown) Bar(state, actions) else Handle(actions, Modifier.align(Alignment.TopCenter))
            state.sheet?.let { SheetLayer(it, state, actions) }
            state.model?.printer?.let { PrinterPrompt(it, if (state.shown) RunningBar.heightDp(state.portrait).dp else 24.dp) }
        }
        MenuDialogs(state, actions)
    }
}

@Composable
private fun Bar(state: RunningBarState, actions: RunningBarActions) {
    val button = if (state.portrait) 40.dp else 36.dp
    Surface(
        Modifier.fillMaxWidth().height(RunningBar.heightDp(state.portrait).dp).pointerInput(Unit) {
            // Counts every touch on the bar without taking it from the buttons.
            awaitPointerEventScope {
                while (true) {
                    val event = awaitPointerEvent(PointerEventPass.Initial)
                    if (event.changes.any { it.pressed }) state.touches++
                }
            }
        },
        color = MaterialTheme.colorScheme.surfaceContainer,
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            BarButton(actions.back, button) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
            Column(Modifier.weight(1f)) {
                Text(state.name, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(state.subtitle, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
            }
            val device = state.device
            if (state.keyboardKey) BarButton(actions.keyboard, button) { Icon(painterResource(R.drawable.ic_keyboard), contentDescription = "Keyboard") }
            if (device != null) for (sheet in RunningSheets.barSheets(device)) when (sheet) {
                RunningSheets.Sheet.APPLICATIONS -> BarButton({ actions.openSheet(sheet) }, button) { Icon(painterResource(R.drawable.ic_apps), contentDescription = "Applications") }
                RunningSheets.Sheet.CARD -> BarButton({ actions.openSheet(sheet) }, button) {
                    val model = state.model
                    val icon = if (!device.cardSupported) RunningSheets.CardIcon.IN
                    else RunningSheets.cardIcon(model?.state ?: SdCard.State.UNKNOWN, model?.slotShown?.linked != null)
                    Icon(painterResource(cardIconRes(icon)), contentDescription = "Storage")
                }
                else -> Unit
            }
            if (state.soundKey) BarButton(actions.toggleSound, button) {
                Icon(painterResource(if (state.sound) R.drawable.ic_volume_up else R.drawable.ic_volume_off), contentDescription = if (state.sound) "Mute" else "Unmute")
            }
            if (state.powerKey) BarButton(actions.power, button) { Icon(painterResource(R.drawable.ic_power), contentDescription = "Power") }
            Box {
                BarButton(actions.openMenu, button) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                Menu(state, actions)
            }
        }
    }
}

@Composable
private fun BarButton(onClick: () -> Unit, size: Dp, content: @Composable () -> Unit) =
    IconButton(onClick, Modifier.size(size), content = content)

@Composable
private fun Menu(state: RunningBarState, actions: RunningBarActions) {
    val device = state.device ?: return
    val model = state.model ?: return
    val rows = RunningMenu.rows(device, RunningMenu.State(
        hold = model.hold, keyboard = model.keyboard, beamOff = !model.beam.active,
        microphone = model.microphone, network = model.network, busy = model.busy != null,
    ))
    DropdownMenu(state.menuOpen, { state.menuOpen = false }) {
        var group = rows.first().group
        for (row in rows) {
            if (row.group != group) { HorizontalDivider(); group = row.group }
            DropdownMenuItem(
                text = { Text(row.label, color = if (row.destructive) MaterialTheme.colorScheme.error else Color.Unspecified) },
                trailingIcon = row.value?.let { value ->
                    { Text(value, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
                },
                enabled = row.enabled,
                onClick = {
                    state.menuOpen = false
                    when (row.id) {
                        RunningMenu.Id.RESET, RunningMenu.Id.HARD_RESET, RunningMenu.Id.NETWORK -> state.dialog = row.id
                        else -> actions.menuRow(row.id)
                    }
                },
            )
        }
    }
}

// Dialogs from the menu. They show whether the bar or its handle is up.
@Composable
private fun MenuDialogs(state: RunningBarState, actions: RunningBarActions) {
    val device = state.device ?: return
    val model = state.model ?: return
    BusyDialog(model)
    AlertBox(model)
    CardPickerDialogs(model)
    if (model.path != null) {
        val snackbar = remember { SnackbarHostState() }
        // The file view covers the Palm; the core stays quiet while a Memory Stick is open.
        DisposableEffect(Unit) {
            actions.filesOpen(true)
            onDispose { actions.filesOpen(false) }
        }
        Dialog({ if (model.path == "/") model.leaveFiles() else model.up() }, DialogProperties(usePlatformDefaultWidth = false)) {
            Surface(Modifier.fillMaxSize()) { CardFilesScreen(model, snackbar) }
        }
    }
    LaunchedEffect(model.path != null) { if (model.path != null) actions.closeSheet() }
    model.internal?.let { files ->
        val snackbar = remember { SnackbarHostState() }
        Dialog({ files.leaveFiles() }, DialogProperties(usePlatformDefaultWidth = false)) {
            Surface(Modifier.fillMaxSize()) { CardFilesScreen(files, snackbar) }
        }
        files.alert?.let { (title, text) ->
            AlertDialog(onDismissRequest = { files.alert = null }, title = { Text(title) }, text = { Text(text) },
                confirmButton = { TextButton(onClick = { files.alert = null }) { Text("OK") } })
        }
        val context = LocalContext.current
        LaunchedEffect(files.note) {
            files.note?.let {
                Toast.makeText(context, it, Toast.LENGTH_SHORT).show()
                files.note = null
            }
        }
    }
    LaunchedEffect(model.internal != null) { if (model.internal != null) actions.closeSheet() }
    val context = LocalContext.current
    LaunchedEffect(model.note) {
        model.note?.let {
            Toast.makeText(context, it, Toast.LENGTH_SHORT).show()
            model.note = null
        }
    }
    when (state.dialog) {
        RunningMenu.Id.RESET -> ResetDialog(device, { state.dialog = null }) { state.dialog = null; model.reset() }
        RunningMenu.Id.HARD_RESET -> HardResetDialog(device, { state.dialog = null }) { saveFirst ->
            state.dialog = null
            model.hardReset(saveFirst)
        }
        RunningMenu.Id.NETWORK -> NetworkDialog(model.network, { state.dialog = null }) { mode ->
            state.dialog = null
            model.changeNetwork(mode)
        }
        else -> Unit
    }
    model.log?.let { log ->
        Dialog({ model.log = null }, DialogProperties(usePlatformDefaultWidth = false)) {
            Surface(Modifier.fillMaxSize()) { LogScreen(log, onClose = { model.log = null }) }
        }
    }
}

// A small tab at the top edge; a tap or a swipe down brings the bar back.
@Composable
private fun Handle(actions: RunningBarActions, modifier: Modifier = Modifier) {
    run {
        Box(
            modifier.width(72.dp).height(20.dp)
                .background(MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.8f), RoundedCornerShape(bottomStart = 10.dp, bottomEnd = 10.dp))
                .semantics { contentDescription = "Show bar" }
                .clickable { actions.show() }
                .pointerInput(Unit) {
                    var drag = 0f
                    detectVerticalDragGestures(onDragStart = { drag = 0f }, onVerticalDrag = { _, y ->
                        drag += y
                        if (drag > 12.dp.toPx()) { drag = Float.NEGATIVE_INFINITY; actions.show() }
                    })
                },
            contentAlignment = Alignment.Center,
        ) {
            Box(Modifier.width(28.dp).height(3.dp).background(MaterialTheme.colorScheme.onSurfaceVariant, RoundedCornerShape(2.dp)))
        }
    }
}
