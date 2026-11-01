package com.rm.frond

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.util.Log
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

// The core's desktop control socket (native/DesktopControl.h), seen from the app's side.
// The app listens on a Unix socket path and the core connects to it with --desktop-socket.
// Lines are id<TAB>command<TAB>hex argument; replies id<TAB>ok|error<TAB>code<TAB>hex text,
// and events 0<TAB>event<TAB>name<TAB>hex text. One core per link.
class ControlLink(val path: File, private val onEvent: (String, String) -> Unit) {
    data class Reply(val ok: Boolean, val code: String, val message: String)

    private val bound: LocalSocket
    private val server: LocalServerSocket
    @Volatile private var client: LocalSocket? = null
    @Volatile private var output: OutputStream? = null
    private val connected = CompletableFuture<Unit>()
    private val pending = ConcurrentHashMap<String, CompletableFuture<Reply>>()
    private val nextId = AtomicInteger(1)
    private val fileJobs = ConcurrentHashMap.newKeySet<String>()
    private val panelState = Object()
    private var panelDesired = false
    private var panelPending = false
    private var panelWorkerStarted = false
    @Volatile private var closed = false
    // Set once the reader has stopped; later requests fail at once.
    @Volatile private var finished = false
    @Volatile var connectedOnce = false
        private set

    init {
        path.delete()
        bound = LocalSocket(LocalSocket.SOCKET_STREAM)
        bound.bind(LocalSocketAddress(path.path, LocalSocketAddress.Namespace.FILESYSTEM))
        server = LocalServerSocket(bound.fileDescriptor)
        Thread(::run, "FrondControl").apply { isDaemon = true }.start()
    }

    private fun run() {
        try {
            val accepted = server.accept()
            client = accepted
            output = accepted.outputStream
            connectedOnce = true
            connected.complete(Unit)
            read(accepted.inputStream)
        } catch (e: Exception) {
            if (!closed) Log.w(TAG, "control link: $e")
        } finally {
            finished = true
            synchronized(panelState) { panelState.notifyAll() }
            connected.complete(Unit)
            drain()
        }
    }

    private fun read(input: InputStream) {
        input.bufferedReader(Charsets.US_ASCII).forEachLine { line ->
            val parts = line.split('\t')
            if (parts.size == 4 && parts[0] == "0" && parts[1] == "event") {
                onEvent(parts[2], decode(parts[3]))
            } else if (parts.size == 4) {
                pending.remove(parts[0])?.complete(Reply(parts[1] == "ok", parts[2], decode(parts[3])))
            }
        }
    }

    // Runs action on the link's thread once the core connects, or once the link has ended without it.
    fun whenConnected(action: () -> Unit) {
        connected.thenRun(action)
    }

    // Sends a command and waits for its reply. Never call this on the core's own thread.
    // Never throws: a core that isn't there or stops answers closed or timeout.
    fun request(command: String, argument: String = "", timeoutSeconds: Long = 120,
                cancelRequested: (() -> Boolean)? = null): Reply {
        val id = nextId.getAndIncrement().toString()
        val reply = CompletableFuture<Reply>()
        return try {
            connected.get(timeoutSeconds, TimeUnit.SECONDS)
            val out = output ?: return CLOSED
            pending[id] = reply
            // The reader may have stopped before this request was added.
            if (finished) drain()
            synchronized(this) {
                out.write("$id\t$command\t${encode(argument)}\n".toByteArray(Charsets.US_ASCII))
                if (command in FILE_COMMANDS) {
                    fileJobs.add(id)
                    if (cancelRequested?.invoke() == true) out.write(cancelLine(id))
                }
                out.flush()
            }
            reply.get(timeoutSeconds, TimeUnit.SECONDS)
        } catch (e: java.util.concurrent.TimeoutException) {
            Reply(false, "timeout", "The emulator didn't answer.")
        } catch (e: Exception) {
            CLOSED
        } finally {
            pending.remove(id)
            fileJobs.remove(id)
        }
    }

    // One pending visibility state per connection; lifecycle callbacks never wait for the core.
    fun setPanelVisible(visible: Boolean) = synchronized(panelState) {
        if (closed || finished) return@synchronized
        panelDesired = visible
        panelPending = true
        if (!panelWorkerStarted) {
            panelWorkerStarted = true
            Thread(::sendPanelStates, "FrondPanel").apply { isDaemon = true }.start()
        }
        panelState.notifyAll()
    }

    private fun sendPanelStates() {
        try {
            connected.get()
            val out = output ?: return
            while (true) {
                val visible = synchronized(panelState) {
                    while (!panelPending && !closed && !finished) panelState.wait()
                    if (closed || finished) return
                    panelPending = false
                    panelDesired
                }
                synchronized(this) {
                    if (closed || finished) return
                    out.write("0\tpanel-visible\t${encode(if (visible) "on" else "off")}\n".toByteArray(Charsets.US_ASCII))
                    out.flush()
                }
            }
        } catch (_: Exception) { }
    }

    fun cancelFiles(): Boolean = try {
        val out = output
        if (out == null) false else synchronized(this) {
            val jobs = fileJobs.toList()
            jobs.forEach { out.write(cancelLine(it)) }
            out.flush()
            jobs.isNotEmpty()
        }
    } catch (_: Exception) { false }

    private fun drain() {
        pending.values.forEach { it.complete(CLOSED) }
    }

    fun close() {
        closed = true
        synchronized(panelState) { panelState.notifyAll() }
        try { client?.close() } catch (_: Exception) {}
        try { server.close() } catch (_: Exception) {}
        try { bound.close() } catch (_: Exception) {}
        path.delete()
    }

    private companion object {
        const val TAG = "Frond"
        val FILE_COMMANDS = setOf("ms-files", "ms-get", "ms-put", "ms-mkdir", "ms-delete", "ms-rename")
        fun cancelLine(id: String) = "0\tcancel-files\t${encode(id)}\n".toByteArray(Charsets.US_ASCII)
        val CLOSED = Reply(false, "closed", "The emulator stopped.")

        fun encode(text: String) = text.toByteArray(Charsets.UTF_8).joinToString("") { "%02x".format(it) }
        fun decode(hex: String) = try {
            ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }.toString(Charsets.UTF_8)
        } catch (e: Exception) {
            hex
        }
    }
}
