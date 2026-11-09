package com.rm.frond

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

// Runs one LinkEngine per install: in the emulator process while a machine with a connection set runs,
// else in the home process while Connections is open. The process hosting it serves the other over an
// abstract local socket (the caller's uid is checked) with the hub's JSON lines. An emulator that finds
// the home process hosting asks it to yield and takes over.
object Links {
    private const val TAG = "Frond"

    // The engine clients get; null while one stops.
    private var engine: LinkEngine? = null
    // Every engine still running here, with its server.
    private val servers = HashMap<LinkEngine, LocalServerSocket>()
    private val remotes = HashMap<LinkEngine, MutableSet<RemoteClient>>()
    private val local = mutableSetOf<LinkConnection>()
    private var multicast: WifiManager.MulticastLock? = null
    @Volatile private var app: Context? = null

    fun socketName(context: Context) = context.packageName + ".link"

    // Android 17's local network access (targetSdk 37) is a runtime permission.
    const val LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK"
    fun needsPermission(context: Context) =
        Build.VERSION.SDK_INT >= 37 && context.checkSelfPermission(LOCAL_NETWORK) != PackageManager.PERMISSION_GRANTED

    // A connection to the engine, hosting it here if no other process does. preferHost: the emulator,
    // which takes the engine over from the home process.
    fun open(context: Context, preferHost: Boolean, onEvent: (JSONObject) -> Unit): LinkConnection {
        app = context.applicationContext
        return LinkConnection(context.applicationContext, preferHost, onEvent).also { it.begin() }
    }

    // The engine this process hosts, or null.
    @Synchronized
    internal fun hosted(): LinkEngine? = engine

    // Starts the engine and the local server here. Null when another process holds either.
    @Synchronized
    internal fun host(context: Context): LinkEngine? {
        engine?.let { return it }
        val socket = try { LocalServerSocket(socketName(context)) } catch (e: IOException) { return null }
        val store = LinkStore(LinkStore.folder(context.filesDir))
        val error = store.open(Build.MODEL ?: "Android")
        if (error != null) {
            Log.w(TAG, "link: $error")
            try { socket.close() } catch (_: IOException) {}
            return null
        }
        lateinit var e: LinkEngine
        e = LinkEngine(store, object : LinkEngine.Host {
            override val discoveryPort = LinkWire.DEFAULT_PORT
            override fun discovery(on: Boolean) = multicast(context, on)
            override fun networkAllowed() = !needsPermission(context)
            override fun stopped() = engineStopped(e)
            override fun radioAvailable() = BtLink.available(context)
            override fun startRadio() = BtLink.start(context)
            override fun radioRunning() = BtLink.running()
            override fun stopRadio() = BtLink.stop()
        })
        val failed = e.start()
        if (failed != null) {
            Log.w(TAG, "link: $failed")
            store.close()
            try { socket.close() } catch (_: IOException) {}
            return null
        }
        engine = e
        servers[e] = socket
        remotes[e] = mutableSetOf()
        Thread({ serve(context, socket, e) }, "FrondLinkServer").apply { isDaemon = true; start() }
        return e
    }

    @Synchronized
    private fun engineStopped(e: LinkEngine) {
        if (engine === e) engine = null
        // Closing an accepting LocalServerSocket needs shutdown on its descriptor to wake accept().
        servers.remove(e)?.let { s ->
            try { android.system.Os.shutdown(s.fileDescriptor, android.system.OsConstants.SHUT_RDWR) } catch (_: Exception) {}
            try { s.close() } catch (_: IOException) {}
        }
        remotes.remove(e)?.toList()?.forEach { it.close() }
        for (c in local.toList()) c.engineGone(e)
        local.removeAll { true }
    }

    @Synchronized
    internal fun addLocal(c: LinkConnection) { local += c }

    @Synchronized
    internal fun dropLocal(c: LinkConnection, e: LinkEngine) {
        local -= c
        stopIfIdle(e)
    }

    @Synchronized
    private fun stopIfIdle(e: LinkEngine) {
        if (engine === e && local.isEmpty() && remotes[e].isNullOrEmpty()) {
            engine = null
            Thread({ e.stop() }, "FrondLinkStop").start()
        }
    }

