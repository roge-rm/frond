package com.rm.frond

import android.content.Context
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

// How the Palm's Bluetooth radio reaches other Fronds on the network.
sealed class BtMode {
    object Off : BtMode()
    // Runs the virtual controllers here and takes other Fronds on a port.
    data class Host(val port: Int) : BtMode()
    // Uses the controllers of a Frond hosting at HOST:PORT.
    data class Join(val host: String, val port: Int) : BtMode()

    // Kept in the bluetooth preferences as off, host:PORT or join:HOST:PORT.
    fun encode(): String = when (this) {
        Off -> "off"
        is Host -> "host:$port"
        is Join -> "join:${IrBeam.hostPort(host, port)}"
    }

    companion object {
        fun decode(text: String?): BtMode? = when {
            text == null -> null
            text == "off" -> Off
            text.startsWith("host:") -> IrBeam.port(text.removePrefix("host:"))?.let { Host(it) }
            text.startsWith("join:") -> BtLink.address(text.removePrefix("join:"))?.let { Join(it.first, it.second) }
            else -> null
        }
    }
}

// Bluetooth over the network. The core's --bluetooth endpoint is a Unix socket where each connection is a
// new virtual controller (BlueZ btvirt's bt-server-bredr); controllers in one btvirt can find and talk to
// each other. Host runs btvirt (packaged as libbtvirt.so) as a child process, points the core at it and
// gives each network peer its own connection to it. Join makes a Unix socket for the core and carries each
// connection it makes to a host: another Android Frond hosting, or a desktop btvirt through
// tools/link-bluetooth.py serve. Bytes pass unchanged. The core doesn't save this in sessions, so each
// emulator process starts with it off.
object BtLink {
    const val DEFAULT_PORT = 5032
    private const val TAG = "Frond"
    private const val PREFS = "bluetooth"
    private const val CHUNK = 16384
    private const val PROGRAM = "libbtvirt.so"
    private const val SERVER = "bt-server-bredr"
    private const val JOIN = "join"

    data class Status(
        val mode: BtMode = BtMode.Off,
        // Network peers while hosting, the core's connections carried to the host while joined.
        val links: Int = 0,
        val error: String? = null,
    )

    // HOST:PORT, [IPv6]:PORT or a host alone for 5032.
    fun address(text: String) = IrBeam.address(text, DEFAULT_PORT)

    // The core's bluetooth-mode argument.
    fun coreMode(socket: File?) = socket?.path ?: "off"

    private val state = MutableStateFlow(Status())
    val status: StateFlow<Status> get() = state

    // Held for a whole change, so changes, cleaning and turning off after a failure run one at a time.
    // Taken before lock, never after.
    private val changing = Any()
    private val lock = Any()
    // Bumped on every change; threads from an older setting stop.
    @Volatile private var generation = 0
    private var program: Process? = null
    private var tcpServer: ServerSocket? = null
    private var unixServer: LocalServerSocket? = null
    // The bound socket whose descriptor unixServer listens on. Kept so its finalizer doesn't close the
    // descriptor under the server; LocalServerSocket.close() leaves a descriptor it didn't make open.
    private var unixBound: LocalSocket? = null
    private val pairs = mutableSetOf<Link>()
    @Volatile private var link: ControlLink? = null

    // Sockets and btvirt's socket folder. Private to the app; sun_path needs it under 108 bytes.
    private fun folder(context: Context) = File(context.noBackupFilesDir, "bt")

    fun last(context: Context): BtMode? =
        BtMode.decode(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("mode", null))

