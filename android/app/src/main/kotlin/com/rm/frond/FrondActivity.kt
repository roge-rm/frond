package com.rm.frond

import android.app.AlertDialog
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.res.Configuration
import android.hardware.input.InputManager
import android.view.InputDevice
import android.media.AudioManager
import android.system.Os
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.widget.RelativeLayout
import android.widget.TextView
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.util.Log
import android.window.OnBackInvokedDispatcher
import androidx.core.view.ViewCompat
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import java.io.File

// Runs the native core through SDL in its own process (":emulator"), so every start gets
// fresh native state. libmain.so holds SDL_main. HomeActivity starts it with the extras
// below; back saves the session through the core's control socket and returns home. Going to
// the background pauses the core (patch 0088) and saves the session, so a process kill keeps it.
open class FrondActivity : SDLActivity() {
    private lateinit var link: ControlLink
    private var sessionPath: String? = null
    @Volatile private var leaving = false
    @Volatile private var quitSent = false
    // Set when the final save on leaving succeeded: a linked card then goes back to Cards.
    @Volatile private var finalSaveOk = false
    private lateinit var settings: Settings
    private var machineSettings = MachineSettings()
    private var savingNote: TextView? = null
    // The core's loopback gdb port while the debugger is on (Settings), else null, and the socket holding it.
    private var debugPort: Int? = null
    private var debugReservation: java.net.Socket? = null
    // Set once DebugLink is stopped for good, so a late start can't open it again. Under debugLock.
    private var debugStopped = false
    private val debugLock = Any()
    // Set in onDestroy; ends the wait to release the core's gdb port.
    @Volatile private var destroyed = false
    // Why the core wasn't started, for the home screen.
    @Volatile private var startRefused: String? = null
    private var keyboardButton: View? = null
    // The running bar over the Palm: its state, view and the owner its ComposeView needs.
    private val bar = com.rm.frond.ui.RunningBarState()
    private var barView: androidx.compose.ui.platform.ComposeView? = null
    private var barOwner: BarOwner? = null
    private var barResults: BarResults? = null
    // The menu's and sheets' model, on the control link.
    private var menuModel: RunningModel? = null
    private var imeVisible = false
    @Volatile private var pendingMicrophone = false
    @Volatile private var foreground = false
    @Volatile private var microphoneResume = 0L
    // Over the Palm (the menu in the bar) the request starts at once; behind another screen it waits for the return.
    fun microphoneOnReturn(on: Boolean) {
        pendingMicrophone = on
        if (on && foreground) runOnUiThread { startPendingMicrophone() }
    }

    private fun startPendingMicrophone() {
        if (leaving || !pendingMicrophone) return
        pendingMicrophone = false
        val owner = this
        val resume = microphoneResume
        Thread {
            val epoch = link.request("microphone-epoch", timeoutSeconds = 10)
            if (epoch.ok && running === owner && foreground && !leaving && microphoneResume == resume)
                link.request("microphone-mode", "on-return:${epoch.message}", timeoutSeconds = 10)
            runOnUiThread { menuModel?.settleMicrophone() }
        }.apply { isDaemon = true }.start()
    }
    private var keyboard: PalmKeyboard? = null
    @Volatile private var danaState: DanaViewState? = null
    private var danaEnvironment = ""
    private var lastDanaKeyboard = DanaKeyboard.AUTO
    private var inputDevices: InputManager? = null
    private var inputListener: InputManager.InputDeviceListener? = null
    // The Palm infrared keyboard, as the core last said.
    @Volatile private var keyboardAttached = false
    @Volatile private var stopped = false
    val inBackground get() = stopped
    // One save at a time: the background save and the save on back.
    private val saveLock = Any()
    // Sound as last sent to the core; Settings can change it while the Palm runs. UI thread only.
    @Volatile private var sound = true
    // Sends sound and display changes to the core in order.
    private val soundSender = java.util.concurrent.Executors.newSingleThreadExecutor {
        Thread(it, "FrondSound").apply { isDaemon = true }
    }
    // The display settings as last sent to the core, as its display command argument. Set on the UI thread.
    @Volatile private var display = ""
    // Game controller buttons and sticks to the Palm's buttons. UI thread only.
    private var controller = ControllerInput(ControllerMap(emptyMap())) { _, _ -> }
    lateinit var runningDevice: Device
        private set

    // The core library SDLActivity loads, and the control socket's file name in app storage.
    protected open val coreLibrary get() = "main"
    protected open val controlSocket get() = "control.sock"
    // The core doesn't answer the control link until gdb first attaches (ARM). The 68k core does.
    protected open val gdbBlocksLink get() = true

    override fun getLibraries(): Array<String> = arrayOf("c++_shared", "SDL2", "SDL2_image", coreLibrary)

