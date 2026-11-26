package com.rm.frond

import android.content.Context
import android.system.Os
import android.system.OsConstants
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException

// This Frond's Bluetooth radio: BlueZ btvirt (packaged as libbtvirt.so) run as a child process. Each
// connection to its bt-server-bredr socket is a new virtual controller, and controllers in one btvirt can
// find and talk to each other. The link engine runs it while a controller wants it (LinkEngine), for the
// running machine on This Frond or over a connection whose radio is here, and for a peer's machines.
object BtLink {
    private const val TAG = "Frond"
    private const val PROGRAM = "libbtvirt.so"
    private const val PHONE = "libfrond-bluetooth-phone.so"
    private const val SERVER = "bt-server-bredr"

    private val lock = Any()
    private var program: Process? = null
    data class PhoneStatus(val call: String = "", val sync: String = "", val socket: String? = null)

    private var phone: Process? = null
    private var phoneRadio: Process? = null
    private var phoneMachine: String? = null
    private var phoneState = PhoneStatus()
    private var retryPhoneAt = 0L

    fun available(context: Context) = File(context.applicationInfo.nativeLibraryDir, PROGRAM).canExecute()

    fun running(): Boolean = synchronized(lock) { program?.let(::alive) == true }

    // Starts btvirt unless it runs, and returns the socket path its controllers are on, or null if it can't
    // run. Doesn't wait: btvirt makes its socket a moment after starting, so callers retry their connect.
    // Scans /proc the first time, so never on the main thread.
    fun start(context: Context): String? {
        synchronized(lock) {
            val dir = File(context.noBackupFilesDir, "bt")
            val socket = File(dir, SERVER).path
            if (program?.let(::alive) == true) return socket
            program = startRadio(context, dir)
            return socket.takeIf { program != null }
        }
    }

    private fun startRadio(context: Context, dir: File): Process? {
        val exe = File(context.applicationInfo.nativeLibraryDir, PROGRAM)
        if (!exe.canExecute()) return null
        clean(context, dir)
        dir.mkdirs()
        val started = try {
            ProcessBuilder(exe.path, "-s${dir.path}").redirectErrorStream(true).start()
        } catch (e: IOException) {
            Log.w(TAG, "bluetooth: btvirt didn't start: $e")
            return null
        }
        Thread({ watch(started) }, "FrondBtRadio").apply { isDaemon = true }.start()
        return started
    }

    // Stops btvirt; its controllers are lost.
    fun stop() {
        synchronized(lock) {
            program?.destroy()
            program = null
        }
    }

    // The Sync Manager phone has its own radio, accessible only to the chosen machine.
    fun updatePhone(context: Context, machine: String?): PhoneStatus = synchronized(lock) {
        if (machine == null) {
            stopPhone()
            return phoneState
        }
        if (machine != phoneMachine) stopPhone()
        phoneMachine = machine
        val now = SystemClock.elapsedRealtime()
        if (now < retryPhoneAt) return phoneState
        val dir = File(context.noBackupFilesDir, "bt-phone")
        val socket = File(dir, SERVER).path
        if (phoneRadio?.let(::alive) != true) {
            phone?.destroy()
            phone = null
            phoneRadio = startRadio(context, dir)
            if (phoneRadio == null) {
                retryPhoneAt = now + 5000
                phoneState = PhoneStatus("unavailable")
                return phoneState
            }
        }
        if (phone != null) return phoneState
        val libraryDir = context.applicationInfo.nativeLibraryDir
        val executable = File(libraryDir, PHONE)
        if (!executable.canExecute()) {
            retryPhoneAt = now + 5000
            phoneState = PhoneStatus("unavailable", socket = socket)
            return phoneState
        }
        val arguments = mutableListOf(executable.path, socket, "--dun")
        if (machine.isNotEmpty()) arguments += listOf("--sync-folder", SyncNative.root(context).path, "--sync-machine", machine)
        val builder = ProcessBuilder(arguments).redirectErrorStream(true)
        builder.environment()["LD_LIBRARY_PATH"] = libraryDir
        builder.environment()["SLIRP_RESOLV_CONF"] = File(context.filesDir, HostDns.FILE).path
        val started = try {
            builder.start()
        } catch (e: IOException) {
            Log.w(TAG, "bluetooth: phone didn't start: $e")
            retryPhoneAt = SystemClock.elapsedRealtime() + 5000
            phoneState = PhoneStatus("unavailable", socket = socket)
            return phoneState
        }
        phone = started
        phoneState = PhoneStatus("starting", socket = socket)
        Thread({ watchPhone(started) }, "FrondBtPhone").apply { isDaemon = true }.start()
        phoneState
    }

    private fun stopPhone() {
        phone?.destroy()
        phoneRadio?.destroy()
        phone = null
        phoneRadio = null
        phoneMachine = null
        phoneState = PhoneStatus()
        retryPhoneAt = 0
    }

    private fun watchPhone(process: Process) {
        try {
            process.inputStream.bufferedReader().useLines { lines -> lines.forEach { line ->
                val state = when {
                    line.startsWith("READY ") -> "idle"
                    line == "DUN_STATE off" -> "idle"
                    line.startsWith("DUN_STATE ") -> line.removePrefix("DUN_STATE ")
                    else -> null
                }
                synchronized(lock) {
                    if (phone === process && state != null) phoneState = phoneState.copy(call = state)
                    if (phone === process && line.startsWith("Network HotSync: "))
                        phoneState = phoneState.copy(sync = line.removePrefix("Network HotSync: "))
                }
            } }
        } catch (_: IOException) {}
        val code = try { process.waitFor() } catch (_: InterruptedException) { return }
        synchronized(lock) {
            if (phone === process) {
                Log.w(TAG, "bluetooth: phone stopped ($code)")
                phone = null
                phoneState = phoneState.copy(call = "unavailable", sync = "")
                retryPhoneAt = SystemClock.elapsedRealtime() + 5000
            }
        }
    }

    private fun watch(process: Process) {
        try {
            process.inputStream.bufferedReader().forEachLine { Log.i(TAG, "btvirt: $it") }
        } catch (_: IOException) {}
        val code = try { process.waitFor() } catch (e: InterruptedException) { return }
        Log.i(TAG, "bluetooth: btvirt stopped ($code)")
    }

    // Stops any btvirt left from an earlier process (one that was killed) and removes the stale sockets.
    // Any libbtvirt.so of this package counts, including one from before an app update, whose
    // nativeLibraryDir has gone. Only our uid's processes. Under lock, with no radio of our own running.
    private fun clean(context: Context, dir: File) {
        val uid = Os.getuid()
        val ours = "/${context.packageName}-"
        File("/proc").listFiles()?.forEach { p ->
            val pid = p.name.toIntOrNull() ?: return@forEach
            val owner = try { Os.stat(p.path).st_uid } catch (e: Exception) { return@forEach }
            if (owner != uid) return@forEach
            val command = try { File(p, "cmdline").readBytes() } catch (e: IOException) { return@forEach }
            val commandLine = String(command).split('\u0000', limit = 3)
            val exe = commandLine.first()
            val argument = commandLine.getOrNull(1)
            if (ours in exe && ((exe.endsWith("/$PROGRAM") && argument == "-s${dir.path}") ||
                    (exe.endsWith("/$PHONE") && argument == File(dir, SERVER).path))) {
                try { Os.kill(pid, OsConstants.SIGTERM) } catch (e: Exception) {}
            }
        }
        dir.listFiles()?.forEach { it.delete() }
    }

    private fun alive(p: Process) = try {
        p.exitValue()
        false
    } catch (e: IllegalThreadStateException) {
        true
    }
}
