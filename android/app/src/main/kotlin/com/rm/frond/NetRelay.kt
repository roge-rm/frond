package com.rm.frond

import android.content.Context
import android.net.ConnectivityManager
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

// How a relayed endpoint (infrared beaming, the serial port, USB HotSync) reaches another machine on the network.
sealed class BeamMode {
    object Off : BeamMode()
    // Waits on all interfaces for one peer at a time.
    data class Listen(val port: Int) : BeamMode()
    data class Connect(val host: String, val port: Int) : BeamMode()

    // Kept in the relay's preferences as off, listen:PORT or connect:HOST:PORT.
    fun encode(): String = when (this) {
        Off -> "off"
        is Listen -> "listen:$port"
        is Connect -> "connect:${NetRelay.hostPort(host, port)}"
    }

    companion object {
        fun decode(text: String?): BeamMode? = when {
            text == null -> null
            text == "off" -> Off
            text.startsWith("listen:") -> NetRelay.port(text.removePrefix("listen:"))?.let { Listen(it) }
            // Saved modes always carry a port; the 1 never applies.
            text.startsWith("connect:") -> NetRelay.address(text.removePrefix("connect:"), 1)?.let { Connect(it.first, it.second) }
            else -> null
        }
    }
}

// Joins one of the core's tcp:PORT endpoints (127.0.0.1, one client at a time) to a peer on the network: another
// Frond running the same relay, or a desktop through tools/link-irda.py, socat and the like. Bytes pass both ways
// unchanged. Runs in the emulator's process. The core saves none of these endpoints in sessions, so each emulator
// process starts with them off. modeCommand and stateCommand are the core's, such as irda-mode and irda-state.
abstract class NetRelay(
    val defaultPort: Int,
    private val modeCommand: String,
    private val stateCommand: String,
    // Preferences file for the last mode set.
    private val prefs: String,
    // For logs and thread names.
    private val what: String,
) {
    enum class Phase { OFF, WAITING, CONNECTING, CONNECTED, DISCONNECTED }

    data class Status(
        val mode: BeamMode = BeamMode.Off,
        val phase: Phase = Phase.OFF,
        // The peer's address while connected.
        val peer: String? = null,
        val error: String? = null,
    )

    // Reads the core's stateCommand reply: null while its endpoint is still tcp on port, otherwise the reason
    // it left, to show.
    abstract fun coreLeftReason(state: String, port: Int): String?

    // What the peer is called in messages: another Frond for infrared, anything for serial and USB.
    protected open val closedMessage = "The other side closed the connection."
    protected open val stoppedMessage = "Stopped waiting for connections."

    // HOST:PORT, [IPv6]:PORT, or a host alone for this relay's default port.
    fun address(text: String): Pair<String, Int>? = address(text, defaultPort)

    private val state = MutableStateFlow(Status())
    val status: StateFlow<Status> get() = state

    // Held for a whole change, so changes and turning off after a failure run one at a time. Taken
    // before lock, never after.
    private val changing = Any()
    private val lock = Any()
    // Bumped on every change; threads from an older setting stop.
    @Volatile private var generation = 0
    private var corePort = 0
    private var server: ServerSocket? = null
    private var session: Session? = null
    // FrondActivity's control link, for asking the core whether its endpoint is still on our port.
    @Volatile private var link: ControlLink? = null

    // The last mode set, for the choices shown next time.
    fun last(context: Context): BeamMode? =
        BeamMode.decode(context.getSharedPreferences(prefs, Context.MODE_PRIVATE).getString("mode", null))

    private fun remember(context: Context, mode: BeamMode) {
        val p = context.getSharedPreferences(prefs, Context.MODE_PRIVATE)
        p.edit().putString("mode", mode.encode()).apply()
    }

    // Sets the core's endpoint and the relay. Blocks on the control link, so never on the main thread.
    // Returns an error to show, or null.
    fun set(context: Context, link: ControlLink, mode: BeamMode): String? {
        synchronized(changing) {
            stop()
            this.link = link
            if (mode == BeamMode.Off) {
                val reply = link.request(modeCommand, "off", timeoutSeconds = 30)
                return if (reply.ok) null else reply.message
            }
            val port = try {
                ServerSocket(0, 1, LOOPBACK).use { it.localPort }
            } catch (e: IOException) {
                return "Couldn't find a free port."
            }
            val reply = link.request(modeCommand, coreMode(port), timeoutSeconds = 30)
            if (!reply.ok) return reply.message
            remember(context, mode)
            var failed: String? = null
            synchronized(lock) {
                val gen = ++generation
                corePort = port
                when (mode) {
                    is BeamMode.Listen -> {
                        val listener = try {
                            ServerSocket().apply {
                                reuseAddress = true
                                bind(InetSocketAddress(mode.port), 4)
                            }
                        } catch (e: IOException) {
                            null
                        }
                        if (listener == null) {
                            ++generation
                            failed = "Couldn't wait on port ${mode.port}. Another app may be using it."
                        } else {
                            server = listener
                            state.value = Status(mode, Phase.WAITING)
                            Thread({ accept(gen, mode, listener) }, "FrondRelayWait-$what").apply { isDaemon = true }.start()
                        }
                    }
                    is BeamMode.Connect -> connectLater(gen, mode)
                    BeamMode.Off -> {}
                }
            }
            if (failed != null) {
                link.request(modeCommand, "off", timeoutSeconds = 30)
                return failed
            }
            Log.i(TAG, "$what: ${mode.encode()}, core on 127.0.0.1:$port")
            return null
        }
    }

    // Connects again after a Connect peer was lost.
    fun retry() {
        synchronized(lock) {
            val mode = state.value.mode as? BeamMode.Connect ?: return
            if (state.value.phase != Phase.DISCONNECTED) return
            connectLater(++generation, mode)
        }
    }

    // Closes the relay without touching the core, for an emulator that's ending or a new setting.
    fun stop() {
        synchronized(lock) {
            ++generation
            try { server?.close() } catch (_: IOException) {}
            server = null
            session?.close()
            session = null
            link = null
            state.value = Status()
        }
    }

    private fun connectLater(gen: Int, mode: BeamMode.Connect) {
        state.value = Status(mode, Phase.CONNECTING)
        Thread({
            val peer = Socket()
            val error = try {
                peer.connect(InetSocketAddress(mode.host, mode.port), 10_000)
                null
            } catch (e: Exception) {
                try { peer.close() } catch (_: IOException) {}
                "Couldn't connect to ${hostPort(mode.host, mode.port)}."
            }
            synchronized(lock) {
                if (gen != generation) {
                    try { peer.close() } catch (_: IOException) {}
                    return@Thread
                }
                if (error != null) state.value = Status(mode, Phase.DISCONNECTED, error = error)
                else begin(gen, mode, peer)
            }
        }, "FrondRelayConnect-$what").apply { isDaemon = true }.start()
    }

    private fun accept(gen: Int, mode: BeamMode.Listen, listener: ServerSocket) {
        var failures = 0
        while (gen == generation) {
            val peer = try {
                listener.accept().also { failures = 0 }
            } catch (e: IOException) {
                if (gen != generation) return
                if (acceptFailed(e, ++failures)) {
                    Log.w(TAG, "$what: stopped waiting for peers: $e")
                    fail(gen, stoppedMessage)
                    return
                }
                continue
            }
            synchronized(lock) {
                // One peer at a time, like the core. A new peer replaces the old one, which may have
                // vanished without closing (a phone leaving the network).
                if (gen != generation) {
                    try { peer.close() } catch (_: IOException) {}
                } else {
                    session?.let {
                        Log.i(TAG, "$what: a new peer replaces ${it.peer.inetAddress?.hostAddress}")
                        it.close()
                    }
                    session = null
                    begin(gen, mode, peer)
                }
            }
        }
    }

    // Turns the relay off with the reason shown, unless a newer setting has replaced gen. Holding changing
    // keeps a set() from starting between the check and the core going off.
    private fun fail(gen: Int, reason: String) {
        synchronized(changing) {
            if (gen != generation) return
            try { link?.request(modeCommand, "off", timeoutSeconds = 30) } catch (_: Exception) {}
            synchronized(lock) {
                if (gen != generation) return
                ++generation
                try { server?.close() } catch (_: IOException) {}
                server = null
                session?.close()
                session = null
                link = null
                state.value = Status(error = reason)
            }
        }
    }

    // Under lock.
    private fun begin(gen: Int, mode: BeamMode, peer: Socket) {
        val s = Session(gen, mode, peer, corePort)
        session = s
        state.value = Status(mode, Phase.CONNECTED, peer = peer.inetAddress?.hostAddress)
        Log.i(TAG, "$what: peer ${peer.inetAddress?.hostAddress} connected")
        s.start()
    }

    private fun ended(s: Session, reason: String?) {
        synchronized(lock) {
            if (session !== s) return
            session = null
            if (s.generation != generation) return
            Log.i(TAG, "$what: peer ended (${reason ?: "closed"})")
            state.value = when (val mode = s.mode) {
                is BeamMode.Listen -> Status(mode, Phase.WAITING)
                is BeamMode.Connect -> Status(mode, Phase.DISCONNECTED, error = reason)
                BeamMode.Off -> Status()
            }
        }
    }

    // The core's endpoint left our port (turned off, or for infrared a loaded session restored the keyboard):
    // the relay is off and the reason shows.
    private fun coreLeft(s: Session, reason: String) {
        synchronized(lock) {
            if (session !== s) return
            session = null
            if (s.generation != generation) return
            ++generation
            try { server?.close() } catch (_: IOException) {}
            server = null
            Log.i(TAG, "$what: off, $reason")
            state.value = Status(error = reason)
        }
    }

    // Asks the core whether its endpoint is still tcp on this port. Null means yes or can't tell yet (a busy
    // core); otherwise the reason it isn't.
    private fun coreGone(port: Int): String? {
        val l = link ?: return "The emulator stopped."
        val reply = l.request(stateCommand, timeoutSeconds = 10)
        if (!reply.ok) return if (reply.code == "timeout") null else reply.message
        return coreLeftReason(reply.message, port)
    }

    // One network peer joined to the core's endpoint. The core can drop its side (infrared on reset, a loaded
    // session, and while the Palm sleeps); this keeps the network peer and connects to the core again for as
    // long as the core's endpoint stays tcp on this port.
    private inner class Session(val generation: Int, val mode: BeamMode, val peer: Socket, val corePort: Int) {
        @Volatile private var core: Socket? = null
        @Volatile private var closed = false

        fun start() {
            try { peer.tcpNoDelay = true } catch (_: IOException) {}
            keepAlive(peer)
            Thread(::fromPeer, "FrondRelayIn-$what").apply { isDaemon = true }.start()
            Thread(::fromCore, "FrondRelayOut-$what").apply { isDaemon = true }.start()
        }

        fun close() {
            closed = true
            try { peer.close() } catch (_: IOException) {}
            try { core?.close() } catch (_: IOException) {}
        }

        private fun finish(reason: String?) {
            if (closed) return
            close()
            ended(this, reason)
        }

        // Network to core. Bytes that arrive while the core side is reconnecting are dropped, as on a wire
        // with nothing at the other end.
        private fun fromPeer() {
            val buffer = ByteArray(CHUNK)
            try {
                val input = peer.getInputStream()
                while (!closed) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    try { core?.getOutputStream()?.write(buffer, 0, n) } catch (_: IOException) {}
                }
                finish(closedMessage)
            } catch (e: IOException) {
                finish("The connection was lost.")
            }
        }

        // Core to network, connecting to the core again when it drops its side.
        private fun fromCore() {
            val buffer = ByteArray(CHUNK)
            var failures = 0
            while (!closed) {
                val c = try {
                    Socket().apply {
                        connect(InetSocketAddress(LOOPBACK, corePort), 2_000)
                        tcpNoDelay = true
                    }
                } catch (e: IOException) {
                    // A sleeping Palm keeps the core's infrared port closed until it wakes, which can be any time.
                    if (++failures % 10 == 0) {
                        val gone = coreGone(corePort)
                        if (gone != null) {
                            Log.w(TAG, "$what: core left 127.0.0.1:$corePort: $gone")
                            if (!closed) {
                                close()
                                coreLeft(this, gone)
                            }
                            return
                        }
                    }
                    Thread.sleep(300)
                    continue
                }
                failures = 0
                core = c
                if (closed) {
                    try { c.close() } catch (_: IOException) {}
                    return
                }
                try {
                    val input = c.getInputStream()
                    val output = peer.getOutputStream()
                    while (true) {
                        val n = try { input.read(buffer) } catch (e: IOException) { -1 }
                        if (n < 0) break
                        output.write(buffer, 0, n)
                    }
                } catch (e: IOException) {
                    finish("The connection was lost.")
                    return
                } finally {
                    core = null
                    try { c.close() } catch (_: IOException) {}
                }
                if (!closed) Thread.sleep(200)
            }
        }
    }

    companion object {
        private const val TAG = "Frond"
        private const val CHUNK = 4096
        // The core listens on IPv4 only; Android's getLoopbackAddress() is ::1.
        private val LOOPBACK: InetAddress = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))

        // "1".."65535", or null.
        fun port(text: String): Int? = text.trim().toIntOrNull()?.takeIf { it in 1..65535 }

        // HOST:PORT, [IPv6]:PORT, or a host alone for defaultPort. Null when it can't be used.
        fun address(text: String, defaultPort: Int): Pair<String, Int>? {
            val t = text.trim()
            if (t.isEmpty() || t.any { it.isWhitespace() }) return null
            if (t.startsWith("[")) {
                val close = t.indexOf(']')
                if (close < 2) return null
                val host = t.substring(1, close)
                val rest = t.substring(close + 1)
                if (rest.isEmpty()) return host to defaultPort
                if (!rest.startsWith(":")) return null
                return port(rest.substring(1))?.let { host to it }
            }
            return when (t.count { it == ':' }) {
                0 -> t to defaultPort
                1 -> {
                    val host = t.substringBefore(':')
                    if (host.isEmpty()) null else port(t.substringAfter(':'))?.let { host to it }
                }
                // A bare IPv6 address, without a port.
                else -> t to defaultPort
            }
        }

        fun hostPort(host: String, port: Int) = if (':' in host) "[$host]:$port" else "$host:$port"

        // The core's *-mode argument for a loopback port, or off.
        fun coreMode(port: Int?) = if (port == null) "off" else "tcp:$port"

        // For a listener whose accept() threw: false to try again after a moment, true when it won't work again.
        // A peer that gave up mid-handshake (ECONNABORTED), a signal or running short of descriptors or memory
        // passes; a closed or broken descriptor, or 50 failures in a row (about 10 s), doesn't.
        fun acceptFailed(e: IOException, failures: Int): Boolean {
            var cause: Throwable? = e
            while (cause != null && cause !is ErrnoException) cause = cause.cause
            val errno = if (cause is ErrnoException) cause.errno else null
            if (errno == OsConstants.EBADF || errno == OsConstants.EINVAL || errno == OsConstants.ENOTSOCK) return true
            val message = e.message ?: ""
            if (listOf("closed", "EBADF", "Bad file descriptor").any { message.contains(it, ignoreCase = true) }) return true
            if (failures >= 50) return true
            try { Thread.sleep(200) } catch (_: InterruptedException) { return true }
            return false
        }

        // TCP keepalive, so a peer that vanished without closing is noticed in about two minutes. The idle
        // and interval options are Linux's TCP_KEEPIDLE (4), TCP_KEEPINTVL (5) and TCP_KEEPCNT (6) in seconds
        // and counts, which OsConstants doesn't name.
        fun keepAlive(socket: Socket) {
            try { socket.keepAlive = true } catch (_: IOException) {}
            try {
                ParcelFileDescriptor.fromSocket(socket).use { pfd ->
                    Os.setsockoptInt(pfd.fileDescriptor, OsConstants.IPPROTO_TCP, 4, 60)
                    Os.setsockoptInt(pfd.fileDescriptor, OsConstants.IPPROTO_TCP, 5, 15)
                    Os.setsockoptInt(pfd.fileDescriptor, OsConstants.IPPROTO_TCP, 6, 4)
                }
            } catch (e: Exception) {
                Log.w(TAG, "keepalive times not set: $e")
            }
        }

        // This device's IPv4 addresses, for the other side to connect to. Walks the network interfaces, so never
        // on the main thread.
        fun addresses(context: Context): List<String> {
            val found = mutableListOf<String>()
            try {
                val connectivity = context.getSystemService(ConnectivityManager::class.java)
                connectivity?.activeNetwork?.let { connectivity.getLinkProperties(it) }?.linkAddresses?.forEach {
                    val a = it.address
                    if (a is Inet4Address && !a.isLoopbackAddress) found += a.hostAddress ?: return@forEach
                }
            } catch (e: RuntimeException) {
            }
            if (found.isEmpty()) {
                try {
                    NetworkInterface.getNetworkInterfaces()?.toList()?.filter { it.isUp && !it.isLoopback }?.forEach { i ->
                        i.inetAddresses.toList().filterIsInstance<Inet4Address>().forEach { found += it.hostAddress ?: "" }
                    }
                } catch (e: Exception) {
                }
            }
            return found.filter { it.isNotEmpty() }.distinct()
        }
    }
}