    private fun multicast(context: Context, on: Boolean) {
        try {
            if (on && multicast == null) {
                val wifi = context.getSystemService(WifiManager::class.java) ?: return
                multicast = wifi.createMulticastLock("frond-link").apply { setReferenceCounted(false); acquire() }
            } else if (!on) {
                multicast?.release()
                multicast = null
            }
        } catch (e: Exception) {
            Log.w(TAG, "link: multicast lock: $e")
        }
    }

    // The other process's connections. Only this app's uid gets in.
    private fun serve(context: Context, socket: LocalServerSocket, e: LinkEngine) {
        while (true) {
            val s = try { socket.accept() } catch (ex: IOException) { return }
            val uid = try { s.peerCredentials.uid } catch (ex: IOException) { -1 }
            if (uid != android.os.Process.myUid()) { try { s.close() } catch (_: IOException) {}; continue }
            val client = RemoteClient(s, e)
            synchronized(this) {
                if (engine !== e) { try { s.close() } catch (_: IOException) {}; return }
                remotes[e]?.add(client)
            }
            client.start()
        }
    }

    // One connection from the other process: its lines go to the engine, events go back through a queue
    // so a slow reader never holds up the engine.
    private class RemoteClient(val socket: LocalSocket, val engine: LinkEngine) : LinkEngine.Client {
        private val queue = LinkedBlockingQueue<String>(256)
        @Volatile private var open = true

        override fun send(event: JSONObject) {
            if (open && !queue.offer(event.toString() + "\n")) close()
        }

        fun start() {
            engine.connect(this)
            Thread({ write() }, "FrondLinkOut").apply { isDaemon = true; start() }
            Thread({ read() }, "FrondLinkIn").apply { isDaemon = true; start() }
        }

        private fun write() {
            try {
                val out = socket.outputStream
                while (open) {
                    val line = queue.poll(500, TimeUnit.MILLISECONDS) ?: continue
                    out.write(line.toByteArray(Charsets.UTF_8))
                }
            } catch (_: Exception) {}
            close()
        }

        private fun read() {
            try {
                val reader = socket.inputStream.bufferedReader()
                while (open) {
                    val line = reader.readLine() ?: break
                    if (line.length > 1 shl 16) break
                    val op = try { JSONObject(line) } catch (_: Exception) { continue }
                    engine.handle(this, op)
                }
            } catch (_: IOException) {}
            close()
        }

        fun close() {
            if (!open) return
            open = false
            try { socket.close() } catch (_: IOException) {}
            engine.disconnect(this)
            synchronized(Links) {
                remotes[engine]?.remove(this)
                stopIfIdle(engine)
            }
        }
    }
}

