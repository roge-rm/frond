package com.rm.frond

import android.content.Context
import android.net.ConnectivityManager
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

// How infrared beaming reaches another Frond on the network.
sealed class BeamMode {
    object Off : BeamMode()
    // Waits on all interfaces for one peer at a time.
    data class Listen(val port: Int) : BeamMode()
    data class Connect(val host: String, val port: Int) : BeamMode()

    // Kept in the beam preferences as off, listen:PORT or connect:HOST:PORT.
    fun encode(): String = when (this) {
        Off -> "off"
        is Listen -> "listen:$port"
        is Connect -> "connect:${IrBeam.hostPort(host, port)}"
    }

    companion object {
        fun decode(text: String?): BeamMode? = when {
            text == null -> null
            text == "off" -> Off
            text.startsWith("listen:") -> IrBeam.port(text.removePrefix("listen:"))?.let { Listen(it) }
            text.startsWith("connect:") -> IrBeam.address(text.removePrefix("connect:"))?.let { Connect(it.first, it.second) }
            else -> null
        }
    }
}

// Infrared beaming over the network. The core's --irda tcp:PORT endpoint takes one peer on 127.0.0.1; this
// relay in the emulator's process joins it to another Frond, an Android one running this relay or a desktop
// one through tools/link-irda.py. Bytes pass both ways unchanged. The core doesn't save its infrared mode in
// sessions, so each emulator process starts with beaming off.
object IrBeam {
    const val DEFAULT_PORT = 5030
    private const val TAG = "Frond"
    private const val PREFS = "beam"
    private const val CHUNK = 4096
    // The core listens on IPv4 only; Android's getLoopbackAddress() is ::1.
    private val LOOPBACK: InetAddress = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))

    enum class Phase { OFF, WAITING, CONNECTING, CONNECTED, DISCONNECTED }

    data class Status(
        val mode: BeamMode = BeamMode.Off,
        val phase: Phase = Phase.OFF,
        // The peer's address while connected.
        val peer: String? = null,
        val error: String? = null,
    )

    // "1".."65535", or null.
    fun port(text: String): Int? = text.trim().toIntOrNull()?.takeIf { it in 1..65535 }

    // HOST:PORT, [IPv6]:PORT, or a host alone for the default port. Null when it can't be used.
    fun address(text: String): Pair<String, Int>? {
        val t = text.trim()
        if (t.isEmpty() || t.any { it.isWhitespace() }) return null
        if (t.startsWith("[")) {
            val close = t.indexOf(']')
            if (close < 2) return null
            val host = t.substring(1, close)
            val rest = t.substring(close + 1)
            if (rest.isEmpty()) return host to DEFAULT_PORT
            if (!rest.startsWith(":")) return null
            return port(rest.substring(1))?.let { host to it }
        }
        return when (t.count { it == ':' }) {
            0 -> t to DEFAULT_PORT
            1 -> {
                val host = t.substringBefore(':')
                if (host.isEmpty()) null else port(t.substringAfter(':'))?.let { host to it }
            }
            // A bare IPv6 address, without a port.
            else -> t to DEFAULT_PORT
        }
    }

    fun hostPort(host: String, port: Int) = if (':' in host) "[$host]:$port" else "$host:$port"

    // The core's irda-mode argument for a loopback port, or off.
    fun coreMode(port: Int?) = if (port == null) "off" else "tcp:$port"

    private val state = MutableStateFlow(Status())
    val status: StateFlow<Status> get() = state

    private val lock = Any()
    // Bumped on every change; threads from an older setting stop.
    @Volatile private var generation = 0
    private var corePort = 0
    private var server: ServerSocket? = null
    private var session: Session? = null

    // The last mode set, for the choices shown next time.
    fun last(context: Context): BeamMode? =
        BeamMode.decode(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("mode", null))

    private fun remember(context: Context, mode: BeamMode) {
        val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        p.edit().putString("mode", mode.encode()).apply()
    }

    // Sets the core's endpoint and the relay. Blocks on the control link, so never on the main thread.
    // Returns an error to show, or null.
    fun set(context: Context, link: ControlLink, mode: BeamMode): String? {
        stop()
        if (mode == BeamMode.Off) {
            val reply = link.request("irda-mode", "off", timeoutSeconds = 30)
            return if (reply.ok) null else reply.message
        }
        val port = try {
            ServerSocket(0, 1, LOOPBACK).use { it.localPort }
        } catch (e: IOException) {
            return "Couldn't find a free port."
        }
        val reply = link.request("irda-mode", coreMode(port), timeoutSeconds = 30)
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
                        Thread({ accept(gen, mode, listener) }, "FrondBeamWait").apply { isDaemon = true }.start()
                    }
                }
                is BeamMode.Connect -> connectLater(gen, mode)
                BeamMode.Off -> {}
            }
        }
        if (failed != null) {
            link.request("irda-mode", "off", timeoutSeconds = 30)
            return failed
        }
        Log.i(TAG, "beaming: ${mode.encode()}, core on 127.0.0.1:$port")
        return null
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
        }, "FrondBeamConnect").apply { isDaemon = true }.start()
    }

    private fun accept(gen: Int, mode: BeamMode.Listen, listener: ServerSocket) {
        while (gen == generation) {
            val peer = try { listener.accept() } catch (e: IOException) { return }
            synchronized(lock) {
                // One peer at a time, like the core.
                if (gen != generation || session != null) {
                    try { peer.close() } catch (_: IOException) {}
                } else {
                    begin(gen, mode, peer)
                }
            }
        }
    }

    // Under lock.
    private fun begin(gen: Int, mode: BeamMode, peer: Socket) {
        val s = Session(gen, mode, peer, corePort)
        session = s
        state.value = Status(mode, Phase.CONNECTED, peer = peer.inetAddress?.hostAddress)
        Log.i(TAG, "beaming: peer ${peer.inetAddress?.hostAddress} connected")
        s.start()
    }

    private fun ended(s: Session, reason: String?) {
        synchronized(lock) {
            if (session !== s) return
            session = null
            if (s.generation != generation) return
            Log.i(TAG, "beaming: peer ended (${reason ?: "closed"})")
            state.value = when (val mode = s.mode) {
                is BeamMode.Listen -> Status(mode, Phase.WAITING)
                is BeamMode.Connect -> Status(mode, Phase.DISCONNECTED, error = reason)
                BeamMode.Off -> Status()
            }
        }
    }

    // One network peer joined to the core's endpoint. The core drops its side on reset, sleep or a loaded
    // session; then this connects to it again and keeps the network peer.
    private class Session(val generation: Int, val mode: BeamMode, val peer: Socket, val corePort: Int) {
        @Volatile private var core: Socket? = null
        @Volatile private var closed = false

        fun start() {
            try { peer.tcpNoDelay = true } catch (_: IOException) {}
            Thread(::fromPeer, "FrondBeamIn").apply { isDaemon = true }.start()
            Thread(::fromCore, "FrondBeamOut").apply { isDaemon = true }.start()
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

        // Network to core. Bytes that arrive while the core side is reconnecting are dropped; IrDA retries.
        private fun fromPeer() {
            val buffer = ByteArray(CHUNK)
            try {
                val input = peer.getInputStream()
                while (!closed) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    try { core?.getOutputStream()?.write(buffer, 0, n) } catch (_: IOException) {}
                }
                finish("The other Frond closed the connection.")
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
                    if (++failures >= 10) {
                        Log.w(TAG, "beaming: can't reach the core on 127.0.0.1:$corePort: $e")
                        finish("The emulator's infrared stopped.")
                        return
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

    // This device's IPv4 addresses, for the other side to connect to.
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
