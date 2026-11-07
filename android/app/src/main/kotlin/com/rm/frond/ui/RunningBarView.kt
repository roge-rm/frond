package com.rm.frond.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
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
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rm.frond.R
import com.rm.frond.RunningBar

// What the bar shows. The activity owns one and changes it on the UI thread.
class RunningBarState {
    var shown by mutableStateOf(true)
    var name by mutableStateOf("")
    var subtitle by mutableStateOf("")
    var sound by mutableStateOf(true)
    var soundKey by mutableStateOf(false)
    var powerKey by mutableStateOf(false)
    var keyboardKey by mutableStateOf(false)
}

class RunningBarActions(
    val back: () -> Unit,
    val toggleSound: () -> Unit,
    val power: () -> Unit,
    val keyboard: () -> Unit,
    val show: () -> Unit,
    val hide: () -> Unit,
    val peripherals: () -> Unit,
)

@Composable
fun RunningBarView(state: RunningBarState, actions: RunningBarActions) {
    FrondTheme(dark = true) {
        if (state.shown) Bar(state, actions) else Handle(actions)
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
            var open by remember { mutableStateOf(false) }
            Box {
                IconButton({ open = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                DropdownMenu(open, { open = false }) {
                    DropdownMenuItem(text = { Text("Peripherals") }, onClick = { open = false; actions.peripherals() })
                    DropdownMenuItem(text = { Text("Hide bar") }, onClick = { open = false; actions.hide() })
                }
            }
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