    override fun onCreate(savedInstanceState: Bundle?) {
        // The core names a new machine's file on its first save, so an older path may not be the one in use.
        sessionPath = Sessions.resolve(this, intent?.getStringExtra(EXTRA_SESSION))
        machineSettings = Sessions.forPath(this, sessionPath)?.settings ?: MachineSettings()
        val device = launchDevice()
        runningDevice = device.unresolvedFirmware()
        settings = machineSettings.applyTo(Settings.from(this, intent))
        sound = launchDevice().sound && settings.sound
        display = machineSettings.displayArgument(tg50Controls = launchDevice().id == "sony-clie-tg50", m68k = launchDevice().runs68k)
        requestedOrientation = settings.orientation.request
        File(filesDir, START_ERROR).delete()
        link = ControlLink(File(filesDir, controlSocket)) { name, state ->
            if (name == "close") leave()
            if (name == "microphone") Log.i(TAG, "microphone: $state")
            if (name == "dana-view" && launchDevice().isDana()) {
                DanaViewState.parse(state)?.let { acceptDanaState(it) }
            }
        }
        chooseAudioDriver()
        // SDL keeps its thread running while paused; the core idles itself in the background and
        // still answers the control socket, so it can save. SDL pauses the audio device.
        Os.setenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0", true)
        Os.setenv("SDL_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO", "1", true)
        HostDns.start(applicationContext)
        // A btvirt and sockets left by an emulator that was killed.
        BtLink.cleanLater(applicationContext)
        super.onCreate(savedInstanceState)
        fitInsideSystemBars()
        requestFrameRate()
        addSavingNote()
        startDebugger()
        addKeyboard()
        addController()
        if (device.isDana()) {
            keyboardButton?.visibility = View.VISIBLE
            link.whenConnected { if (link.connectedOnce) updateDanaEnvironment() }
        }
        SdCard.clean(this)
        // An edited card kept from a kill goes back into the Palm once the core answers, which with the debugger on
        // is after gdb first connects.
        link.whenConnected {
            Thread { SdCard.restorePending(applicationContext, link, sessionPath) }.apply { isDaemon = true }.start()
        }
        // The Dana's USB port gets the machine's printer or Wi-Fi adapter at every start.
        UsbPort.startRequest(UsbPort.forStart(launchDevice(), machineSettings.usbPort))?.let { (command, argument) ->
            link.whenConnected {
                Thread { link.request(command, argument, timeoutSeconds = 30).takeIf { !it.ok }?.let { Log.w(TAG, "usb port: ${it.message}") } }
                    .apply { isDaemon = true }.start()
            }
        }
        // Infrared, serial and Bluetooth over a connection are set from the machine's settings at every start.
        val infraredPhone = machineSettings.infrared == InfraredChoice.PHONE && launchDevice().irPhone
        val infraredLink = InfraredChoice.linkId(machineSettings.infrared) != null || infraredPhone
        val serialModem = machineSettings.serial == InfraredChoice.MODEM && launchDevice().modem
        val serialLink = (InfraredChoice.linkId(machineSettings.serial) != null && launchDevice().serialPort) || serialModem
        val bluetoothLink = machineSettings.bluetooth != BluetoothChoice.OFF && launchDevice().bluetooth
        if (infraredLink || serialLink || bluetoothLink) {
            if ((infraredLink && !infraredPhone) || (serialLink && !serialModem) || InfraredChoice.linkId(machineSettings.bluetooth) != null) askLocalNetwork()
            link.whenConnected {
                Thread({
                    val name = Sessions.forPath(applicationContext, sessionPath)?.name ?: launchDevice().label
                    if (infraredLink) IrLink.apply(applicationContext, link, machineSettings.infrared, name, launchDevice().label)
                        ?.let { Log.w(TAG, "infrared: $it") }
                    if (serialLink) SerialLink.apply(applicationContext, link, machineSettings.serial, name, launchDevice().label)
                        ?.let { Log.w(TAG, "serial: $it") }
                    if (bluetoothLink) BluetoothLink.apply(applicationContext, link, machineSettings.bluetooth, name, launchDevice().label)
                        ?.let { Log.w(TAG, "bluetooth: $it") }
                }, "FrondLinks").apply { isDaemon = true }.start()
            }
        }
        running = this
        resolveRunningDevice()
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) logSpeed()
        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { back() }
        }
    }

    // The core's command line. With a saved session file the core loads it; otherwise it starts
    // the device from the NOR and NAND extras (files/firmware/palm-tx/ when they're missing).
    override fun getArguments(): Array<String> {
        val extras = intent?.extras
        val arguments = mutableListOf<String>()
        val session = sessionPath
        // Any session file in the machine's folder loads, so a fresh start can't save over it.
        val saved = session?.let { path ->
            File(path).takeIf { it.isFile || it.isDirectory } ?: Sessions.forPath(this, path)?.file?.takeIf { it.isFile }
        }
        if (saved != null) {
            arguments += saved.path
            arguments += cardArguments()
        } else {
            val device = Device.byId(extras?.getString(EXTRA_DEVICE)) ?: Device.TX
            val nor = extras?.getString(EXTRA_NOR) ?: Firmware.nor(this, device).path
            val nand = extras?.getString(EXTRA_NAND) ?: Firmware.nand(this, device).path
            arguments += device.coreFlags
            arguments += nor
            if (File(nand).isFile) arguments += listOf("--nand", nand)
            // A new machine runs in its own folder, where cards above 128 MB live until the first save.
            if (!device.runs68k) session?.let { File(it).parentFile }?.takeIf { (it.isDirectory || it.mkdirs()) && it.canonicalFile != Sessions.folder(this).canonicalFile }?.let {
                arguments += listOf("--work-folder", it.path)
            }
            // Not saved yet: only the run lock, as there's no slot to load.
            cardArguments()
        }
        arguments += machineSettings.arguments(soundSupported = launchDevice().sound,
            tg50Controls = launchDevice().id == "sony-clie-tg50", m68k = launchDevice().runs68k)
        if (launchDevice().isDana()) {
            arguments += listOf("--dana-view", machineSettings.danaView.argument() + " " + currentDanaEnvironment())
        }
        arguments += listOf("--window-background", BACKGROUND_HEX)
        arguments += machineSettings.audioArguments(launchDevice().lowLatencyAudio, audioPeriod()) { fastAudioQualified() }
        debugPort?.let { arguments += listOf("--gdb", it.toString()) }
        arguments += listOf("--desktop-socket", link.path.path)
        return arguments.toTypedArray()
    }

    private fun launchDevice() = Device.byId(intent?.getStringExtra(EXTRA_DEVICE)) ?: Device.TX

    // The machine's folder in the sessions folder, or null for a session elsewhere.
    private fun machineFolder(): File? = Sessions.forPath(this, sessionPath)?.folder

    // SDL asks for the main function first, on its thread. A machine in the sessions folder only
    // starts once this process holds its .run.lock (10 s wait), so the home screen leaves its slot
    // alone. Without it no core runs: SDL finds no such function, returns and ends the activity,
    // and the home screen shows startRefused.
    override fun getMainFunction(): String {
        val folder = machineFolder() ?: return super.getMainFunction()
        val deadline = System.currentTimeMillis() + 10_000
        while (runLock == null) {
            runLock = Cards.tryRunLock(folder)
            if (runLock != null || System.currentTimeMillis() > deadline) break
            Thread.sleep(100)
        }
        if (runLock != null) return super.getMainFunction()
        Log.w(TAG, "couldn't take ${folder.name}/${Cards.RUN_LOCK}, not starting")
        startRefused = "Couldn't start ${Sessions.forPath(this, sessionPath)?.name ?: folder.name}: Frond is still using it. Try again in a moment."
        return "frond_not_started"
    }

    // Takes a linked card if it is free and gives the core the card flags slot.json asks for, once
    // getMainFunction has the run lock. ARM machines only; the 68k core doesn't know the flags.
    // Runs on SDL's thread.
    private fun cardArguments(): List<String> {
        val folder = machineFolder() ?: return emptyList()
        if (launchDevice().runs68k || runLock == null) return emptyList()
        return try {
            val start = Cards.takeAtStart(Sessions.folder(this), folder)
            start.heldBy?.let { Log.i(TAG, "linked card is in $it") }
            start.arguments
        } catch (e: Exception) {
            Log.e(TAG, "card start: ${e.message}")
            emptyList()
        }
    }

    // After a save of the machine that succeeded: the slot is what the .frond says again.
    fun savedMachine() {
        if (runLock == null) return
        // Not while the file view has the card out: the slot must load swapped until it's back.
        machineFolder()?.takeIf { it.canonicalFile != SdCard.editing?.canonicalFile }?.let { Cards.clearSwap(it) }
    }

    // The core refuses to start with --fast-audio unless the NOR it runs is the qualified one for its
    // device. Runs on SDL's thread.
    private fun fastAudioQualified(): Boolean {
        val device = launchDevice()
        val nor = File(intent?.getStringExtra(EXTRA_NOR) ?: Firmware.nor(this, device).path)
        return Firmware.fastAudioQualified(device, sessionPath?.let { File(it) }, nor)
    }

    // Draws edge to edge in the colour around the device and keeps SDL's surface inside the system
    // bars and cutout, where the core centres the device.
    private fun fitInsideSystemBars() {
        val layout = SDLActivity.getContentView() ?: return
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.setBackgroundDrawable(ColorDrawable(BACKGROUND))
        layout.setBackgroundColor(BACKGROUND)
        @Suppress("DEPRECATION")
        if (Build.VERSION.SDK_INT < 35) {
            window.statusBarColor = Color.TRANSPARENT
            window.navigationBarColor = Color.TRANSPARENT
        }
        if (Build.VERSION.SDK_INT >= 29) window.isNavigationBarContrastEnforced = false
        WindowInsetsControllerCompat(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }
        // The room for the bar is known once the layout has its new size.
        layout.addOnLayoutChangeListener { _, l, t, r, b, oldL, oldT, oldR, oldB -> if (b - t != oldB - oldT || r - l != oldR - oldL) updateSurfaceMargin() }
        ViewCompat.setOnApplyWindowInsetsListener(layout) { view, insets ->
            // The soft keyboard shrinks the surface too, and the core fits the device above it.
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout() or WindowInsetsCompat.Type.ime())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            imeVisible = insets.isVisible(WindowInsetsCompat.Type.ime())
            updateSurfaceMargin()
            insets
        }
    }

    // Moves the Palm's surface down for a bar that takes its own space, and for the corner keyboard button
    // while the soft keyboard is up.
    private fun updateSurfaceMargin() {
        val density = resources.displayMetrics.density
        val portrait = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_PORTRAIT
        bar.portrait = portrait
        val barPx = (RunningBar.heightDp(portrait) * density).toInt()
        val parent = SDLActivity.getContentView()
        val available = if (parent == null) 0 else parent.height - parent.paddingTop - parent.paddingBottom
        val imeButtons = if (imeVisible && keyboardButton?.visibility == View.VISIBLE) (48 * density).toInt() else 0
        val margin = RunningBar.surfaceTopMargin(bar.mode, bar.shown, barPx, available, (RunningBar.MIN_PALM_DP * density).toInt(), imeButtons)
        (SDLActivity.mSurface?.layoutParams as? android.view.ViewGroup.MarginLayoutParams)?.let {
            if (it.topMargin != margin) {
                it.topMargin = margin
                SDLActivity.mSurface?.layoutParams = it
            }
        }
    }

    // The core draws at most 60 frames a second. Saying so lets a 90 or 120 Hz display drop to
    // 60 Hz, so frames show evenly instead of 11 and 22 ms apart.
    private fun requestFrameRate() {
        if (Build.VERSION.SDK_INT < 30) return
        val holder = SDLActivity.mSurface?.holder ?: return
        fun request(surface: Surface) {
            if (surface.isValid) surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        }
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = request(holder.surface)
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {}
        })
        request(holder.surface)
    }

    // SDL asks for an orientation from the window's shape; the setting decides instead.
    override fun setOrientationBis(w: Int, h: Int, resizable: Boolean, hint: String?) {
        requestedOrientation = settings.orientation.request
    }

    @Deprecated("Back before Android 13")
    override fun onBackPressed() = back()

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (event.keyCode == android.view.KeyEvent.KEYCODE_BACK) {
            if (event.action == android.view.KeyEvent.ACTION_UP) back()
            return true
        }
        // Controller keys go to the Palm here and never reach SDL, so they don't also arrive as
        // keyboard keys. Unused ones are swallowed too, or Android turns B into Back.
        if (ControllerMap.fromController(event)) {
            when (event.action) {
                android.view.KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0 && !leaving) controller.key(event.deviceId, event.keyCode, true)
                android.view.KeyEvent.ACTION_UP -> controller.key(event.deviceId, event.keyCode, false)
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        if (event.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK) && event.action == android.view.MotionEvent.ACTION_MOVE) {
            if (!leaving) {
                controller.axes(
                    event.deviceId,
                    event.getAxisValue(android.view.MotionEvent.AXIS_X),
                    event.getAxisValue(android.view.MotionEvent.AXIS_Y),
                    event.getAxisValue(android.view.MotionEvent.AXIS_HAT_X),
                    event.getAxisValue(android.view.MotionEvent.AXIS_HAT_Y),
                )
            }
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    // Resolve once off the UI thread. Power stays absent until the NOR is identified.
    private fun resolveRunningDevice() {
        val device = launchDevice()
        val session = sessionPath?.let { File(it) }
        val nor = File(intent?.getStringExtra(EXTRA_NOR) ?: Firmware.nor(this, device).path)
        Thread({
            val resolved = Firmware.runningDevice(device, session, nor)
            runOnUiThread {
                if (destroyed || leaving) return@runOnUiThread
                runningDevice = resolved
                configureController(resolved)
                addBar(resolved)
            }
        }, "FrondFirmware").apply { isDaemon = true }.start()
    }

    private fun configureController(device: Device) {
        controller.releaseAll()
        val buttons = PalmButtons(device, link, cacheDir)
        controller = ControllerInput(ControllerMap.load(this, device), buttons::press)
    }

    // Controller mapping for this device from Settings, and releases when a controller goes away.
    private fun addController() {
        configureController(runningDevice)
        inputDevices = getSystemService(InputManager::class.java)
        inputListener = object : InputManager.InputDeviceListener {
            override fun onInputDeviceAdded(deviceId: Int) = updateDanaEnvironment()
            override fun onInputDeviceChanged(deviceId: Int) = updateDanaEnvironment()
            override fun onInputDeviceRemoved(deviceId: Int) {
                controller.release(deviceId)
                updateDanaEnvironment()
            }
        }
        inputDevices?.registerInputDeviceListener(inputListener!!, null)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        // Android stops sending a held controller's events to a window without focus.
        if (!hasFocus) controller.releaseAll()
        super.onWindowFocusChanged(hasFocus)
    }

    override fun onPause() {
        foreground = false
        microphoneResume++
        pendingMicrophone = false
        if (launchDevice().microphone && link.connectedOnce) {
            Thread { link.request("microphone-mode", "off", timeoutSeconds = 2) }
                .apply { isDaemon = true }.start()
        }
        controller.releaseAll()
        // The menu and sheets live in this activity, so they pause with it: a waiting microphone request
        // is dropped and the Bluetooth read stops until the Connections sheet is in front again.
        menuModel?.cancelMicrophoneRequest()
        menuModel?.stopBluetoothRefresh()
        super.onPause()
    }

    // Saves the session and asks the core to quit. SDL then finishes the activity when its main
    // returns. Also runs when SDL asks the core to close (the core sends a "close" event).
    // Back closes the bar's menu first, then leaves the machine.
    private fun back() {
        if (bar.sheet != null) openSheet(null) else if (bar.menuOpen) bar.menuOpen = false else leave()
    }

    // Opens a sheet over the Palm, or closes it. The view covers the screen while one is open.
    private fun openSheet(sheet: RunningSheets.Sheet?) {
        if (leaving && sheet != null) return
        bar.menuOpen = false
        bar.sheet = sheet
        layoutBar()
        if (sheet != null) readMenuState()
    }

    // A Memory Stick's file view covers the Palm, so the core is told its panel is up.
    private fun filesOpen(open: Boolean) {
        if (runningDevice.memoryStick) link.setPanelVisible(open)
    }

    private fun leave() {
        if (leaving) return
        leaving = true
        link.cancelFiles()
        runOnUiThread {
            controller.releaseAll()
            hideKeyboard()
            keyboardButton?.visibility = View.GONE
            barView?.visibility = View.GONE
            savingNote?.visibility = View.VISIBLE
        }
        Thread({
            letDebuggerGo()
            // A card out for the file view goes back in first, so the final save has it.
            menuModel?.putCardBackForStop()
            val path = sessionPath
            val reply = synchronized(saveLock) {
                releaseInput()
                if (path != null) link.request("save", path) else null
            }
            if (reply != null && reply.ok) {
                savedMachine()
                finalSaveOk = true
            }
            when {
                reply == null || reply.ok -> quit()
                // The core stopped during the save; SDL finishes the activity and onDestroy leaves a note.
                reply.code == "closed" -> Log.e(TAG, "core stopped while saving")
                else -> {
                    Log.e(TAG, "save failed: ${reply.code} ${reply.message}")
                    runOnUiThread { askQuit(reply.message) }
                }
            }
        }, "FrondLeave").start()
    }

    // Lifts the pen and keys in the guest before a save. The core handles the focus event before it
    // answers the next request.
    private fun releaseInput() {
        SDLActivity.nativeFocusChanged(false)
        link.request("device-state", timeoutSeconds = 10)
    }

    override fun onStart() {
        stopped = false
        super.onStart()
        IrLink.setPaused(false)
        SerialLink.setPaused(false)
    }

    // Hidden: SDL pauses the core and its audio. Saving here means a process kill while away keeps
    // the session; the next start loads this save.
    override fun onStop() {
        super.onStop()
        stopped = true
        // The core pauses in the background; a connection's far side sees the Palm paused.
        IrLink.setPaused(true)
        SerialLink.setPaused(true)
        if (isFinishing || isChangingConfigurations) return
        saveInBackground()
    }

    // Saves the session on a thread, unless back is already saving it.
    fun saveInBackground() {
        val path = sessionPath ?: return
        if (leaving) return
        Thread({
            synchronized(saveLock) {
                if (leaving) return@Thread
                releaseInput()
                val reply = link.request("save", path, timeoutSeconds = 60)
                if (reply.ok) savedMachine()
                Log.i(TAG, "background save: ${reply.code} ${reply.message}")
            }
        }, "FrondStopSave").start()
    }

    // Dana toggles its native keyboard. Other devices show the IME only for an attached infrared keyboard,
    // with a keyboard button in the corner. Hardware keys reach the core through SDL either way.
    private fun addKeyboard() {
        val layout = SDLActivity.getContentView() as? RelativeLayout ?: return
        if (!launchDevice().isDana()) {
            val input = PalmKeyboard(this)
            layout.addView(input, RelativeLayout.LayoutParams(1, 1))
            keyboard = input
        }
        val button = android.widget.ImageButton(this).apply {
            setImageResource(R.drawable.ic_keyboard)
            contentDescription = "Keyboard"
            setBackgroundColor(Color.TRANSPARENT)
            alpha = 0.7f
            val pad = (8 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad, pad, pad)
            visibility = View.GONE
            setOnClickListener { toggleKeyboard() }
        }
        layout.addView(button, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.ALIGN_PARENT_TOP)
            addRule(RelativeLayout.ALIGN_PARENT_END)
        })
        keyboardButton = button
    }

    private fun imeShowing(): Boolean =
        ViewCompat.getRootWindowInsets(window.decorView)?.isVisible(WindowInsetsCompat.Type.ime()) == true

    private fun toggleKeyboard() {
        if (launchDevice().isDana()) {
            val view = danaState?.view ?: machineSettings.danaView
            val mode = danaState?.toggledKeyboard(lastDanaKeyboard)
                ?: if (view.keyboard == DanaKeyboard.HIDDEN) lastDanaKeyboard else DanaKeyboard.HIDDEN
            changeDanaView(view.copy(keyboard = mode)) { _, error ->
                if (error != null && !destroyed && !leaving) AlertDialog.Builder(this)
                    .setTitle("Couldn't change keyboard").setMessage(error).setPositiveButton("OK", null).show()
            }
            return
        }
        val input = keyboard ?: return
        if (imeShowing()) hideKeyboard() else if (keyboardAttached) input.show()
    }

    private fun hideKeyboard() {
        keyboard?.hide()
        SDLActivity.mSurface?.requestFocus()
    }

    // Applies the running machine's sound preference. Off mutes the output and keeps the
    // guest's audio timing; on after a start with --no-sound opens the audio device.
    private fun applySound() {
        val wanted = launchDevice().sound && (Sessions.forPath(this, sessionPath)?.settings?.sound ?: sound)
        if (wanted == sound) return
        sound = wanted
        link.whenConnected {
            if (!link.connectedOnce) return@whenConnected
            // Each send reads the setting when it goes out, so the last one carries the newest value
            // whatever order changes made before the link connected are queued in.
            soundSender.execute {
                val reply = link.request("sound-mode", if (sound) "on" else "off", timeoutSeconds = 10)
                if (!reply.ok) Log.w(TAG, "sound-mode: ${reply.code} ${reply.message}")
            }
        }
    }

    // Applies the running machine's Size, Smooth pixels and TG50 controls. The core changes the
    // window once the Palm shows again.
    fun applyDisplay() {
        val saved = Sessions.forPath(this, sessionPath)?.settings ?: return
        if (launchDevice().isDana() && saved.danaView != (danaState?.view ?: machineSettings.danaView)) {
            changeDanaView(saved.danaView) { _, error -> if (error != null) Log.w(TAG, "dana-view: $error") }
        }
        val wanted = saved.displayArgument(tg50Controls = launchDevice().id == "sony-clie-tg50", m68k = launchDevice().runs68k)
        if (wanted == display) return
        display = wanted
        link.whenConnected {
            if (!link.connectedOnce) return@whenConnected
            // As with sound, each send reads the newest value when it goes out.
            soundSender.execute {
                val sent = display
                val reply = link.request("display", sent, timeoutSeconds = 10)
                if (!reply.ok) {
                    Log.w(TAG, "display: ${reply.code} ${reply.message}")
                    // Try again on the next change or resume.
                    if (display == sent) display = ""
                }
            }
        }
    }

    // Sends a 68k machine's saved Speed to the core, which changes it at once.
    fun applySpeed() {
        if (!launchDevice().runs68k) return
        val wanted = (Sessions.forPath(this, sessionPath)?.settings ?: return).speed.arg
        Thread({
            val reply = link.request("speed", wanted, timeoutSeconds = 10)
            if (!reply.ok) Log.w(TAG, "speed: ${reply.code} ${reply.message}")
        }, "FrondSpeedSet").apply { isDaemon = true }.start()
    }

    private fun currentDanaEnvironment(): String {
        val manager = getSystemService(InputManager::class.java)
        val keyboards = manager?.inputDeviceIds?.asSequence()?.mapNotNull { id ->
            manager.getInputDevice(id)?.let {
                DanaKeyboardDevice(it.isVirtual, it.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC, Build.VERSION.SDK_INT < 27 || it.isEnabled)
            }
        }?.toList() ?: emptyList()
        return danaEnvironment(resources.displayMetrics.density, hasDanaHardwareKeyboard(keyboards))
    }

    // Host changes send only host fields, preserving the core's current zoom and pan.
    private fun updateDanaEnvironment() {
        if (!launchDevice().isDana() || destroyed || leaving || !link.connectedOnce) return
        val wanted = currentDanaEnvironment()
        if (wanted == danaEnvironment) return
        danaEnvironment = wanted
        soundSender.execute {
            if (destroyed || leaving) return@execute
            val reply = link.request("dana-view", wanted, timeoutSeconds = 10)
            if (!reply.ok) {
                if (danaEnvironment == wanted) danaEnvironment = ""
                Log.w(TAG, "dana-view: ${reply.message}")
            } else readDanaState()
        }
    }

    private fun readDanaState(): DanaViewState? {
        val reply = link.request("dana-view-state", timeoutSeconds = 10)
        return if (reply.ok) DanaViewState.parse(reply.message)?.also { acceptDanaState(it) } else null
    }

    // Native notifications update preferences directly, without echoing another setter.
    private fun acceptDanaState(state: DanaViewState) {
        danaState = state
        runOnUiThread {
            if (destroyed) return@runOnUiThread
            if (state.view.keyboard != DanaKeyboard.HIDDEN) lastDanaKeyboard = state.view.keyboard
            val previous = machineSettings.danaView
            machineSettings = machineSettings.copy(danaView = state.view)
            keyboardButton?.contentDescription = if (state.resolved == DanaKeyboard.HIDDEN) "Show keyboard" else "Hide keyboard"
            if (previous != state.view) {
                try {
                    Sessions.forPath(this, sessionPath)?.let { session ->
                        Sessions.updateSettings(this, session.id) { it.copy(danaView = state.view) }
                    }
                } catch (e: Exception) { Log.w(TAG, "couldn't save Dana view: ${e.message}") }
            }
        }
    }

    // A refused command leaves the saved preference and menu at the last accepted native state.
    fun changeDanaView(view: DanaView, done: (DanaView?, String?) -> Unit) {
        if (!launchDevice().isDana() || destroyed || leaving) {
            done(null, "The emulator isn't running.")
            return
        }
        soundSender.execute {
            val reply = link.request("dana-view", view.argument(), timeoutSeconds = 10)
            val actual = readDanaState()
            runOnUiThread {
                if (!destroyed) done(actual?.view, if (!reply.ok) reply.message else if (actual == null) "Couldn't read the Dana view." else null)
            }
        }
    }

    // Asks the core whether the infrared keyboard is attached and shows the keyboard button to match. Waits for
    // the control link first, which with the debugger on can be a long time.
    private fun readKeyboard() {
        if (launchDevice().isDana()) {
            keyboardButton?.visibility = if (leaving) View.GONE else View.VISIBLE
            return
        }
        if (!link.connectedOnce) {
            link.whenConnected { if (!leaving && link.connectedOnce) runOnUiThread { readKeyboard() } }
            return
        }
        Thread({
            val reply = link.request("keyboard-state", timeoutSeconds = 60)
            if (!reply.ok) return@Thread
            val attached = reply.message == "attached"
            keyboardAttached = attached
            runOnUiThread {
                bar.keyboardKey = attached
                syncCorner()
                if (!attached && keyboard?.hasFocus() == true) hideKeyboard()
            }
        }, "FrondKeyboard").apply { isDaemon = true }.start()
    }

    override fun onResume() {
        super.onResume()
        foreground = true
        startPendingMicrophone()
        if (!leaving) readKeyboard()
        if (!leaving) applySound()
        bar.sound = sound
        if (!leaving) applyDisplay()
        menuModel?.readMicrophone()
        if (bar.sheet == RunningSheets.Sheet.CONNECTIONS) menuModel?.startBluetoothRefresh()
        if (!leaving) updateDanaEnvironment()
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        requestedOrientation = settings.orientation.request
        layoutBar()
        updateSurfaceMargin()
        ViewCompat.requestApplyInsets(SDLActivity.getContentView() ?: window.decorView)
        updateDanaEnvironment()
    }

    // The ComposeView over the Palm: the whole width while the bar shows, just the handle when it's hidden,
    // so touches outside both reach the Palm.
    private fun addBar(device: Device) {
        val layout = SDLActivity.getContentView() as? RelativeLayout ?: return
        if (barView != null) return
        val session = Sessions.forPath(this, sessionPath)
        bar.shown = machineSettings.barShown
        bar.mode = machineSettings.barMode
        bar.name = session?.name ?: device.label
        bar.subtitle = RunningBar.subtitle(device, machineSettings)
        bar.soundKey = RunningBar.soundKey(device)
        bar.sound = sound
        bar.powerKey = RunningBar.powerKey(device)
        val owner = BarOwner().also { it.create() }
        barOwner = owner
        val results = BarResults(this)
        barResults = results
        val model = RunningModel(
            ModelHost(this, owner.lifecycleScope, { runOnUiThread(it) }, {}),
            device, SdCard(this, link, sessionPath, device), PalmDatabases(this, link), link, session?.name ?: device.label,
        )
        menuModel = model
        bar.device = device
        bar.model = model
        model.watchGdb()
        model.watchBeam()
        model.watchBluetooth()
        model.watchLinks()
        readMenuState()
        val view = androidx.compose.ui.platform.ComposeView(this).apply {
            setViewTreeLifecycleOwner(owner)
            setViewTreeViewModelStoreOwner(owner)
            setViewTreeSavedStateRegistryOwner(owner)
            setContent {
                androidx.compose.runtime.CompositionLocalProvider(androidx.activity.compose.LocalActivityResultRegistryOwner provides results) {
                    com.rm.frond.ui.RunningBarView(bar, com.rm.frond.ui.RunningBarActions(
                        back = { leave() }, toggleSound = { toggleSound() }, power = { pressPower() },
                        keyboard = { toggleKeyboard() }, show = { setBarShown(true) }, hide = { setBarShown(false) }, autoHide = { setBarShown(false, remember = false) },
                        openMenu = { openMenu() }, menuRow = { menuRow(it) },
                        openSheet = { openSheet(it) }, closeSheet = { openSheet(null) }, filesOpen = { filesOpen(it) },
                    ))
                }
            }
        }
        barView = view
        // Compose makes its window recomposer from the content view's owners, so SDL's layout needs them too.
        layout.setViewTreeLifecycleOwner(owner)
        layout.setViewTreeViewModelStoreOwner(owner)
        layout.setViewTreeSavedStateRegistryOwner(owner)
        layout.addView(view, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.ALIGN_PARENT_TOP)
        })
        layoutBar()
    }

    private fun readMenuState() {
        val model = menuModel ?: return
        model.readKeyboard()
        model.readHold()
        model.readNetwork()
        model.readMicrophone()
        model.readCard()
    }

    private fun openMenu() {
        if (leaving) return
        readMenuState()
        bar.menuOpen = !bar.menuOpen
    }

    // The menu's rows. Reset, Hard reset and Network ask in the bar's own dialogs.
    private fun menuRow(id: RunningMenu.Id) {
        val model = menuModel ?: return
        if (leaving) return
        when (id) {
            RunningMenu.Id.HOLD -> model.changeHold(model.hold != true)
            RunningMenu.Id.KEYBOARD -> model.attachKeyboard(model.keyboard != true)
            RunningMenu.Id.MICROPHONE -> toggleMicrophone(model)
            RunningMenu.Id.DISPLAY, RunningMenu.Id.CONNECTIONS -> openSheet(RunningSheets.menuSheet(id))
            RunningMenu.Id.SCREENSHOT -> takeScreenshot()
            RunningMenu.Id.HIDE_BAR -> setBarShown(false)
            RunningMenu.Id.SAVE_COPY -> saveCopy(model)
            RunningMenu.Id.LOG -> model.showLog()
            RunningMenu.Id.RESET, RunningMenu.Id.HARD_RESET, RunningMenu.Id.NETWORK -> Unit
        }
    }

    private fun toggleMicrophone(model: RunningModel) {
        val on = model.microphone != "on" && model.microphone != "pending"
        if (!on) model.changeMicrophone(false)
        else if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == android.content.pm.PackageManager.PERMISSION_GRANTED) model.changeMicrophone(true)
        else requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), REQUEST_MICROPHONE)
    }

    // Android 17 asks before Frond reaches the local network; Connections need it.
    fun askLocalNetwork() {
        runOnUiThread {
            if (Links.needsPermission(this)) requestPermissions(arrayOf(Links.LOCAL_NETWORK), REQUEST_LOCAL_NETWORK)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (barResults?.dispatchPermissions(requestCode, permissions, grantResults) == true) return
        if (requestCode != REQUEST_MICROPHONE) return
        if (grantResults.firstOrNull() == android.content.pm.PackageManager.PERMISSION_GRANTED) menuModel?.changeMicrophone(true)
        else menuModel?.microphoneDenied()
    }

    private fun saveCopy(model: RunningModel) {
        @Suppress("DEPRECATION")
        startActivityForResult(
            android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(android.content.Intent.CATEGORY_OPENABLE)
                .setType("application/zip")
                .putExtra(android.content.Intent.EXTRA_TITLE, "${model.name}.zip"),
            REQUEST_SAVE_COPY,
        )
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: android.content.Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (barResults?.dispatchResult(requestCode, resultCode, data) == true) return
        if (requestCode == REQUEST_SAVE_COPY && resultCode == RESULT_OK) data?.data?.let { menuModel?.saveCopy(it) }
    }

    // The Palm's view as a PNG in Pictures/Frond.
    private fun takeScreenshot() {
        val surface = SDLActivity.mSurface ?: return
        if (surface.width == 0 || surface.height == 0) return
        val bitmap = android.graphics.Bitmap.createBitmap(surface.width, surface.height, android.graphics.Bitmap.Config.ARGB_8888)
        android.view.PixelCopy.request(surface, bitmap, { result ->
            if (result != android.view.PixelCopy.SUCCESS) {
                android.widget.Toast.makeText(this, "Couldn't take the screenshot", android.widget.Toast.LENGTH_SHORT).show()
                return@request
            }
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date())
            val name = RunningMenu.screenshotName(bar.name, stamp)
            Thread({
                val saved = try { savePicture(bitmap, name); true } catch (e: Exception) { Log.w(TAG, "screenshot: ${e.message}"); false }
                runOnUiThread {
                    android.widget.Toast.makeText(this, if (saved) "Screenshot saved" else "Couldn't save the screenshot", android.widget.Toast.LENGTH_SHORT).show()
                }
            }, "FrondScreenshot").apply { isDaemon = true }.start()
        }, android.os.Handler(mainLooper))
    }

    private fun savePicture(bitmap: android.graphics.Bitmap, name: String) {
        if (Build.VERSION.SDK_INT >= 29) {
            val values = android.content.ContentValues().apply {
                put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, name)
                put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
                put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Frond")
            }
            val uri = contentResolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: throw java.io.IOException("no place to save")
            contentResolver.openOutputStream(uri)!!.use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        } else {
            val folder = getExternalFilesDir(android.os.Environment.DIRECTORY_PICTURES) ?: throw java.io.IOException("no place to save")
            File(folder, name).outputStream().use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        }
    }

    private fun layoutBar() {
        val view = barView ?: return
        val params = view.layoutParams as RelativeLayout.LayoutParams
        val full = bar.shown || bar.sheet != null
        val width = if (full) RelativeLayout.LayoutParams.MATCH_PARENT else RelativeLayout.LayoutParams.WRAP_CONTENT
        val height = if (bar.sheet != null) RelativeLayout.LayoutParams.MATCH_PARENT else RelativeLayout.LayoutParams.WRAP_CONTENT
        val portrait = resources.configuration.orientation != android.content.res.Configuration.ORIENTATION_LANDSCAPE
        val centre = if (!full && RunningBar.handleCentred(portrait)) RelativeLayout.TRUE else 0
        val start = if (!full && !RunningBar.handleCentred(portrait)) RelativeLayout.TRUE else 0
        if (params.width != width || params.height != height || params.getRule(RelativeLayout.CENTER_HORIZONTAL) != centre ||
            params.getRule(RelativeLayout.ALIGN_PARENT_START) != start) {
            params.width = width
            params.height = height
            params.addRule(RelativeLayout.CENTER_HORIZONTAL, centre)
            params.addRule(RelativeLayout.ALIGN_PARENT_START, start)
            view.layoutParams = params
        }
        syncCorner()
        updateSurfaceMargin()
    }

    // The corner keyboard button shows only while the bar is hidden.
    private fun syncCorner() {
        val corner = RunningBar.cornerButtons(bar.shown) && !leaving
        keyboardButton?.visibility = if (corner && bar.keyboardKey) View.VISIBLE else View.GONE
    }

    // The bar's mode changed in the Display sheet.
    fun applyBarMode() {
        val mode = Sessions.forPath(this, sessionPath)?.settings?.barMode ?: return
        runOnUiThread {
            if (bar.mode == mode) return@runOnUiThread
            bar.mode = mode
            machineSettings = machineSettings.copy(barMode = mode)
            layoutBar()
        }
    }

    // remember is false when Auto-hide hides the bar, so the machine still starts with it shown.
    private fun setBarShown(shown: Boolean, remember: Boolean = true) {
        if (bar.shown == shown) return
        bar.shown = shown
        layoutBar()
        if (!remember) return
        machineSettings = machineSettings.copy(barShown = shown)
        val id = Sessions.forPath(this, sessionPath)?.id ?: return
        Thread({
            try {
                Sessions.updateSettings(applicationContext, id) { it.copy(barShown = shown) }
            } catch (e: Exception) {
                Log.w(TAG, "bar setting: ${e.message}")
            }
        }, "FrondBarSetting").apply { isDaemon = true }.start()
    }

    private fun toggleSound() {
        if (leaving) return
        val on = !bar.sound
        bar.sound = on
        val id = Sessions.forPath(this, sessionPath)?.id
        Thread({
            try {
                if (id != null) Sessions.updateSettings(applicationContext, id) { it.copy(sound = on) }
            } catch (e: Exception) {
                Log.w(TAG, "sound setting: ${e.message}")
            }
            runOnUiThread { if (!leaving) applySound() }
        }, "FrondBarSound").apply { isDaemon = true }.start()
    }

    // Presses and releases the Palm's power key by key replay on the control link.
    private fun pressPower() {
        if (leaving) return
        Thread({
            // A file of its own per press, so two quick presses never share one.
            val file = File.createTempFile("power", ".replay", cacheDir)
            try {
                file.writeText("0 key-down power\n80 key-up power\n")
                // The core drops the microphone for input-replay; arm it again if it was on.
                val wasOn = link.request("microphone-state", timeoutSeconds = 10).message == "on"
                val reply = link.request("input-replay", file.path, timeoutSeconds = 30)
                if (!reply.ok) Log.w(TAG, "power: ${reply.code} ${reply.message}")
                if (wasOn) link.request("microphone-mode", "on", timeoutSeconds = 10)
            } finally {
                file.delete()
            }
        }, "FrondBarPower").apply { isDaemon = true }.start()
    }

    // With the debugger on in Settings: a loopback port for the core's --gdb, gdb's port on the network through
    // DebugLink, and a note over the emulator while the core waits for gdb before it runs the Palm.
    private fun startDebugger() {
        if (!settings.debugger) return
        val reserved = DebugLink.reservePort()
        if (reserved == null) {
            Log.e(TAG, "debugger: no free loopback port")
            return
        }
        debugReservation = reserved
        val port = reserved.localPort
        debugPort = port
        val layout = SDLActivity.getContentView() as? RelativeLayout
        val note = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(48, 24, 48, 24)
            setBackgroundColor(Color.argb(0xCC, 0x0B, 0x0C, 0x11))
            visibility = View.GONE
        }
        layout?.addView(note, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.CENTER_IN_PARENT)
        })
        Thread({
            val error = synchronized(debugLock) {
                // Back right at launch already stopped it.
                if (debugStopped) return@Thread
                DebugLink.relay(BeamMode.Listen(settings.debuggerPort), port)
            }
            val addresses = NetRelay.addresses(applicationContext)
            val text = when {
                error != null -> error
                addresses.isEmpty() -> "Waiting for gdb on port ${settings.debuggerPort}"
                else -> "Waiting for gdb at " + addresses.joinToString(", ") { "$it:${settings.debuggerPort}" }
            }
            runOnUiThread { if (!(gdbBlocksLink && link.connectedOnce) && !leaving) { note.text = text; note.visibility = View.VISIBLE } }
            // Nothing can reach the core's gdb port, so let the Palm run without it.
            if (error != null) releaseDebugger(port)
        }, "FrondDebugger").apply { isDaemon = true }.start()
        if (gdbBlocksLink) {
            link.whenConnected { runOnUiThread { layout?.removeView(note) } }
        } else {
            // This core runs the Palm and answers the control link before gdb attaches, so the note stays until
            // gdb-state says gdb is connected.
            Thread({
                while (!leaving && !destroyed) {
                    val reply = link.request("gdb-state", timeoutSeconds = 5)
                    if (reply.ok && reply.message.startsWith("connected")) break
                    Thread.sleep(1000)
                }
                runOnUiThread { layout?.removeView(note) }
            }, "FrondGdbNote").apply { isDaemon = true }.start()
        }
    }

    // Ends any gdb session, which lets a Palm stopped in gdb run, and lets a core still waiting for its first gdb
    // go on, so the control link can save. Not on the main thread.
    private fun letDebuggerGo() {
        val port = debugPort ?: return
        stopDebugLink()
        if (!link.connectedOnce) releaseDebugger(port)
    }

    private fun stopDebugLink() {
        synchronized(debugLock) {
            debugStopped = true
            DebugLink.stop()
        }
    }

    // Until the core's control link connects or the activity is gone. The core may not be listening yet just
    // after launch; after a release it connects the control link, and is only released again if it doesn't.
    private fun releaseDebugger(port: Int) {
        while (!link.connectedOnce && !destroyed) {
            if (DebugLink.release(port)) {
                val until = System.nanoTime() + 10_000_000_000L
                while (!link.connectedOnce && !destroyed && System.nanoTime() < until) Thread.sleep(100)
            } else {
                Thread.sleep(300)
            }
        }
    }

    // The core is still waiting for its first gdb and hasn't run the Palm.
    val waitingForGdb: Boolean get() = gdbBlocksLink && debugPort != null && !link.connectedOnce

    // A small note over the emulator while back saves the session.
    private fun addSavingNote() {
        val layout = SDLActivity.getContentView() as? RelativeLayout ?: return
        val note = TextView(this).apply {
            text = "Saving"
            setTextColor(Color.WHITE)
            textSize = 18f
            gravity = Gravity.CENTER
            setPadding(48, 24, 48, 24)
            setBackgroundColor(Color.argb(0xCC, 0x0B, 0x0C, 0x11))
            visibility = View.GONE
        }
        layout.addView(note, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.CENTER_IN_PARENT)
        })
        savingNote = note
    }

    // Debug builds log the core's speed every 5 s while shown.
    private fun logSpeed() {
        Thread({
            while (!leaving) {
                Thread.sleep(5000)
                if (stopped || leaving) continue
                val reply = link.request("speed-state", timeoutSeconds = 5)
                if (reply.code == "closed") break
                if (reply.ok) Log.i(TAG, "speed: ${reply.message}")
            }
        }, "FrondSpeed").apply { isDaemon = true }.start()
    }

    // Debug builds read "driver period" from files/audio-test, for comparing audio setups.
    private val audioTest: List<String> by lazy {
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE == 0) emptyList()
        else try { File(filesDir, "audio-test").readText().trim().split(' ') } catch (e: Exception) { emptyList() }
    }

    // SDL's audio backend: aaudio, openslES or android.
    private fun chooseAudioDriver() {
        val driver = audioTest.getOrNull(0)?.takeIf { it.isNotEmpty() } ?: defaultAudioDriver()
        Os.setenv("SDL_AUDIODRIVER", driver, true)
        Log.i(TAG, "audio driver $driver")
    }

    private fun defaultAudioDriver() = if (Build.VERSION.SDK_INT >= 27) "aaudio" else "openslES"

    // Host audio period in frames, normal and low-latency audio alike: a power of two (SDL rounds
    // to one) of at least 1024 and at least the phone's output buffer in 44.1 kHz frames. SDL's
    // AAudio and OpenSL output is filled in bursts of the mixer's period, so 512, the desktop's,
    // underran 8-13 times a second on the x86_64 AVD against 3-4 at 1024 (doc/step9-plan.md, M4
    // notes), and low latency at 512 fell back on a phone within 0.2 s. The core sizes its
    // low-latency limits from this period.
    private fun audioPeriod(): Int {
        audioTest.getOrNull(1)?.toIntOrNull()?.takeIf { it in 64..8192 }?.let { return it }
        val audio = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val burst = audio.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)?.toIntOrNull() ?: 0
        val rate = audio.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 0
        val frames = if (burst > 0 && rate > 0) (burst.toLong() * 44100 / rate).toInt() else 0
        var period = 1024
        while (period < frames && period < 4096) period *= 2
        Log.i(TAG, "audio output $rate Hz, $burst frames per buffer; period $period")
        return period
    }

    private fun quit() {
        quitSent = true
        link.request("quit", timeoutSeconds = 10)
    }

    private fun askQuit(message: String) {
        AlertDialog.Builder(this)
            .setTitle("Couldn't save the session")
            .setMessage(message)
            .setPositiveButton("Keep running") { _, _ ->
                leaving = false
                savingNote?.visibility = View.GONE
            }
            .setNegativeButton("Quit without saving") { _, _ -> Thread(::quit, "FrondQuit").start() }
            .setCancelable(false)
            .show()
    }

    override fun onDestroy() {
        // Android recreates the activity for a change the manifest doesn't take. SDL would quit the
        // core, and a second core can't start in this process, so save the session and end the
        // process instead; the next start loads the save.
        destroyed = true
        menuModel?.printer?.close()
        barOwner?.destroy()
        inputListener?.let { inputDevices?.unregisterInputDeviceListener(it) }
        inputListener = null
        if (!isFinishing && !leaving) {
            if (waitingForGdb) {
                // Nothing has run since the session loaded, so there's nothing to save.
                Log.w(TAG, "recreated by Android while waiting for gdb")
                Process.killProcess(Process.myPid())
            }
            // Ends a gdb session holding the Palm stopped, so the save below gets an answer.
            if (debugPort != null) Thread(::stopDebugLink, "FrondDebugger").apply { start(); join(2_000) }
            sessionPath?.let { path ->
                val reply = synchronized(saveLock) {
                    releaseInput()
                    link.request("save", path, timeoutSeconds = 20)
                }
                if (reply.ok) savedMachine()
                Log.w(TAG, "recreated by Android, saved: ${reply.code} ${reply.message}")
            }
            Process.killProcess(Process.myPid())
        }
        super.onDestroy()
        // A core that stopped by itself, before anyone asked it to quit, failed to start or crashed.
        if (isFinishing && !quitSent) {
            File(filesDir, START_ERROR).writeText(
                startRefused ?: if (link.connectedOnce) "The emulator stopped unexpectedly." else "The emulator couldn't start this session.",
            )
        }
        if (running === this) running = null
        // Nothing has the card out for the file view once the machine has stopped.
        if (isFinishing) SdCard.editing = null
        // The core has quit after a good final save: a linked card goes back to Cards.
        if (isFinishing && quitSent && finalSaveOk && runLock != null && !launchDevice().runs68k) {
            machineFolder()?.let { folder ->
                try {
                    Cards.releaseAtClose(Sessions.folder(this), folder)?.let { Log.w(TAG, "card release: $it") }
                } catch (e: Exception) {
                    Log.e(TAG, "card release: ${e.message}")
                }
            }
        }
        HostDns.stop(applicationContext)
        IrLink.stop()
        SerialLink.stop()
        UsbLink.stop()
        stopDebugLink()
        try { debugReservation?.close() } catch (_: Exception) {}
        BluetoothLink.stop()
        link.close()
        // Native statics in the core don't reset between runs, so the process ends with the activity.
        if (isFinishing) Process.killProcess(Process.myPid())
    }

    // The control link, for the menu's model in this process.
    val control: ControlLink get() = link
    val session: String? get() = sessionPath

    companion object {
        // The emulator in this process, if it's up.
        @Volatile var running: FrondActivity? = null
        // The running machine's .run.lock, held until the process ends.
        @Volatile private var runLock: Cards.RunLock? = null
        const val TAG = "Frond"
        const val EXTRA_SESSION = "session"
        private const val REQUEST_MICROPHONE = 41
        private const val REQUEST_LOCAL_NETWORK = 43
        private const val REQUEST_SAVE_COPY = 42
        const val EXTRA_DEVICE = "device"
        const val EXTRA_NOR = "nor"
        const val EXTRA_NAND = "nand"
        // Written when the core stops without being asked; HomeActivity shows it once.
        const val START_ERROR = "start-error.txt"
        // Around the device and behind the system bars, the dark end of the TX button bar art.
        const val BACKGROUND_HEX = "0B0C11"
        val BACKGROUND = Color.rgb(0x0B, 0x0C, 0x11)
    }
}