// One client of the engine, wherever it runs, as desktop/LinkClient does: sticky ops (watch, ir-attach) are
// sent again after every reconnect, and a lost engine is found or started again while any are set. Events
// come on a background thread.
class LinkConnection internal constructor(
    private val context: Context,
    private val preferHost: Boolean,
    private val onEvent: (JSONObject) -> Unit,
) {
    private val lock = Object()
    private val sticky = LinkedHashMap<String, JSONObject>()
    private var engine: LinkEngine? = null
    private var client: LinkEngine.Client? = null
    private var remote: LocalSocket? = null
    private var remoteOut: OutputStream? = null
    private var closed = false
    private var lost = false
    // After the home process yields, it leaves the emulator a few seconds to take over.
    private var holdUntil = 0L

    val connected get() = synchronized(lock) { engine != null || remote != null }

    internal fun begin() {
        Thread(::manage, "FrondLinkClient").apply { isDaemon = true; start() }
    }

    fun send(op: JSONObject) {
        synchronized(lock) {
            engine?.let { e -> client?.let { e.handle(it, op) }; return }
            val out = remoteOut ?: return
            try { out.write((op.toString() + "\n").toByteArray(Charsets.UTF_8)) } catch (_: IOException) {}
        }
    }

    // Replaces the op under key, sent now and after every reconnect; null clears it. A cleared or replaced
    // ir, serial or bt attach is detached first, as the engine takes one of each per client.
    fun setSticky(key: String, op: JSONObject?) {
        synchronized(lock) {
            if (key in ATTACHES && sticky.containsKey(key)) send(JSONObject().put("op", "$key-detach"))
            if (op == null) sticky.remove(key) else sticky[key] = op
        }
        op?.let(::send)
    }

    fun close() {
        val e: LinkEngine?
        val c: LinkEngine.Client?
        synchronized(lock) {
            if (closed) return
            closed = true
            e = engine
            c = client
            engine = null
            client = null
            try { remote?.close() } catch (_: IOException) {}
            remote = null
            remoteOut = null
            lock.notifyAll()
        }
        if (e != null && c != null) {
            e.disconnect(c)
            Links.dropLocal(this, e)
        }
    }

    internal fun engineGone(e: LinkEngine) {
        synchronized(lock) {
            if (engine !== e) return
            engine = null
            client = null
            lost = true
            if (!preferHost) holdUntil = System.currentTimeMillis() + 4000
            lock.notifyAll()
        }
    }

    private fun manage() {
        while (true) {
            synchronized(lock) {
                while (!closed && (engine != null || remote != null)) lock.wait()
                if (closed) return
            }
            if (lost) { lost = false; onEvent(JSONObject().put("type", "lost")) }
            if (!attach()) Thread.sleep(300)
        }
    }

    // One try: the engine this process hosts, else the other process's, else ours.
    private fun attach(): Boolean {
        if (Links.hosted() != null) return hostHere()
        val s = LocalSocket()
        val reached = try {
            s.connect(LocalSocketAddress(Links.socketName(context), LocalSocketAddress.Namespace.ABSTRACT))
            true
        } catch (e: IOException) {
            try { s.close() } catch (_: IOException) {}
            false
        }
        val ours = reached && try { s.peerCredentials.pid == android.os.Process.myPid() } catch (e: IOException) { false }
        val step = LinkAttach.choose(
            hostedHere = Links.hosted() != null, reached = reached, reachedOurs = ours, preferHost = preferHost,
            holding = System.currentTimeMillis() < holdUntil,
        )
        if (step != LinkAttach.Step.JOIN_REMOTE && step != LinkAttach.Step.YIELD_THEN_HOST && reached) {
            try { s.close() } catch (_: IOException) {}
        }
        return when (step) {
            LinkAttach.Step.HOST_HERE -> hostHere()
            LinkAttach.Step.JOIN_REMOTE -> remote(s)
            LinkAttach.Step.YIELD_THEN_HOST -> {
                // The home process hosts: ask it to yield, then wait for it to close.
                try {
                    s.outputStream.write("{\"op\":\"yield\"}\n".toByteArray())
                    s.soTimeout = 3000
                    while (s.inputStream.read(ByteArray(4096)) >= 0) {}
                } catch (_: IOException) {}
                try { s.close() } catch (_: IOException) {}
                hostHere()
            }
            LinkAttach.Step.WAIT -> false
        }
    }

    private fun hostHere(): Boolean {
        var e: LinkEngine? = null
        for (i in 0 until 20) {
            e = Links.host(context)
            if (e != null || !preferHost) break
            Thread.sleep(150)
        }
        if (e == null) return false
        val c = object : LinkEngine.Client {
            override fun send(event: JSONObject) = onEvent(event)
        }
        val ops: List<JSONObject>
        synchronized(lock) {
            if (closed) return true
            engine = e
            client = c
            ops = sticky.values.toList()
        }
        Links.addLocal(this)
        e.connect(c)
        for (op in ops) e.handle(c, op)
        return true
    }

    private companion object {
        val ATTACHES = setOf("ir", "serial", "bt")
    }

    private fun remote(s: LocalSocket): Boolean {
        val ops: List<JSONObject>
        synchronized(lock) {
            if (closed) { try { s.close() } catch (_: IOException) {}; return true }
            remote = s
            remoteOut = s.outputStream
            ops = sticky.values.toList()
        }
        for (op in ops) send(op)
        Thread({
            try {
                val reader = s.inputStream.bufferedReader()
                while (true) {
                    val line = reader.readLine() ?: break
                    val event = try { JSONObject(line) } catch (_: Exception) { continue }
                    onEvent(event)
                }
            } catch (_: IOException) {}
            synchronized(lock) {
                if (remote === s) {
                    remote = null
                    remoteOut = null
                    lost = true
                    lock.notifyAll()
                }
            }
            try { s.close() } catch (_: IOException) {}
        }, "FrondLinkRemote").apply { isDaemon = true; start() }
        return true
    }
}

