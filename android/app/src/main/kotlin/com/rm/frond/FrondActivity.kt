package com.rm.frond

import android.app.AlertDialog
import android.content.Context
import android.content.pm.ApplicationInfo
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
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import java.io.File

// Runs the native core through SDL in its own process (":emulator"), so every start gets
// fresh native state. libmain.so holds SDL_main. HomeActivity starts it with the extras
// below; back saves the session through the core's control socket and returns home. Going to
// the background pauses the core (patch 0088) and saves the session, so a process kill keeps it.
class FrondActivity : SDLActivity() {
    private lateinit var link: ControlLink
    private var sessionPath: String? = null
    @Volatile private var leaving = false
    @Volatile private var quitSent = false
    private lateinit var settings: Settings
    private var savingNote: TextView? = null
    private var menuButton: View? = null
    private var keyboardButton: View? = null
    private var keyboard: PalmKeyboard? = null
    // The Palm infrared keyboard, as the core last said.
    @Volatile private var keyboardAttached = false
    @Volatile private var stopped = false
    // Set while our own Peripherals screen covers the emulator; it saves on its own stop instead.
    @Volatile private var panelOpen = false
    // One save at a time: the background save and the save on back.
    private val saveLock = Any()
    // Game controller buttons and sticks to the Palm's buttons. UI thread only.
    private lateinit var controller: ControllerInput

    override fun getLibraries(): Array<String> = arrayOf("c++_shared", "SDL2", "SDL2_image", "main")

