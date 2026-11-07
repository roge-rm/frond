package com.rm.frond

import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner

// The bar over the running machine: its size and what it shows, apart from the drawing in ui/RunningBarView.kt.
object RunningBar {
    const val HEIGHT_DP = 48

    // Power shows only where the device's button bar has a power key (a firmware without one drops it).
    fun powerKey(device: Device) = "power" in device.buttons

    // Sound shows only on devices that make sound.
    fun soundKey(device: Device) = device.sound

    // How far the Palm's surface is moved down: the bar's height in portrait while it shows, and at least
    // the corner buttons' height while the soft keyboard is up.
    fun surfaceTopMargin(shown: Boolean, portrait: Boolean, barPx: Int, imeButtonsPx: Int) =
        maxOf(imeButtonsPx, if (shown && portrait) barPx else 0)

    // The line under the machine's name: the engine on ARM devices, else the device.
    fun subtitle(device: Device, settings: MachineSettings) = if (device.runs68k) device.label else settings.engine.label

    // The corner buttons show while the bar is hidden.
    fun cornerButtons(shown: Boolean) = !shown
}

// SDLActivity is a plain Activity, so a ComposeView in it needs its own lifecycle and saved state.
internal class BarOwner : LifecycleOwner, SavedStateRegistryOwner, ViewModelStoreOwner {
    private val registry = LifecycleRegistry(this)
    private val saved = SavedStateRegistryController.create(this)
    override val viewModelStore = ViewModelStore()
    override val lifecycle: Lifecycle get() = registry
    override val savedStateRegistry: SavedStateRegistry get() = saved.savedStateRegistry

    fun create() {
        saved.performAttach()
        saved.performRestore(null)
        registry.currentState = Lifecycle.State.RESUMED
    }

    fun destroy() {
        registry.currentState = Lifecycle.State.DESTROYED
        viewModelStore.clear()
    }
}
