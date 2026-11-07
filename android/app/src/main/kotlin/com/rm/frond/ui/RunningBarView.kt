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
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.R
import com.rm.frond.Device
import com.rm.frond.NetRelay
import com.rm.frond.PeripheralsModel
import com.rm.frond.RunningBar
import com.rm.frond.RunningMenu

// What the bar shows. The activity owns one and changes it on the UI thread.
class RunningBarState {
    var shown by mutableStateOf(true)
    var name by mutableStateOf("")
    var subtitle by mutableStateOf("")
    var sound by mutableStateOf(true)
    var soundKey by mutableStateOf(false)
    var powerKey by mutableStateOf(false)
    var keyboardKey by mutableStateOf(false)
    var menuOpen by mutableStateOf(false)
    // The device and model behind the menu; null until the firmware is resolved.
    var device by mutableStateOf<Device?>(null)
    var model by mutableStateOf<PeripheralsModel?>(null)
    // The confirm or chooser dialog open over the Palm: RESET, HARD_RESET or NETWORK.
    var dialog by mutableStateOf<RunningMenu.Id?>(null)
}

class RunningBarActions(
    val back: () -> Unit,
    val toggleSound: () -> Unit,
    val power: () -> Unit,
    val keyboard: () -> Unit,
    val show: () -> Unit,
    val hide: () -> Unit,
    val openMenu: () -> Unit,
    val menuRow: (RunningMenu.Id) -> Unit,
)

@Composable
fun RunningBarView(state: RunningBarState, actions: RunningBarActions) {
    FrondTheme(dark = true) {
        if (state.shown) Bar(state, actions) else Handle(actions)
        MenuDialogs(state, actions)
    }
}

@Composable
private fun Bar(state: RunningBarState, actions: RunningBarActions) {
    Surface(Modifier.fillMaxWidth().height(RunningBar.HEIGHT_DP.dp), color = MaterialTheme.colorScheme.surfaceContainer) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            IconButton(actions.back) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
            Column(Modifier.weight(1f)) {
                Text(state.name, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(state.subtitle, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
            }
            if (state.keyboardKey) IconButton(actions.keyboard) { Icon(painterResource(R.drawable.ic_keyboard), contentDescription = "Keyboard") }
            if (state.soundKey) IconButton(actions.toggleSound) {
                Icon(painterResource(if (state.sound) R.drawable.ic_volume_up else R.drawable.ic_volume_off), contentDescription = if (state.sound) "Mute" else "Unmute")
            }
            if (state.powerKey) IconButton(actions.power) { Icon(painterResource(R.drawable.ic_power), contentDescription = "Power") }
            Box {
                IconButton(actions.openMenu) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                Menu(state, actions)
            }
        }
    }
}

@Composable
private fun Menu(state: RunningBarState, actions: RunningBarActions) {
    val device = state.device ?: return
    val model = state.model ?: return
    val rows = RunningMenu.rows(device, RunningMenu.State(
        hold = model.hold, keyboard = model.keyboard, beamOff = model.beam.phase == NetRelay.Phase.OFF,
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
private fun Handle(actions: RunningBarActions) {
    run {
        Box(
            Modifier.width(72.dp).height(20.dp)
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
