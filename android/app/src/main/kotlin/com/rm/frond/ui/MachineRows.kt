package com.rm.frond.ui

import android.content.Context
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.rm.frond.CpuSpeed
import com.rm.frond.Device
import com.rm.frond.Engine
import com.rm.frond.Firmware
import com.rm.frond.JogControls
import com.rm.frond.MachineSettings
import com.rm.frond.ScreenSize
import java.io.File
import kotlin.math.roundToInt

// Per-machine settings rows shared by the Edit settings screen (form style) and the running menu (list style).
// note is a one-clause line under a row, used for "Applies at next start".
const val APPLIES_AT_NEXT_START = "Applies at next start"

fun Device.isTg50() = id == "sony-clie-tg50"

// Whether Sony audio preview (TG50) or Low-latency audio (others) can be used with this firmware. Blocking.
fun audioPreviewQualified(context: Context, device: Device, session: File?): Boolean =
    if (device.isTg50()) Firmware.sonyPreviewEligible(device, session, Firmware.nor(context, device))
    else Firmware.fastAudioQualified(device, session, Firmware.nor(context, device))

fun audioPreviewLabel(device: Device) = if (device.isTg50()) "Sony audio preview" else "Low-latency audio"

@Composable
fun MachineSwitchRow(
    label: String, checked: Boolean, listStyle: Boolean, enabled: Boolean = true, note: String? = null,
    onChange: (Boolean) -> Unit,
) {
    if (!listStyle) {
        SwitchRow(label, checked, enabled, onChange)
        return
    }
    val faded = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
    ListItem(
        headlineContent = { Text(label, color = if (enabled) Color.Unspecified else faded) },
        supportingContent = if (note != null && enabled) ({ Text(note) }) else null,
        trailingContent = { Switch(checked = checked, enabled = enabled, onCheckedChange = onChange) },
        modifier = if (enabled) Modifier.clickableRow { onChange(!checked) } else Modifier,
    )
}

@Composable
fun <T> MachineChoiceRow(
    label: String, value: T, options: List<T>, name: (T) -> String, listStyle: Boolean, onChange: (T) -> Unit,
) {
    if (!listStyle) {
        Choice(label, value, options, name, onChange)
        return
    }
    var open by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth()) {
        ListItem(
            headlineContent = { Text(label) },
            supportingContent = { Text(name(value)) },
            modifier = Modifier.clickableRow { open = true },
        )
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (o in options) DropdownMenuItem(text = { Text(name(o)) }, onClick = { open = false; onChange(o) })
        }
    }
}

// Size, Smooth pixels, Contrast on 68k devices, and on the TG50 Show keyboard and Jog controls.
@Composable
fun MachineDisplayRows(device: Device, settings: MachineSettings, listStyle: Boolean, onChange: (MachineSettings) -> Unit) {
    MachineChoiceRow("Size", settings.size, ScreenSize.entries, { it.label }, listStyle) { onChange(settings.copy(size = it)) }
    MachineSwitchRow("Smooth pixels", settings.smoothing, listStyle) { onChange(settings.copy(smoothing = it)) }
    if (device.runs68k) MachineContrastRow(settings, listStyle, onChange)
    if (device.isTg50()) {
        MachineSwitchRow("Show keyboard", settings.showKeyboard, listStyle) { onChange(settings.copy(showKeyboard = it)) }
        MachineChoiceRow("Jog controls", settings.jogControls, JogControls.entries, { it.label }, listStyle) {
            onChange(settings.copy(jogControls = it))
        }
    }
}

@Composable
fun MachineSoundRow(device: Device, settings: MachineSettings, listStyle: Boolean, onChange: (MachineSettings) -> Unit) {
    MachineSwitchRow("Sound", device.sound && settings.sound, listStyle, enabled = device.sound) {
        onChange(settings.copy(sound = it))
    }
}

// Sony audio preview on the TG50, Low-latency audio elsewhere. qualified is null while being checked.
@Composable
fun MachineAudioPreviewRow(
    device: Device, settings: MachineSettings, qualified: Boolean?, listStyle: Boolean, note: String? = null,
    onChange: (MachineSettings) -> Unit,
) {
    val usable = qualified == true
    if (device.isTg50()) {
        MachineSwitchRow("Sony audio preview", usable && settings.sonyAudioPreview, listStyle, usable, note) {
            onChange(settings.copy(sonyAudioPreview = it))
        }
    } else {
        MachineSwitchRow("Low-latency audio", usable && settings.fastAudio, listStyle, usable, note) {
            onChange(settings.copy(fastAudio = it))
        }
    }
}

// Exact or Fast. Only shown in debug builds until the Fast engine is qualified.
@Composable
fun MachineEngineRow(settings: MachineSettings, listStyle: Boolean, onChange: (MachineSettings) -> Unit) {
    MachineChoiceRow("Engine", settings.engine, Engine.entries, { it.label }, listStyle) { onChange(settings.copy(engine = it)) }
}

// The LCD contrast, 0 to 100. The change is saved when the slider is let go.
@Composable
fun MachineContrastRow(settings: MachineSettings, listStyle: Boolean, onChange: (MachineSettings) -> Unit) {
    var value by remember(settings.contrast) { mutableFloatStateOf(settings.contrast.toFloat()) }
    val slider = @Composable {
        Slider(
            value = value,
            onValueChange = { value = it },
            valueRange = 0f..100f,
            onValueChangeFinished = {
                val next = value.roundToInt()
                if (next != settings.contrast) onChange(settings.copy(contrast = next))
            },
        )
    }
    if (listStyle) {
        ListItem(headlineContent = { Text("Contrast: ${value.roundToInt()}") }, supportingContent = { slider() })
    } else {
        Column(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
            Text("Contrast: ${value.roundToInt()}", style = MaterialTheme.typography.bodyLarge)
            slider()
        }
    }
}

// Original or Uncapped, for 68k devices.
@Composable
fun MachineSpeedRow(settings: MachineSettings, listStyle: Boolean, onChange: (MachineSettings) -> Unit) {
    MachineChoiceRow("Speed", settings.speed, CpuSpeed.entries, { it.label }, listStyle) { onChange(settings.copy(speed = it)) }
}