    private fun remember(context: Context, mode: BtMode) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("mode", mode.encode()).apply()
    }

    // Sets the core's endpoint and the relay. Blocks on the control link and on btvirt starting, so never on
    // the main thread. Returns an error to show, or null.
    fun set(context: Context, link: ControlLink, mode: BtMode): String? {
        synchronized(changing) {
            // The core lets go of the old endpoint first, so btvirt and the sockets can go.
            val off = link.request("bluetooth-mode", "off", timeoutSeconds = 30)
            close()
            clean(context)
            if (!off.ok) return off.message
            if (mode == BtMode.Off) return null
            remember(context, mode)
            val dir = folder(context)
            dir.mkdirs()
            val gen = synchronized(lock) {
                this.link = link
                ++generation
            }
            val (error, core) = when (mode) {
                is BtMode.Host -> host(context, gen, mode, dir) to File(dir, SERVER)
                is BtMode.Join -> join(gen, mode, File(dir, JOIN)) to File(dir, JOIN)
                BtMode.Off -> return null
            }
            if (error != null) {
                undo(gen)
                return error
            }
            val reply = link.request("bluetooth-mode", coreMode(core), timeoutSeconds = 30)
            if (!reply.ok) {
                undo(gen)
                return reply.message
            }
            Log.i(TAG, "bluetooth: ${mode.encode()}, core on $core")
            return null
        }
    }

    // Closes what a failed change set up, unless something newer has replaced it.
    private fun undo(gen: Int) {
        synchronized(lock) {
            if (gen != generation) return
            ++generation
            closeAll()
            link = null
            state.value = Status()
        }
    }

    private fun host(context: Context, gen: Int, mode: BtMode.Host, dir: File): String? {
        val exe = File(context.applicationInfo.nativeLibraryDir, PROGRAM)
        if (!exe.canExecute()) return "The Bluetooth host isn't in this build."
        val started = try {
            ProcessBuilder(exe.path, "-s${dir.path}").redirectErrorStream(true).start()
        } catch (e: IOException) {
            Log.w(TAG, "bluetooth: btvirt didn't start: $e")
            return "The Bluetooth host couldn't start."
        }
        synchronized(lock) { program = started }
        Thread({ watch(gen, started) }, "FrondBtHost").apply { isDaemon = true }.start()
        val socket = File(dir, SERVER)
        // btvirt makes its sockets once its main loop is up, a moment after starting.
        var ready = false
        for (i in 0 until 50) {
            if (socket.exists() && reachable(socket)) {
                ready = true
                break
            }
            if (!alive(started)) break
            Thread.sleep(100)
        }
        if (!ready) return "The Bluetooth host couldn't start."
        val listener = try {
            ServerSocket().apply {
                reuseAddress = true
                bind(InetSocketAddress(mode.port), 8)
            }
        } catch (e: IOException) {
            return "Couldn't wait on port ${mode.port}. Another app may be using it."
        }
        synchronized(lock) {
            if (gen != generation) {
                listener.close()
                return "Changed while starting."
            }
            tcpServer = listener
            state.value = Status(mode)
        }
        Thread({ acceptPeers(gen, mode, listener, socket) }, "FrondBtWait").apply { isDaemon = true }.start()
        return null
    }

    private fun join(gen: Int, mode: BtMode.Join, path: File): String? {
        path.delete()
        val bound = LocalSocket(LocalSocket.SOCKET_STREAM)
        val server = try {
            bound.bind(LocalSocketAddress(path.path, LocalSocketAddress.Namespace.FILESYSTEM))
            // Listens on the bound socket's descriptor, which stays bound's to close.
            LocalServerSocket(bound.fileDescriptor)
        } catch (e: IOException) {
            Log.w(TAG, "bluetooth: join socket: $e")
            try { bound.close() } catch (_: IOException) {}
            return "Couldn't make the Bluetooth socket."
        }
        synchronized(lock) {
            if (gen != generation) {
                closeServer(server, bound)
                return "Changed while starting."
            }
            unixServer = server
            unixBound = bound
            state.value = Status(mode)
        }
        Thread({ acceptCore(gen, mode, server) }, "FrondBtJoin").apply { isDaemon = true }.start()
        return null
    }

    // Each network peer gets its own controller from btvirt.
    private fun acceptPeers(gen: Int, mode: BtMode.Host, listener: ServerSocket, socket: File) {
        var failures = 0
        while (gen == generation) {
            val peer = try {
                listener.accept().also { failures = 0 }
            } catch (e: IOException) {
                if (gen != generation) return
                if (IrBeam.acceptFailed(e, ++failures)) {
                    Log.w(TAG, "bluetooth: stopped taking peers: $e")
                    fail(gen, "Stopped taking other Fronds.")
                    return
                }
                continue
            }
            val local = LocalSocket(LocalSocket.SOCKET_STREAM)
            try {
                local.connect(LocalSocketAddress(socket.path, LocalSocketAddress.Namespace.FILESYSTEM))
            } catch (e: IOException) {
                Log.w(TAG, "bluetooth: peer ${peer.inetAddress?.hostAddress} not carried: $e")
                shut(peer)
                shut(local)
                continue
            }
            Log.i(TAG, "bluetooth: peer ${peer.inetAddress?.hostAddress} connected")
            begin(gen, mode, peer, local)
        }
    }

    // Each connection the core makes (Palm OS turning Bluetooth on, after a reset or sleep) goes to the host.
    private fun acceptCore(gen: Int, mode: BtMode.Join, server: LocalServerSocket) {
        var failures = 0
        while (gen == generation) {
            val local = try {
                server.accept().also { failures = 0 }
            } catch (e: IOException) {
                if (gen != generation) return
                if (IrBeam.acceptFailed(e, ++failures)) {
                    Log.w(TAG, "bluetooth: stopped taking the core's connections: $e")
                    fail(gen, "The Bluetooth socket stopped working.")
                    return
                }
                continue
            }
            if (gen != generation) {
                shut(local)
                return
            }
            Thread({
                val peer = Socket()
                try {
                    peer.connect(InetSocketAddress(mode.host, mode.port), 10_000)
                } catch (e: Exception) {
                    shut(peer)
                    shut(local)
                    synchronized(lock) {
                        if (gen == generation) state.value = state.value.copy(error = "Couldn't reach ${IrBeam.hostPort(mode.host, mode.port)}.")
                    }
                    return@Thread
                }
                begin(gen, mode, peer, local)
            }, "FrondBtConnect").apply { isDaemon = true }.start()
        }
    }

    private fun begin(gen: Int, mode: BtMode, peer: Socket, local: LocalSocket) {
        try { peer.tcpNoDelay = true } catch (_: IOException) {}
        IrBeam.keepAlive(peer)
        val p = Link(gen, peer, local)
        synchronized(lock) {
            if (gen != generation) {
                p.close()
                return
            }
            pairs += p
            state.value = Status(mode, links = pairs.size)
        }
        p.start()
    }

    private fun ended(p: Link) {
        synchronized(lock) {
            if (!pairs.remove(p) || p.generation != generation) return
            state.value = state.value.copy(links = pairs.size)
        }
    }

    // btvirt stopping by itself takes the controllers with it: Bluetooth goes off with the reason shown.
    private fun watch(gen: Int, process: Process) {
        try {
            process.inputStream.bufferedReader().forEachLine { Log.i(TAG, "btvirt: $it") }
        } catch (_: IOException) {}
        val code = try { process.waitFor() } catch (e: InterruptedException) { return }
        if (gen != generation) return
        Log.w(TAG, "bluetooth: btvirt stopped ($code)")
        fail(gen, "The Bluetooth host stopped.")
    }

    // Turns Bluetooth off with the reason shown, unless a newer setting has replaced gen. Holding changing
    // keeps a set() from starting between the check and the core going off.
    private fun fail(gen: Int, reason: String) {
        synchronized(changing) {
            if (gen != generation) return
            try { link?.request("bluetooth-mode", "off", timeoutSeconds = 30) } catch (_: Exception) {}
            synchronized(lock) {
                if (gen != generation) return
                ++generation
                closeAll()
                link = null
                state.value = Status(error = reason)
            }
        }
    }

    // Closes the relay and btvirt without touching the core, for an emulator that's ending. Doesn't block:
    // the /proc scan runs on its own thread.
    fun stop(context: Context) {
        close()
        cleanLater(context)
    }

    private fun close() {
        synchronized(lock) {
            ++generation
            closeAll()
            link = null
            state.value = Status()
        }
    }

    // Under lock.
    private fun closeAll() {
        try { tcpServer?.close() } catch (_: IOException) {}
        tcpServer = null
        unixServer?.let { closeServer(it, unixBound) }
        unixServer = null
        unixBound = null
        pairs.forEach { it.close() }
        pairs.clear()
        program?.destroy()
        program = null
    }

    // clean() on its own thread, skipped if a setting has been made since.
    fun cleanLater(context: Context) {
        val gen = generation
        Thread({
            synchronized(changing) {
                if (gen == generation) clean(context)
            }
        }, "FrondBtClean").apply { isDaemon = true }.start()
    }

    // Stops any btvirt left from an earlier emulator (one that was killed) and removes the stale sockets.
    // Any libbtvirt.so of this package counts, including one from before an app update, whose
    // nativeLibraryDir has gone. Only our uid's processes. Scans /proc, so never on the main thread.
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

    private fun reachable(socket: File): Boolean {
        val probe = LocalSocket(LocalSocket.SOCKET_STREAM)
        return try {
            probe.connect(LocalSocketAddress(socket.path, LocalSocketAddress.Namespace.FILESYSTEM))
            true
        } catch (e: IOException) {
            false
        } finally {
            shut(probe)
        }
    }

    private fun alive(p: Process) = try {
        p.exitValue()
        false
    } catch (e: IllegalThreadStateException) {
        true
    }

    // Shutting the descriptor down wakes a thread blocked in accept(), which close() alone doesn't.
    private fun closeServer(server: LocalServerSocket, bound: LocalSocket?) {
        try { Os.shutdown(server.fileDescriptor, OsConstants.SHUT_RDWR) } catch (_: Exception) {}
        try { server.close() } catch (_: IOException) {}
        try { bound?.close() } catch (_: IOException) {}
    }

    private fun shut(c: Closeable) {
        if (c is LocalSocket) {
            try { Os.shutdown(c.fileDescriptor, OsConstants.SHUT_RDWR) } catch (_: Exception) {}
        }
        try { c.close() } catch (_: IOException) {}
    }

    // One network connection joined to one controller connection. Either side closing closes both, as
    // a controller can't continue on a new connection.
    private class Link(val generation: Int, val peer: Socket, val local: LocalSocket) {
        @Volatile private var closed = false

        fun start() {
            val a = Thread({ pump({ peer.getInputStream() }, { local.outputStream }) }, "FrondBtIn")
            val b = Thread({ pump({ local.inputStream }, { peer.getOutputStream() }) }, "FrondBtOut")
            for (t in listOf(a, b)) {
                t.isDaemon = true
                t.start()
            }
        }

        private fun pump(from: () -> InputStream, to: () -> OutputStream) {
            val buffer = ByteArray(CHUNK)
            try {
                val input = from()
                val output = to()
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    output.write(buffer, 0, n)
                }
            } catch (_: IOException) {
            }
            close()
            ended(this)
        }

        fun close() {
            if (closed) return
            closed = true
            shut(peer)
            shut(local)
        }
    }
}
