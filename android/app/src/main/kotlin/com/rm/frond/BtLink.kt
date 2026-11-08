package com.rm.frond

import android.content.Context
import android.system.Os
import android.system.OsConstants
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
    private const val SERVER = "bt-server-bredr"

    private val lock = Any()
    private var program: Process? = null

    // btvirt's socket folder. Private to the app; sun_path needs it under 108 bytes.
    private fun folder(context: Context) = File(context.noBackupFilesDir, "bt")

    fun available(context: Context) = File(context.applicationInfo.nativeLibraryDir, PROGRAM).canExecute()

    fun running(): Boolean = synchronized(lock) { program?.let(::alive) == true }

    // Starts btvirt unless it runs, and returns the socket path its controllers are on, or null if it can't
    // run. Doesn't wait: btvirt makes its socket a moment after starting, so callers retry their connect.
    // Scans /proc the first time, so never on the main thread.
    fun start(context: Context): String? {
        synchronized(lock) {
            val socket = File(folder(context), SERVER).path
            if (program?.let(::alive) == true) return socket
            program = null
            val exe = File(context.applicationInfo.nativeLibraryDir, PROGRAM)
            if (!exe.canExecute()) return null
            clean(context)
            val dir = folder(context)
            dir.mkdirs()
            val started = try {
                ProcessBuilder(exe.path, "-s${dir.path}").redirectErrorStream(true).start()
            } catch (e: IOException) {
                Log.w(TAG, "bluetooth: btvirt didn't start: $e")
                return null
            }
            program = started
            Thread({ watch(started) }, "FrondBtRadio").apply { isDaemon = true }.start()
            Log.i(TAG, "bluetooth: radio on $socket")
            return socket
        }
    }

    // Stops btvirt; its controllers are lost.
    fun stop() {
        synchronized(lock) {
            program?.destroy()
            program = null
        }
    }

    private fun watch(process: Process) {
        try {
            process.inputStream.bufferedReader().forEachLine { Log.i(TAG, "btvirt: $it") }
        } catch (_: IOException) {}
        val code = try { process.waitFor() } catch (e: InterruptedException) { return }
        Log.i(TAG, "bluetooth: btvirt stopped ($code)")
        synchronized(lock) { if (program === process) program = null }
    }

    // clean() on its own thread.
    fun cleanLater(context: Context) {
        Thread({ synchronized(lock) { if (program == null) clean(context) } }, "FrondBtClean").apply { isDaemon = true }.start()
    }

    // Stops any btvirt left from an earlier process (one that was killed) and removes the stale sockets.
    // Any libbtvirt.so of this package counts, including one from before an app update, whose
    // nativeLibraryDir has gone. Only our uid's processes. Under lock, with no radio of our own running.
    private fun clean(context: Context) {
        val uid = Os.getuid()
        val ours = "/${context.packageName}-"
        File("/proc").listFiles()?.forEach { p ->
            val pid = p.name.toIntOrNull() ?: return@forEach
            val owner = try { Os.stat(p.path).st_uid } catch (e: Exception) { return@forEach }
            if (owner != uid) return@forEach
            val command = try { File(p, "cmdline").readBytes() } catch (e: IOException) { return@forEach }
            val exe = String(command).substringBefore('\u0000')
            if (exe.endsWith("/$PROGRAM") && ours in exe) {
                try { Os.kill(pid, OsConstants.SIGTERM) } catch (e: Exception) {}
            }
        }
        folder(context).listFiles()?.forEach { it.delete() }
    }

    private fun alive(p: Process) = try {
        p.exitValue()
        false
    } catch (e: IllegalThreadStateException) {
        true
    }
}
