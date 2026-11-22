package com.rm.frond

import android.app.Activity
import android.content.Intent
import android.os.Handler
import android.os.Looper
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.ActivityResultRegistryOwner
import androidx.activity.result.contract.ActivityResultContract
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.ActivityOptionsCompat
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

    // Landscape has less height to spare, so its bar is shorter.
    const val LANDSCAPE_HEIGHT_DP = 40

    // The least height the Palm keeps when the bar takes its own space; with less room the bar overlays.
    const val MIN_PALM_DP = 200

    // Auto-hide mode hides the bar after this long without a touch on it.
    const val AUTO_HIDE_MS = 4000L

    fun heightDp(portrait: Boolean) = if (portrait) HEIGHT_DP else LANDSCAPE_HEIGHT_DP

    // Whether the bar takes its own space above the Palm: Own space mode, shown, and the Palm keeps minPalmPx.
    fun takesSpace(mode: BarMode, shown: Boolean, barPx: Int, availablePx: Int, minPalmPx: Int) =
        mode == BarMode.OWN_SPACE && shown && availablePx - barPx >= minPalmPx

    // How far the Palm's surface is moved down: the bar's height when it takes its own space, and at least
    // the corner button's height while the soft keyboard is up.
    fun surfaceTopMargin(mode: BarMode, shown: Boolean, barPx: Int, availablePx: Int, minPalmPx: Int, imeButtonsPx: Int, touchHidden: Boolean = false) =
        if (touchHidden) 0 else maxOf(imeButtonsPx, if (takesSpace(mode, shown, barPx, availablePx, minPalmPx)) barPx else 0)

    // Whether an input device is a game controller: gamepad or joystick buttons, and not an on-screen one.
    fun isGameController(sources: Int, virtual: Boolean): Boolean {
        if (virtual) return false
        val pad = android.view.InputDevice.SOURCE_GAMEPAD
        val stick = android.view.InputDevice.SOURCE_JOYSTICK
        return sources and pad == pad || sources and stick == stick
    }

    // The on-screen controls (the bar, its handle, the keyboard button and the core's jog panels) go away
    // while the setting is on and a controller is connected.
    fun touchHidden(setting: Boolean, controllerConnected: Boolean) = setting && controllerConnected

    enum class BackAction { CLOSE_SHEET, CLOSE_MENU, SHOW_BAR, HIDE_BAR, LEAVE }

    // What Back does. With the touch controls hidden it opens the bar (the way to the menu) and closes it
    // again, and doesn't leave the machine.
    fun backAction(touchHidden: Boolean, peeking: Boolean, sheetOpen: Boolean, menuOpen: Boolean) = when {
        sheetOpen -> BackAction.CLOSE_SHEET
        menuOpen -> BackAction.CLOSE_MENU
        touchHidden && peeking -> BackAction.HIDE_BAR
        touchHidden -> BackAction.SHOW_BAR
        else -> BackAction.LEAVE
    }

    // The bar hides by itself only in Auto-hide mode, and not while a menu, sheet or dialog is open.
    fun autoHides(mode: BarMode, shown: Boolean, menuOpen: Boolean, sheetOpen: Boolean, dialogOpen: Boolean) =
        mode == BarMode.AUTO_HIDE && shown && !menuOpen && !sheetOpen && !dialogOpen

    // A side sheet in landscape, a bottom sheet in portrait.
    fun sideSheet(portrait: Boolean) = !portrait

    // The side sheet's width: 360 dp, at most 45% of the screen.
    fun sideSheetWidthDp(screenWidthDp: Int) = minOf(360, screenWidthDp * 45 / 100)

    // The line under the machine's name: the engine on ARM devices, else the device.
    fun subtitle(device: Device, settings: MachineSettings) = if (device.runs68k) device.label else settings.engine.label

    // Where the hidden bar's handle sits: top centre in portrait, where the Palm starts below it, and the
    // top start corner in landscape, beside the Palm, which is fitted to the height and centred.
    fun handleCentred(portrait: Boolean) = portrait

    // The hidden bar's touch area around its tab.
    const val HANDLE_TOUCH_WIDTH_DP = 96
    const val HANDLE_TOUCH_HEIGHT_DP = 40

    // The corner keyboard button shows while the bar is hidden.
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

// Lets the bar's Compose code use activity result launchers in a plain Activity: launches go through
// startActivityForResult and the activity hands results back to dispatchResult.
internal class BarResults(private val activity: Activity) : ActivityResultRegistry(), ActivityResultRegistryOwner {
    override val activityResultRegistry get() = this

    override fun <I, O> onLaunch(requestCode: Int, contract: ActivityResultContract<I, O>, input: I, options: ActivityOptionsCompat?) {
        val direct = contract.getSynchronousResult(activity, input)
        if (direct != null) {
            Handler(Looper.getMainLooper()).post { dispatchResult(requestCode, direct.value) }
            return
        }
        val intent = contract.createIntent(activity, input)
        if (isPermissionRequest(intent.action)) {
            // startActivityForResult has no activity for this action; the system dialog is requestPermissions.
            activity.requestPermissions(intent.getStringArrayExtra(PERMISSIONS) ?: emptyArray(), requestCode)
            return
        }
        @Suppress("DEPRECATION")
        activity.startActivityForResult(intent, requestCode, options?.toBundle())
    }

    // The activity's onRequestPermissionsResult, in the intent form the permission contracts parse.
    fun dispatchPermissions(requestCode: Int, permissions: Array<String>, grantResults: IntArray): Boolean =
        dispatchResult(
            requestCode, Activity.RESULT_OK,
            Intent().putExtra(PERMISSIONS, permissions).putExtra(GRANT_RESULTS, grantResults),
        )

    companion object {
        private const val PERMISSIONS = ActivityResultContracts.RequestMultiplePermissions.EXTRA_PERMISSIONS
        private const val GRANT_RESULTS = ActivityResultContracts.RequestMultiplePermissions.EXTRA_PERMISSION_GRANT_RESULTS

        fun isPermissionRequest(action: String?) = action == ActivityResultContracts.RequestMultiplePermissions.ACTION_REQUEST_PERMISSIONS
    }
}