    override fun onCreate(savedInstanceState: Bundle?) {
        sessionPath = intent?.getStringExtra(EXTRA_SESSION)
        settings = Settings.from(this, intent)
        requestedOrientation = settings.orientation.request
        File(filesDir, START_ERROR).delete()
        link = ControlLink(File(filesDir, "control.sock")) { name, _ -> if (name == "close") leave() }
        chooseAudioDriver()
        // SDL keeps its thread running while paused; the core idles itself in the background and
        // still answers the control socket, so it can save. SDL pauses the audio device.
        Os.setenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0", true)
        Os.setenv("SDL_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO", "1", true)
        HostDns.start(applicationContext)
        // A btvirt and sockets left by an emulator that was killed.
        Thread { BtLink.clean(applicationContext) }.start()
        super.onCreate(savedInstanceState)
        fitInsideSystemBars()
        requestFrameRate()
        addSavingNote()
        addMenuButton()
        addKeyboard()
        addController()
        SdCard.clean(this)
        // An edited card kept from a kill goes back into the Palm once the core answers.
        Thread { SdCard.restorePending(applicationContext, link, sessionPath) }.apply { isDaemon = true }.start()
        running = this
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) logSpeed()
        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { leave() }
        }
    }

    // The core's command line. With a saved session file the core loads it; otherwise it starts
    // the device from the NOR and NAND extras (files/firmware/palm-tx/ when they're missing).
    override fun getArguments(): Array<String> {
        val extras = intent?.extras
        val arguments = mutableListOf<String>()
        val session = sessionPath
        if (session != null && File(session).isFile) {
            arguments += session
        } else {
            val device = Device.byId(extras?.getString(EXTRA_DEVICE)) ?: Device.TX
            val nor = extras?.getString(EXTRA_NOR) ?: Firmware.nor(this, device).path
            val nand = extras?.getString(EXTRA_NAND) ?: Firmware.nand(this, device).path
            arguments += device.coreFlags
            arguments += nor
            if (File(nand).isFile) arguments += listOf("--nand", nand)
        }
        arguments += listOf("--window-fit", settings.size.arg, "--window-background", BACKGROUND_HEX)
        if (settings.smoothing) arguments += "--smooth-lcd"
        if (!settings.sound) arguments += "--no-sound"
        if (settings.fastAudio && fastAudioQualified()) {
            arguments += "--fast-audio"
        } else {
            arguments += listOf("--audio-period", audioPeriod().toString())
        }
        arguments += listOf("--desktop-socket", link.path.path)
        return arguments.toTypedArray()
    }

    // The core refuses to start with --fast-audio unless the NOR it runs is the qualified one for its
    // device: the one inside a saved session, else the installed firmware. Runs on SDL's thread.
    private fun fastAudioQualified(): Boolean {
        val session = sessionPath?.let { File(it) }?.takeIf { it.isFile }
        val device = Device.byId(intent?.getStringExtra(EXTRA_DEVICE)) ?: Device.TX
        val md5 = if (session != null) Sessions.norMd5(session) else Firmware.md5(File(intent?.getStringExtra(EXTRA_NOR) ?: Firmware.nor(this, device).path))
        return md5 != null && Device.entries.any { it.fastAudioNorMd5 == md5 }
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
        ViewCompat.setOnApplyWindowInsetsListener(layout) { view, insets ->
            // The soft keyboard shrinks the surface too, and the core fits the device above it.
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout() or WindowInsetsCompat.Type.ime())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            // With the soft keyboard up the device would cover the corner buttons, so it goes below them.
            val buttons = if (insets.isVisible(WindowInsetsCompat.Type.ime())) menuButton?.height ?: 0 else 0
            (SDLActivity.mSurface?.layoutParams as? android.view.ViewGroup.MarginLayoutParams)?.let {
                if (it.topMargin != buttons) {
                    it.topMargin = buttons
                    SDLActivity.mSurface?.layoutParams = it
                }
            }
            insets
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
    override fun onBackPressed() = leave()

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (event.keyCode == android.view.KeyEvent.KEYCODE_BACK) {
            if (event.action == android.view.KeyEvent.ACTION_UP) leave()
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

    // Controller mapping for this device from Settings, and releases when a controller goes away.
    private fun addController() {
        val device = Device.byId(intent?.getStringExtra(EXTRA_DEVICE)) ?: Device.TX
        val buttons = PalmButtons(device, link, cacheDir)
        controller = ControllerInput(ControllerMap.load(this, device), buttons::press)
        getSystemService(android.hardware.input.InputManager::class.java)?.registerInputDeviceListener(
            object : android.hardware.input.InputManager.InputDeviceListener {
                override fun onInputDeviceAdded(deviceId: Int) {}
                override fun onInputDeviceChanged(deviceId: Int) {}
                override fun onInputDeviceRemoved(deviceId: Int) = controller.release(deviceId)
            },
            null,
        )
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        // Android stops sending a held controller's events to a window without focus.
        if (!hasFocus) controller.releaseAll()
        super.onWindowFocusChanged(hasFocus)
    }

    override fun onPause() {
        controller.releaseAll()
        super.onPause()
    }

    // Saves the session and asks the core to quit. SDL then finishes the activity when its main
    // returns. Also runs when SDL asks the core to close (the core sends a "close" event).
    private fun leave() {
        if (leaving) return
        leaving = true
        runOnUiThread {
            controller.releaseAll()
            hideKeyboard()
            keyboardButton?.visibility = View.GONE
            savingNote?.visibility = View.VISIBLE
        }
        Thread({
            val path = sessionPath
            val reply = synchronized(saveLock) {
                releaseInput()
                if (path != null) link.request("save", path) else null
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
        panelOpen = false
        super.onStart()
    }

    // Hidden: SDL pauses the core and its audio. Saving here means a process kill while away keeps
    // the session; the next start loads this save.
    override fun onStop() {
        super.onStop()
        stopped = true
        if (isFinishing || isChangingConfigurations || panelOpen) return
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
                Log.i(TAG, "background save: ${reply.code} ${reply.message}")
            }
        }, "FrondStopSave").start()
    }

    // A small button in the corner that opens the menu screen (applications, SD card and the rest).
    private fun addMenuButton() {
        val layout = SDLActivity.getContentView() as? RelativeLayout ?: return
        val button = android.widget.ImageButton(this).apply {
            setImageResource(R.drawable.ic_menu)
            contentDescription = "Menu"
            setBackgroundColor(Color.TRANSPARENT)
            alpha = 0.7f
            val pad = (8 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad, pad, pad)
            setOnClickListener { openPanel() }
            id = View.generateViewId()
        }
        menuButton = button
        layout.addView(button, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.ALIGN_PARENT_TOP)
            addRule(RelativeLayout.ALIGN_PARENT_END)
        })
    }

    // While the Palm infrared keyboard is attached, a keyboard button beside the menu button shows
    // and hides the soft keyboard. Hardware keys reach the core through SDL either way.
    private fun addKeyboard() {
        val layout = SDLActivity.getContentView() as? RelativeLayout ?: return
        val input = PalmKeyboard(this)
        layout.addView(input, RelativeLayout.LayoutParams(1, 1))
        keyboard = input
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
            menuButton?.let { addRule(RelativeLayout.START_OF, it.id) } ?: addRule(RelativeLayout.ALIGN_PARENT_END)
        })
        keyboardButton = button
    }

    private fun imeShowing(): Boolean =
        ViewCompat.getRootWindowInsets(window.decorView)?.isVisible(WindowInsetsCompat.Type.ime()) == true

    private fun toggleKeyboard() {
        val input = keyboard ?: return
        if (imeShowing()) hideKeyboard() else if (keyboardAttached) input.show()
    }

    private fun hideKeyboard() {
        keyboard?.hide()
        SDLActivity.mSurface?.requestFocus()
    }

    // Asks the core whether the infrared keyboard is attached and shows the keyboard button to match.
    private fun readKeyboard() {
        Thread({
            val reply = link.request("keyboard-state", timeoutSeconds = 60)
            if (!reply.ok) return@Thread
            val attached = reply.message == "attached"
            keyboardAttached = attached
            runOnUiThread {
                keyboardButton?.visibility = if (attached && !leaving) View.VISIBLE else View.GONE
                if (!attached && keyboard?.hasFocus() == true) hideKeyboard()
            }
        }, "FrondKeyboard").apply { isDaemon = true }.start()
    }

    override fun onResume() {
        super.onResume()
        if (!leaving) readKeyboard()
    }

    private fun openPanel() {
        if (leaving) return
        hideKeyboard()
        panelOpen = true
        startActivity(
            android.content.Intent(this, PeripheralsActivity::class.java)
                .putExtra(EXTRA_DEVICE, intent?.getStringExtra(EXTRA_DEVICE) ?: Device.TX.id),
        )
    }

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

    // Host audio period in frames for normal audio: a power of two (SDL rounds to one) of at least
    // 1024 and at least the phone's output buffer in 44.1 kHz frames. 512, the desktop's, underran
    // 8-13 times a second on the x86_64 AVD against 3-4 at 1024 (doc/step9-plan.md, M4 notes).
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
        if (!isFinishing && !leaving) {
            sessionPath?.let { path ->
                val reply = synchronized(saveLock) {
                    releaseInput()
                    link.request("save", path, timeoutSeconds = 20)
                }
                Log.w(TAG, "recreated by Android, saved: ${reply.code} ${reply.message}")
            }
            Process.killProcess(Process.myPid())
        }
        super.onDestroy()
        // A core that stopped by itself, before anyone asked it to quit, failed to start or crashed.
        if (isFinishing && !quitSent) {
            File(filesDir, START_ERROR).writeText(
                if (link.connectedOnce) "The emulator stopped unexpectedly." else "The emulator couldn't start this session.",
            )
        }
        if (running === this) running = null
        HostDns.stop(applicationContext)
        IrBeam.stop()
        BtLink.stop(applicationContext)
        link.close()
        // Native statics in the core don't reset between runs, so the process ends with the activity.
        if (isFinishing) Process.killProcess(Process.myPid())
    }

    // The control link, for the Peripherals screen in this process.
    val control: ControlLink get() = link
    val session: String? get() = sessionPath

    companion object {
        // The emulator in this process, if it's up.
        @Volatile var running: FrondActivity? = null
        const val TAG = "Frond"
        const val EXTRA_SESSION = "session"
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