// Where a LinkConnection gets its engine on one try. Every client in a process shares the engine that
// process hosts; only an engine in the other process is joined over the socket, or asked to yield by the
// emulator. Yielding our own engine would drop every other link kind running here.
object LinkAttach {
    enum class Step { HOST_HERE, JOIN_REMOTE, YIELD_THEN_HOST, WAIT }

    // hostedHere: this process runs the engine. reached: the abstract socket answered; reachedOurs: and it's
    // this process's server, an engine stopping here. holding: the home process just yielded to the emulator.
    fun choose(hostedHere: Boolean, reached: Boolean, reachedOurs: Boolean, preferHost: Boolean, holding: Boolean): Step = when {
        hostedHere -> Step.HOST_HERE
        // Ours, stopping: wait for it to go, then start again.
        reached && reachedOurs -> Step.WAIT
        reached && preferHost -> Step.YIELD_THEN_HOST
        reached -> Step.JOIN_REMOTE
        !preferHost && holding -> Step.WAIT
        else -> Step.HOST_HERE
    }
}

// The machine's infrared choice in its settings: off, or link:<id> for a paired connection. Android has no
// This computer, as it runs one machine at a time.
object InfraredChoice {
    const val OFF = "off"

    const val MODEM = "modem"

    const val PHONE = "phone"

    fun linkId(value: String): String? = value.removePrefix("link:").takeIf { value.startsWith("link:") && it.isNotEmpty() && it != "local" }

    // What a saved value means here: anything that isn't a link is off.
    fun normalise(value: String?): String = if (value != null && linkId(value) != null) value else OFF

    // The infrared port's saved value: the same, or phone.
    fun normaliseInfrared(value: String?): String = if (value == PHONE) PHONE else normalise(value)

    // The serial port's saved value: the same, or modem.
    fun normaliseSerial(value: String?): String = if (value == MODEM) MODEM else normalise(value)

    // The picker's rows as (value, label): Off, then the paired connections by name, then the current
    // value as Unknown connection if it names one that's gone.
    fun choices(current: String, paired: List<Pair<String, String>>): List<Pair<String, String>> = buildList {
        add(OFF to "Off")
        for ((id, name) in paired) add("link:$id" to name)
        if (linkId(current) != null && none { it.first == current }) add(current to "Unknown connection")
    }

    // The serial port's rows: the same, with Modem after Off on a device that has one.
    fun serialChoices(current: String, paired: List<Pair<String, String>>, modem: Boolean): List<Pair<String, String>> =
        choices(current, paired).toMutableList().also { if (modem) it.add(1, MODEM to "Modem") }

    // The infrared rows: the same, with Mobile phone after Off on a device that has an infrared phone.
    fun infraredChoices(current: String, paired: List<Pair<String, String>>, phone: Boolean): List<Pair<String, String>> =
        choices(current, paired).toMutableList().also { if (phone) it.add(1, PHONE to "Mobile phone") }

    // A link to a connection that's gone acts as Off.
    fun active(value: String, paired: List<Pair<String, String>>): String? = linkId(value)?.takeIf { id -> paired.any { it.first == id } }

    fun folder(context: Context): File = LinkStore.folder(context.filesDir)
}

// The machine's Bluetooth choice in its settings: off, radio (This Frond's radio) or link:<id> for a paired
// connection, as on the desktop without its Controller socket and This computer.
object BluetoothChoice {
    const val OFF = "off"
    const val RADIO = "radio"

    fun normalise(value: String?): String = when {
        value == RADIO -> RADIO
        value != null && InfraredChoice.linkId(value) != null -> value
        else -> OFF
    }

    // Off, This Frond, the paired connections by name, then the current value as Unknown connection if it names
    // one that's gone.
    fun choices(current: String, paired: List<Pair<String, String>>): List<Pair<String, String>> = buildList {
        add(OFF to "Off")
        add(RADIO to "This Frond")
        for ((id, name) in paired) add("link:$id" to name)
        if (InfraredChoice.linkId(current) != null && none { it.first == current }) add(current to "Unknown connection")
    }

    // The engine's bt-attach link: radio or the connection's id. Null for off and for a connection that's gone.
    fun active(value: String, paired: List<Pair<String, String>>): String? =
        if (value == RADIO) RADIO else InfraredChoice.active(value, paired)
}