// Infrared beaming. The core's irda-state is "tcp:PORT 127.0.0.1:PORT waiting|connected[ suspended][ error: ...]"
// or "off[ error: ...]"; a sleeping Palm shows suspended and has its port closed until it wakes.
object IrBeam : NetRelay(5030, "irda-mode", "irda-state", "beam", "beaming") {
    const val DEFAULT_PORT = 5030
    override val closedMessage = "The other Frond closed the connection."
    override val stoppedMessage = "Stopped waiting for other Fronds."

    override fun coreLeftReason(state: String, port: Int): String? {
        if (state.startsWith("tcp:$port ")) return null
        return state.substringAfter("error: ", "").ifEmpty { "The emulator's infrared stopped." }
    }
}

// The TX's cradle serial port. serial-state is "tcp 127.0.0.1:PORT waiting|connected" or "off[ reason]". The core
// keeps the client through reset, Palm sleep and loads. The E2 has no serial port.
object SerialLink : NetRelay(5033, "serial-mode", "serial-state", "serial", "serial") {
    const val DEFAULT_PORT = 5033

    override fun coreLeftReason(state: String, port: Int): String? =
        tcpLeftReason(state, port, "The emulator's serial port stopped.")
}

// USB HotSync on the TX and E2. usb-state is "tcp 127.0.0.1:PORT waiting|connected PHASE" or "off[ reason]"; the
// core keeps the client through reset, Palm sleep and loads, which only replug the cable.
object UsbLink : NetRelay(5034, "usb-mode", "usb-state", "usb", "usb") {
    const val DEFAULT_PORT = 5034

    override fun coreLeftReason(state: String, port: Int): String? =
        tcpLeftReason(state, port, "The emulator's USB stopped.")
}

// For serial-state and usb-state: null while still tcp on port, otherwise the core's reason or fallback.
fun tcpLeftReason(state: String, port: Int, fallback: String): String? {
    if (state.startsWith("tcp 127.0.0.1:$port ")) return null
    // "off REASON" after a failed switch; anything else (plain off, another port, a pty) has no reason to show.
    return if (state.startsWith("off ")) state.removePrefix("off ").trim().ifEmpty { fallback } else fallback
}
