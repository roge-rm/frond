package com.rm.frond

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.system.Os
import android.system.OsConstants
import android.util.Log
import com.rm.frond.LinkWire.EventKind
import com.rm.frond.LinkWire.LinkError
import com.rm.frond.LinkWire.LinkState
import com.rm.frond.LinkWire.PairResult
import com.rm.frond.LinkWire.PeerState
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.DatagramChannel
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.nio.channels.ServerSocketChannel
import java.nio.channels.SocketChannel
import java.io.ByteArrayOutputStream
import java.util.ArrayDeque
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.Semaphore

// The Android side of Connections, the same job as the desktop's frond --link-service (desktop/LinkHub.cpp)
// and the same wire: the store, discovery, the TCP listener, links to paired installs and the running
// machine's infrared, serial and Bluetooth sockets. Clients (the Connections screen, the running machine) send it the hub's JSON
// ops and get its events. One thread runs everything: sockets on a Selector, the native link objects,
// and every client op, posted from other threads.
class LinkEngine(private val store: LinkStore, private val host: Host) {
    interface Host {
        // UDP port for beacons; 0 turns discovery off.
        val discoveryPort: Int
        // Called on the engine thread when discovery starts and stops, for the Wi-Fi multicast lock.
        fun discovery(on: Boolean) {}
        // False while Android's local network access is off for Frond.
        fun networkAllowed(): Boolean = true
        // The engine stopped by itself (yield, or a failure); called on its thread.
        fun stopped() {}
        // This Frond's radio (BtLink): start returns the socket its controllers connect to, or null when
        // there's none. Called on the engine thread.
        fun radioAvailable(): Boolean = false
        fun startRadio(): String? = null
        fun radioRunning(): Boolean = false
        fun stopRadio() {}
    }

    // Gets events on the engine thread. Must not block for long.
    interface Client {
        fun send(event: JSONObject)
    }

    private class ClientState(val client: Client) {
        var watch = false
    }

    private enum class Kind { UNKNOWN, LINK, PAIR_IN, PAIR_OUT }

    private inner class Conn(val channel: SocketChannel, val outgoing: Boolean) {
        var kind = Kind.UNKNOWN
        var pre = ByteArray(0)
        var link = 0L
        var pairing = 0L
        var peer = ""
        val created = now()
        var closingAt = 0L
        var closing = false
        var up = false
        var connected = !outgoing
        var gotAnswer = false
        val out = ArrayDeque<ByteBuffer>()
        var key: SelectionKey? = null
        val toWrite get() = out.sumOf { it.remaining() }
        var remote: InetSocketAddress? = null
    }

    private class Held(val ch: Int, val kind: Int, val machine: String, val device: String)

    private class PeerRun {
        var link: Conn? = null
        var dialing: Conn? = null
        var backoff = 0
        var nextTry = 0L
        var target = 0
        var failed = false
        var forgotten = false
        // Either side's failed handshake against the pinned key; shown, never stored (LinkHub.h).
        var keyMismatch = false
        var seen: InetAddress? = null
        var seenPort = 0
        var seenAt = 0L
        var beaconDialAt = -100000L
        var idleSince = 0L
        val held = ArrayList<Held>()
    }

    private class Nearby(val name: String, val address: InetAddress, val port: Int, val at: Long)

    private enum class St { IDLE, OPENING, WAITING, JOINED, BUSY }

    private inner class Attach(
        val client: ClientState, val machine: String, val device: String, val link: String, val socketPath: String,
        val kind: Int = LinkWire.KIND_INFRARED,
    ) {
        val serial get() = kind == LinkWire.KIND_SERIAL
        val bt get() = kind == LinkWire.KIND_BLUETOOTH
        // Bluetooth: the engine's socket the core connects to, once per controller.
        var btServer: LocalServerSocket? = null
        var btBound: LocalSocket? = null
        // Serial: the reader takes one buffer from the core at a time and waits on the gate until the mux has
        // taken it all (outHeld is what didn't fit yet), so the core's queue fills and its CTS drops. Bytes
        // from the link go to the writer thread; credit goes back as it writes them (unacked). pendingIn holds
        // what came before the core socket was up.
        var gate: Semaphore? = null
        var outHeld: ByteArray? = null
        var outAt = 0
        var writer: LinkedBlockingQueue<ByteArray>? = null
        var unacked = 0
        val pendingIn = ByteArrayOutputStream()
        var core: LocalSocket? = null
        var coreGeneration = 0
        var coreUp = false
        var nextCoreTry = 0L
        var st = St.IDLE
        var ch = 0
        var chLink: Conn? = null
        var farMachine = ""
        var farDevice = ""
        var busyBy = ""
        var farState = PeerState.RUNNING
        var retryAt = 0L
        var paused = false
        var lastStatus: JSONObject? = null
    }

    // One Bluetooth controller: a core's connection to the engine's socket (ours), or a peer's machine on our
    // radio. It goes to this Frond's radio, or over a Bluetooth channel to the side whose radio it is.
    private inner class Controller {
        var owner: Attach? = null           // the machine here it belongs to; null for a peer's
        var peer = ""                       // a peer's: the connection it came over, and its machine
        var machine = ""
        var core: LocalSocket? = null       // ours: the core's connection
        var radioSock: LocalSocket? = null  // our radio, for ours on this Frond's radio and for a peer's
        var radioUp = false
        var wantsRadio = false
        var radioTry = 0L
        var chLink: Conn? = null            // the Bluetooth channel, when the radio is on the other side
        var ch = 0
        var open = false                    // the channel was accepted
        // The near socket (the core's, or the radio's for a peer's) as serial does it: the reader takes one
        // buffer at a time and waits on the gate until the mux has it all; the writer gives it the link's
        // bytes and credit goes back as it does (unacked). pending holds what came before the near end was up.
        var gate: Semaphore? = null
        var outHeld: ByteArray? = null
        var outAt = 0
        var writer: LinkedBlockingQueue<ByteArray>? = null
        var unacked = 0
        val pending = ByteArrayOutputStream()
        var nearGeneration = 0
    }

    private val clock0 = System.nanoTime()
    private fun now() = (System.nanoTime() - clock0) / 1_000_000 + 1_000_000
    private fun wall() = System.currentTimeMillis()

    private val tasks = ConcurrentLinkedQueue<() -> Unit>()
    private var selector: Selector? = null
    private var listener: ServerSocketChannel? = null
    private var udp: DatagramChannel? = null
    private var thread: Thread? = null
    @Volatile private var running = false
    val port get() = listenPort
    private var listenPort = 0

    private val clients = ArrayList<ClientState>()
    private val conns = ArrayList<Conn>()
    private val attaches = ArrayList<Attach>()
    private val controllers = ArrayList<Controller>()
    // This Frond's radio, while the engine runs it.
    private var radioOn = false
    private var radioPath = ""
    private var radioStarted = 0L
    private var radioIdleSince = 0L
    private var radioFailedAt = 0L
    private val runs = HashMap<String, PeerRun>()
    private val nearby = HashMap<String, Nearby>()
    private val recent = HashMap<String, ArrayList<Long>>()
    private var lastBeacon = -100000L
    private var wasActive = false
    private var lastSnapshot: JSONObject? = null
    private var lastTick = 0L

    // B: the code shown while someone pairs with us.
    private var code = 0L
    private var requester = ByteArray(0)
    private var requesterName = ""
    private var pairCooldown = 0L
    private var codeAt = 0L

    // A: the pairing this engine runs for one client.
    private class PairOut {
        var client: ClientState? = null
        var host = ""
        var port = 0
        var typedAddress = ""
        var code = ""
        var conn: Any? = null
        var peerName = ""
        var peerId = ByteArray(0)
    }
    private var pairOut = PairOut()
    private val pairConn get() = pairOut.conn as Conn?

    // ---- Life ----

    // Opens the sockets and starts the thread. Returns an error, or null. The store must be open.
    fun start(): String? {
        val sel = Selector.open()
        selector = sel
        val server = ServerSocketChannel.open()
        try {
            server.setOption(StandardSocketOptions.SO_REUSEADDR, true)
            try { server.bind(InetSocketAddress(store.identity.port), 16) } catch (e: IOException) { server.bind(InetSocketAddress(0), 16) }
        } catch (e: IOException) {
            server.close()
            sel.close()
            return "Couldn't take connections from the network."
        }
        server.configureBlocking(false)
        server.register(sel, SelectionKey.OP_ACCEPT)
        listener = server
        listenPort = server.socket().localPort
        if (host.discoveryPort != 0) {
            try {
                val d = DatagramChannel.open()
                d.setOption(StandardSocketOptions.SO_REUSEADDR, true)
                d.setOption(StandardSocketOptions.SO_BROADCAST, true)
                d.bind(InetSocketAddress(host.discoveryPort))
                d.configureBlocking(false)
                d.register(sel, SelectionKey.OP_READ)
                udp = d
            } catch (e: IOException) {
                Log.w(TAG, "link: no discovery: $e")
            }
        }
        running = true
        thread = Thread(::loop, "FrondLink").apply { isDaemon = true; start() }
        Log.i(TAG, "link: engine on port $listenPort as ${store.identity.name}")
        return null
    }

    // Closes every link with a close record. From any thread; returns once the thread has ended.
    fun stop() {
        val t = thread ?: return
        post { shutdown() }
        if (Thread.currentThread() !== t) t.join(2000)
    }

    fun post(task: () -> Unit) {
        tasks.add(task)
        selector?.wakeup()
    }

    fun connect(client: Client) = post {
        val c = ClientState(client)
        clients += c
        client.send(JSONObject().put("type", "hello").put("id", store.identity.hex).put("name", store.identity.name).put("port", listenPort))
    }

    fun disconnect(client: Client) = post { clients.firstOrNull { it.client === client }?.let(::dropClient) }

    fun handle(client: Client, op: JSONObject) = post { clients.firstOrNull { it.client === client }?.let { handleOp(it, op) } }

    private fun shutdown() {
        if (!running) return
        running = false
        for (c in ArrayList(conns)) {
            if (c.link != 0L && c.connected) {
                LinkNative.linkClose(c.link, LinkWire.CLOSE_NORMAL)
                LinkNative.linkTakeOutput(c.link, 0)?.let { try { c.channel.write(ByteBuffer.wrap(it)) } catch (_: IOException) {} }
            }
            removeConn(c)
        }
        for (k in ArrayList(controllers)) dropController(k, false)
        for (a in attaches) { closeCore(a); closeBtServer(a) }
        attaches.clear()
        if (radioOn) { host.stopRadio(); radioOn = false }
        endPairRequest("cancelled")
        try { listener?.close() } catch (_: IOException) {}
        try { udp?.close() } catch (_: IOException) {}
        try { selector?.close() } catch (_: IOException) {}
        if (wasActive) host.discovery(false)
        wasActive = false
        store.close()
        Log.i(TAG, "link: engine stopped")
    }

    private fun loop() {
        try {
            while (running) {
                val sel = selector ?: break
                val wait = (100 - (now() - lastTick)).coerceIn(1, 100)
                sel.select(wait)
                if (!running) break
                for (key in sel.selectedKeys().toList()) {
                    sel.selectedKeys().remove(key)
                    if (!key.isValid) continue
                    when (val what = key.attachment()) {
                        is Conn -> connReady(what, key)
                        else -> if (key.channel() === listener) incoming() else if (key.channel() === udp) datagrams()
                    }
                }
                while (true) { val task = tasks.poll() ?: break; task(); if (!running) break }
                if (running && now() - lastTick >= 100) { lastTick = now(); tick() }
            }
        } catch (e: Exception) {
            Log.e(TAG, "link: engine failed", e)
        }
        shutdown()
        host.stopped()
    }

    // ---- Clients ----

    private fun sendTo(c: ClientState?, o: JSONObject) {
        if (c != null && c in clients) c.client.send(o)
    }

    private fun broadcastWatchers(o: JSONObject) {
        for (c in clients) if (c.watch) c.client.send(o)
    }

    private fun watchers() = clients.count { it.watch }

    private fun dropClient(c: ClientState) {
        if (!clients.remove(c)) return
        for (a in ArrayList(attaches)) if (a.client === c) removeAttach(a)
        if (pairOut.client === c) {
            pairConn?.let { closeConn(it, false) }
            pairOut = PairOut()
        }
        if (c.watch && watchers() == 0 && code != 0L) endPairRequest("cancelled")
    }

    private fun handleOp(c: ClientState, op: JSONObject) {
        when (op.optString("op")) {
            "watch" -> {
                c.watch = op.optBoolean("on", true)
                if (c.watch) sendTo(c, snapshot()) else if (watchers() == 0 && code != 0L) endPairRequest("cancelled")
            }
            "state" -> sendTo(c, snapshot())
            "rename" -> {
                val name = op.optString("name").trim().take(64)
                if (name.isNotEmpty()) store.setName(name)
            }
            "peer-rename", "peer-address", "forget" -> {
                val hex = op.optString("id")
                store.find(hex) ?: return
                when (op.optString("op")) {
                    "peer-rename" -> {
                        val name = op.optString("name").trim().take(64)
                        if (name.isNotEmpty()) store.update(hex) { it.copy(name = name) }
                    }
                    "peer-address" -> {
                        store.update(hex) { it.copy(address = op.optString("address").trim()) }
                        val r = run(hex)
                        r.nextTry = now(); r.backoff = 0; r.failed = false
                    }
                    else -> {
                        val r = run(hex)
                        r.link?.let { closeConn(it) }
                        r.dialing?.let { closeConn(it) }
                        runs.remove(hex)
                        store.remove(hex)
                    }
                }
                store.save()
            }
            "pair" -> {
                pairConn?.let { closeConn(it, false) }
                pairOut = PairOut()
                pairOut.client = c
                if (op.has("id")) {
                    val n = nearby[op.optString("id")]
                    if (n == null) {
                        sendTo(c, JSONObject().put("type", "pair-result").put("result", "unreachable"))
                        pairOut = PairOut()
                        return
                    }
                    pairOut.host = n.address.hostAddress ?: ""
                    pairOut.port = n.port
                    pairOut.peerName = n.name
                } else {
                    val typed = op.optString("address").trim()
                    pairOut.typedAddress = typed
                    val target = LinkWire.hostPort(typed)
                    if (target == null) {
                        sendTo(c, JSONObject().put("type", "pair-result").put("result", "unreachable").put("name", typed))
                        pairOut = PairOut()
                        return
                    }
                    pairOut.host = target.first
                    pairOut.port = target.second
                    pairOut.peerName = typed
                }
                pairOutStart()
            }
            "pair-code" -> {
                if (pairOut.client !== c) return
                pairOut.code = op.optString("code")
                val conn = pairConn
                if (conn != null && conn.gotAnswer && conn.pairing == 0L) {
                    conn.pairing = newPairing(pairOut.code)
                    pump(conn)
                    pairDone(conn)
                } else if (conn == null) pairOutStart()
            }
            "pair-cancel" -> {
                if (pairOut.client !== c) return
                pairConn?.let { closeConn(it, false) }
                pairOut = PairOut()
            }
            "pair-dismiss" -> if (code != 0L) endPairRequest("cancelled")
            "peer-radio" -> {
                val p = store.find(op.optString("id")) ?: return
                val here = op.optString("radio") != "there"
                val at = maxOf(wall(), p.bluetoothRadioAt + 1)
                setRadio(p.id, here, at)
                val link = runs[p.id]?.link
                if (link != null && link.up && link.link != 0L) { LinkNative.linkSendRadio(link.link, here, at); pump(link) }
            }
            "bt-attach" -> btAttach(c, op)
            "bt-detach" -> for (a in ArrayList(attaches)) if (a.client === c && a.bt) removeAttach(a)
            "ir-attach", "serial-attach" -> {
                val kind = if (op.optString("op") == "ir-attach") LinkWire.KIND_INFRARED else LinkWire.KIND_SERIAL
                if (attaches.any { it.client === c && it.kind == kind }) return
                val a = Attach(c, op.optString("machine"), op.optString("device"), op.optString("link"), op.optString("socket"), kind)
                a.paused = op.optBoolean("paused", false)
                attaches += a
                // A serial port opens only once it's joined, so the Palm's DSR and DCD mean the far port is open.
                if (!a.serial) coreConnect(a)
                val r = run(a.link)
                if (r.link == null && r.dialing == null) { r.nextTry = now(); r.backoff = 0 }
                matchPeer(a.link)
                syncCores()
            }
            "ir-detach", "serial-detach" -> {
                val kind = if (op.optString("op") == "ir-detach") LinkWire.KIND_INFRARED else LinkWire.KIND_SERIAL
                for (a in ArrayList(attaches)) if (a.client === c && a.kind == kind) removeAttach(a)
            }
            // The app went to the background (paused) or came back.
            "ir-state", "serial-state" -> for (a in attaches) {
                if (a.client !== c) continue
                a.paused = op.optString("state") == "paused"
                coreState(a, ownState(a))
            }
            "yield" -> shutdown()
        }
    }

    private fun run(hex: String) = runs.getOrPut(hex) { PeerRun() }

    // ---- TCP ----

    private fun register(c: Conn, ops: Int) {
        val sel = selector ?: return
        try {
            c.key = c.channel.register(sel, ops, c)
        } catch (e: IOException) {
            removeConn(c)
        }
    }

    private fun interest(c: Conn) {
        val key = c.key ?: return
        if (!key.isValid) return
        key.interestOps(if (!c.connected) SelectionKey.OP_CONNECT else SelectionKey.OP_READ or (if (c.out.isEmpty()) 0 else SelectionKey.OP_WRITE))
    }

    private fun setOptions(ch: SocketChannel) {
        try {
            ch.setOption(StandardSocketOptions.TCP_NODELAY, true)
            ch.setOption(StandardSocketOptions.SO_SNDBUF, SEND_BUFFER)
        } catch (_: IOException) {}
    }

    private fun incoming() {
        while (true) {
            val s = try { listener?.accept() } catch (e: IOException) { null } ?: return
            val from = (s.remoteAddress as? InetSocketAddress)?.address
            if (!LinkWire.allowedPeer(from)) { s.close(); continue }
            val t = now()
            val times = recent.getOrPut(from!!.hostAddress ?: "") { ArrayList() }
            times.removeAll { t - it > 10000 }
            val pending = conns.count { !it.outgoing && !it.up && it.kind != Kind.PAIR_IN }
            if (times.size >= PER_ADDRESS || pending >= MAX_PENDING) { s.close(); continue }
            times += t
            s.configureBlocking(false)
            setOptions(s)
            val c = Conn(s, false)
            c.remote = s.remoteAddress as? InetSocketAddress
            conns += c
            register(c, SelectionKey.OP_READ)
        }
    }

    private val readBuffer = ByteBuffer.allocate(65536)

    private fun connReady(c: Conn, key: SelectionKey) {
        if (c !in conns) return
        try {
            if (key.isConnectable) {
                if (!c.channel.finishConnect()) return
                c.connected = true
                c.remote = c.channel.remoteAddress as? InetSocketAddress
                setOptions(c.channel)
                if (c.kind == Kind.PAIR_OUT) queue(c, LinkWire.pairRequest(store.identity.id, store.identity.name))
                pump(c)
                interest(c)
                return
            }
            if (key.isWritable) {
                flush(c)
                if (c !in conns) return
                pump(c)
            }
            if (key.isReadable) {
                readBuffer.clear()
                val n = c.channel.read(readBuffer)
                if (n < 0) { removeConn(c); return }
                if (n > 0) connInput(c, readBuffer.array().copyOf(n))
            }
        } catch (e: IOException) {
            removeConn(c)
        }
    }

    private fun queue(c: Conn, bytes: ByteArray) {
        if (bytes.isEmpty()) return
        c.out.add(ByteBuffer.wrap(bytes))
        flush(c)
    }

    private fun flush(c: Conn) {
        if (!c.connected || c !in conns) return
        try {
            while (c.out.isNotEmpty()) {
                val b = c.out.peek()!!
                c.channel.write(b)
                if (b.hasRemaining()) break
                c.out.poll()
            }
        } catch (e: IOException) {
            removeConn(c)
            return
        }
        if (c.closing && c.out.isEmpty() && !(c.link != 0L && LinkNative.linkHasOutput(c.link))) { removeConn(c); return }
        interest(c)
    }

    private fun connInput(c: Conn, data: ByteArray) {
        if (c.closing) return
        when (c.kind) {
            Kind.UNKNOWN -> {
                c.pre += data
                when (LinkWire.FirstFrame.entries[LinkNative.peekFirstFrame(c.pre)]) {
                    LinkWire.FirstFrame.NEED_MORE -> if (c.pre.size > 4096) closeConn(c, false)
                    LinkWire.FirstFrame.NOISE -> {
                        c.kind = Kind.LINK
                        c.link = LinkNative.linkAnswer(store.identity.secret, store.identity.id, store.identity.name, store.pairedTable(), now())
                        val pre = c.pre
                        c.pre = ByteArray(0)
                        LinkNative.linkReceive(c.link, pre, pre.size, now())
                        afterLink(c)
                    }
                    LinkWire.FirstFrame.PAIR -> {
                        val (first, used) = LinkWire.frameAt(c.pre, c.pre.size) ?: return
                        val rest = c.pre.copyOfRange(used, c.pre.size)
                        c.pre = ByteArray(0)
                        pairFirst(c, first, rest)
                    }
                    LinkWire.FirstFrame.BAD_VERSION -> {
                        queue(c, LinkNative.refuseFrame(LinkWire.REFUSE_VERSION))
                        closeConn(c)
                    }
                    LinkWire.FirstFrame.GARBAGE -> closeConn(c, false)
                }
            }
            Kind.LINK -> {
                LinkNative.linkReceive(c.link, data, data.size, now())
                afterLink(c)
            }
            Kind.PAIR_IN, Kind.PAIR_OUT -> pairInput(c, data)
        }
    }

    private fun linkState(c: Conn) = LinkState.entries[LinkNative.linkState(c.link)]

    private fun afterLink(c: Conn) {
        if (c !in conns || c.link == 0L) return
        val state = linkState(c)
        if (c.peer.isEmpty() && state != LinkState.HANDSHAKE) c.peer = LinkWire.hex(LinkNative.linkPeerId(c.link))
        if (state == LinkState.UP && !c.up) {
            c.up = true
            linkUp(c)
            if (c !in conns) return
        }
        if (c.up) muxEvents(c)
        if (c !in conns) return
        if (c.up) btRadioChoices(c)
        pump(c)
        if (c !in conns) return
        if (linkState(c) == LinkState.DOWN && !c.closing) {
            val hex = c.peer.ifEmpty { LinkWire.hex(LinkNative.linkPeerId(c.link)) }
            val p = store.find(hex)
            val error = LinkError.entries[LinkNative.linkError(c.link)]
            if (error == LinkError.KEY_CHANGED && p != null) {
                val r = run(hex)
                if (!(r.link != null && r.link !== c && r.link!!.up)) r.keyMismatch = true
            }
            if (error == LinkError.UNKNOWN_PEER && p != null && c.outgoing) run(hex).forgotten = true
            closeConn(c)
        }
    }

    private fun linkUp(c: Conn) {
        val p = store.find(c.peer)
        if (p == null) { closeConn(c); return }
        val r = run(c.peer)
        val old = r.link
        if (old != null && old !== c) {
            // One link per pair of installs: keep the one dialled by the lower id; from the same side, the newer.
            val initNew = if (c.outgoing) store.identity.hex else p.id
            val initOld = if (old.outgoing) store.identity.hex else p.id
            if (!(initNew == initOld || initNew < initOld)) { closeConn(c); return }
            r.link = null
            linkDown(old)
            closeConn(old)
        }
        if (r.dialing === c) r.dialing = null
        r.link = c
        r.backoff = 0; r.failed = false; r.forgotten = false; r.keyMismatch = false; r.idleSince = now()
        val peerName = LinkNative.linkPeerName(c.link)
        store.update(c.peer) {
            var q = it.copy(lastSeen = wall(), keyChanged = false, peerName = peerName)
            val remote = c.remote
            if (c.outgoing && remote != null) q = q.copy(lastHost = remote.address.hostAddress ?: "", lastPort = remote.port)
            q
        }
        store.save()
        matchPeer(c.peer)
        btLinkUp(c)
    }

    // The link went: its channels are gone, the machines on it wait again.
    private fun linkDown(c: Conn) {
        for (a in attaches) if (a.chLink === c) clearChannel(a)
        btLinkDown(c)
        if (c.peer.isNotEmpty()) runs[c.peer]?.let { r ->
            if (r.link === c) { r.link = null; r.held.clear(); r.nextTry = now() + LinkWire.backoff(0) }
            if (r.dialing === c) r.dialing = null
        }
        // Serial bytes in flight are lost and the core's serial port closes (DSR and DCD drop).
        syncCores()
    }

    private fun pump(c: Conn) {
        if (c !in conns || !c.connected) return
        // Then serial bytes the core held while the channel had no room, and what they make.
        for (round in 0..1) {
            while (c.toWrite < WRITE_AHEAD) {
                val out = when {
                    c.link != 0L && LinkNative.linkHasOutput(c.link) -> LinkNative.linkTakeOutput(c.link, WRITE_AHEAD)
                    c.pairing != 0L -> LinkNative.pairTakeOutput(c.pairing)
                    else -> null
                }
                if (out == null || out.isEmpty()) break
                c.out.add(ByteBuffer.wrap(out))
                flush(c)
                if (c !in conns) return
            }
            if (round == 1 || c.closing) break
            val serialMoved = pullSerial(c)
            if (!(pullBluetooth(c) || serialMoved)) break
        }
        flush(c)
    }

    private fun closeConn(c: Conn, flush: Boolean = true) {
        if (c !in conns) return
        if (c.up) { c.up = false; linkDown(c); if (c.peer.isNotEmpty()) matchPeer(c.peer) }
        if (c.link != 0L && linkState(c) != LinkState.DOWN) LinkNative.linkClose(c.link, LinkWire.CLOSE_NORMAL)
        if (pairConn === c) pairOut.conn = null
        if (!c.closing) c.closingAt = now()
        c.closing = true
        if (flush && c.connected) {
            pump(c)
            // A peer that never reads gets dropped by the tick after CLOSING_MS.
        } else {
            removeConn(c)
        }
    }

    private fun removeConn(c: Conn) {
        if (!conns.remove(c)) return
        if (c.up) { c.up = false; linkDown(c) }
        if (c.peer.isNotEmpty()) runs[c.peer]?.let { r ->
            if (r.dialing === c) { r.dialing = null; dialFailed(c) }
            if (r.link === c) r.link = null
            matchPeer(c.peer)
        }
        if (pairConn === c) {
            pairOut.conn = null
            if (c.kind == Kind.PAIR_OUT && !c.gotAnswer && c.pairing == 0L)
                sendTo(pairOut.client, JSONObject().put("type", "pair-result").put("result", "unreachable").put("name", pairOut.peerName))
        }
        try { c.key?.cancel() } catch (_: Exception) {}
        try { c.channel.close() } catch (_: IOException) {}
        if (c.link != 0L) { LinkNative.linkFree(c.link); c.link = 0L }
        if (c.pairing != 0L) { LinkNative.pairFree(c.pairing); c.pairing = 0L }
    }

    // Where to dial a peer, tried in turn: the last beacon's address, the last address a handshake we
    // dialled succeeded at and the typed address, each once.
    private fun dialTargets(hex: String): List<Pair<String, Int>> {
        val p = store.find(hex) ?: return emptyList()
        val targets = ArrayList<Pair<String, Int>>()
        fun add(host: String, port: Int) {
            if (host.isNotEmpty() && port != 0 && (host to port) !in targets) targets += host to port
        }
        runs[hex]?.let { r -> r.seen?.let { add(it.hostAddress ?: "", r.seenPort) } }
        add(p.lastHost, p.lastPort)
        if (p.address.isNotEmpty()) LinkWire.hostPort(p.address)?.let { add(it.first, it.second) }
        return targets
    }

    private fun dial(hex: String) {
        val p = store.find(hex) ?: return
        val r = run(hex)
        val targets = dialTargets(hex)
        if (targets.isEmpty()) { r.failed = true; r.nextTry = now() + LinkWire.backoff(r.backoff++); return }
        val target = targets[r.target % targets.size]
        val key = LinkWire.unhex(p.id, 16) ?: return
        val s = SocketChannel.open()
        s.configureBlocking(false)
        val c = Conn(s, true)
        c.kind = Kind.LINK
        c.peer = hex
        conns += c
        r.dialing = c
        c.link = LinkNative.linkDial(store.identity.secret, store.identity.id, store.identity.name, key, p.key, now())
        try {
            setOptions(s)
            val address = InetAddress.getByName(target.first)
            if (!LinkWire.allowedPeer(address)) throw IOException("not a local address")
            if (s.connect(InetSocketAddress(address, target.second))) {
                c.connected = true
                c.remote = s.remoteAddress as? InetSocketAddress
                register(c, SelectionKey.OP_READ)
                pump(c)
            } else register(c, SelectionKey.OP_CONNECT)
        } catch (e: Exception) {
            removeConn(c)
        }
    }

    // Tries the next address at once, and backs off after a round of them.
    private fun dialFailed(c: Conn) {
        val r = run(c.peer)
        val count = dialTargets(c.peer).size
        ++r.target
        if (count > 1 && r.target % count != 0) { r.nextTry = now(); return }
        r.failed = true
        r.nextTry = now() + LinkWire.backoff(r.backoff++)
    }

    // ---- Pairing ----
    // Before CPace, A sends a request frame (header, 0, idA, name) and B answers with its own id and
    // name, or a refuse frame if its Connections screen isn't open. B shows the code then.

    private fun newPairing(typed: String) =
        LinkNative.pairDial(store.identity.secret, store.identity.id, store.identity.name, typed, now())

    private fun pairFirst(c: Conn, first: ByteArray, rest: ByteArray) {
        val frame = LinkWire.parsePairFrame(first)
        val request = frame as? LinkWire.PairFrame.Request
        val same = code != 0L && request != null && request.id.contentEquals(requester)
        if (request == null || watchers() == 0 || now() < pairCooldown || (code != 0L && !same) ||
            request.id.contentEquals(store.identity.id)) {
            queue(c, LinkNative.refuseFrame(LinkWire.REFUSE_NOT_PAIRING))
            closeConn(c)
            return
        }
        if (code == 0L) {
            code = LinkNative.codeNew(now())
            codeAt = now()
            requester = request.id
            requesterName = request.name.ifEmpty { "Frond" }
            broadcastWatchers(JSONObject().put("type", "pair-request").put("name", requesterName)
                .put("code", LinkWire.showCode(LinkNative.codeText(code))))
        }
        c.kind = Kind.PAIR_IN
        queue(c, LinkWire.pairRequest(store.identity.id, store.identity.name))
        c.pairing = LinkNative.pairAnswer(store.identity.secret, store.identity.id, store.identity.name, code, now())
        if (rest.isNotEmpty()) pairInput(c, rest)
    }

    private fun pairInput(c: Conn, data: ByteArray) {
        if (c.kind == Kind.PAIR_OUT && !c.gotAnswer) {
            c.pre += data
            val (body, used) = LinkWire.frameAt(c.pre, c.pre.size) ?: return
            val rest = c.pre.copyOfRange(used, c.pre.size)
            c.pre = ByteArray(0)
            when (val f = LinkWire.parsePairFrame(body)) {
                is LinkWire.PairFrame.Request -> {
                    pairOut.peerId = f.id
                    if (f.name.isNotEmpty()) pairOut.peerName = f.name
                    c.gotAnswer = true
                    sendTo(pairOut.client, JSONObject().put("type", "pair-peer").put("name", pairOut.peerName))
                    if (pairOut.code.isNotEmpty()) c.pairing = newPairing(pairOut.code)
                    if (rest.isNotEmpty() && c.pairing != 0L) LinkNative.pairReceive(c.pairing, rest, rest.size, now())
                    pump(c)
                    if (c.pairing != 0L) pairDone(c)
                }
                else -> {
                    val why = (f as? LinkWire.PairFrame.Refused)?.reason ?: LinkWire.REFUSE_PROTOCOL
                    sendTo(pairOut.client, JSONObject().put("type", "pair-result").put("name", pairOut.peerName).put("result",
                        when (why) { LinkWire.REFUSE_NOT_PAIRING -> "notpairing"; LinkWire.REFUSE_VERSION -> "version"; else -> "failed" }))
                    c.gotAnswer = true
                    closeConn(c, false)
                }
            }
            return
        }
        if (c.pairing == 0L) return
        LinkNative.pairReceive(c.pairing, data, data.size, now())
        pump(c)
        pairDone(c)
    }

    private fun pairOutStart() {
        val target = try { InetAddress.getByName(pairOut.host) } catch (e: Exception) { null }
        val s = SocketChannel.open()
        s.configureBlocking(false)
        val c = Conn(s, true)
        c.kind = Kind.PAIR_OUT
        conns += c
        pairOut.conn = c
        try {
            if (target == null || !LinkWire.allowedPeer(target)) throw IOException("not a local address")
            setOptions(s)
            if (s.connect(InetSocketAddress(target, pairOut.port))) {
                c.connected = true
                c.remote = s.remoteAddress as? InetSocketAddress
                register(c, SelectionKey.OP_READ)
                queue(c, LinkWire.pairRequest(store.identity.id, store.identity.name))
            } else register(c, SelectionKey.OP_CONNECT)
        } catch (e: Exception) {
            removeConn(c)
        }
    }

    private fun pairDone(c: Conn) {
        if (c.pairing == 0L) return
        val result = PairResult.entries[LinkNative.pairResult(c.pairing)]
        if (result == PairResult.PENDING) return
        var pairedId = ""
        if (result == PairResult.PAIRED) {
            val info = LinkNative.pairPeer(c.pairing)
            val id = info.copyOfRange(0, 16)
            val key = info.copyOfRange(16, 48)
            val name = info.copyOfRange(48, info.size).toString(Charsets.UTF_8)
            val hex = LinkWire.hex(id)
            pairedId = hex
            val old = store.find(hex)
            var p = (old ?: LinkStore.Peer(hex, name, name, key)).copy(key = key, peerName = name, paired = true, keyChanged = false, lastSeen = wall())
            if (p.name.isEmpty()) p = p.copy(name = name)
            if (c.kind == Kind.PAIR_OUT) {
                if (pairOut.typedAddress.isNotEmpty()) p = p.copy(address = pairOut.typedAddress)
                c.remote?.let { p = p.copy(lastHost = it.address.hostAddress ?: "", lastPort = pairOut.port) }
            }
            store.put(p)
            run(hex).apply { forgotten = false; failed = false; keyMismatch = false }
            nearby.remove(hex)
            store.save()
        }
        if (c.kind == Kind.PAIR_OUT) {
            sendTo(pairOut.client, JSONObject().put("type", "pair-result").put("result", LinkWire.pairResultName(result))
                .put("name", pairOut.peerName).put("id", pairedId))
            if (result == PairResult.PAIRED) { pairOut.client = null; pairOut.code = "" }
        } else if (code != 0L) {
            when {
                result == PairResult.PAIRED -> { LinkNative.codeSucceeded(code); endPairRequest("paired") }
                result == PairResult.LOCKED_OUT || LinkNative.codeTriesLeft(code) <= 0 -> endPairRequest("lockedout")
                result == PairResult.EXPIRED -> endPairRequest("expired")
                else -> broadcastWatchers(JSONObject().put("type", "pair-request-update").put("result", LinkWire.pairResultName(result))
                    .put("tries", LinkNative.codeTriesLeft(code)))
            }
        }
        closeConn(c)
    }

    // A code that ran out or was locked out gets the same cooldown, so codes can't be cycled.
    private fun endPairRequest(result: String) {
        if (code == 0L) return
        if (result == "lockedout" || result == "expired") pairCooldown = now() + PAIR_COOLDOWN_MS
        broadcastWatchers(JSONObject().put("type", "pair-request-end").put("result", result).put("name", requesterName))
        for (c in ArrayList(conns))
            if (c.kind == Kind.PAIR_IN && c.pairing != 0L && LinkNative.pairResult(c.pairing) == PairResult.PENDING.ordinal) closeConn(c, false)
        // Pairings still alive keep their own reference to the code.
        LinkNative.codeFree(code)
        code = 0L
        requester = ByteArray(0)
        requesterName = ""
    }

    // ---- Channels ----

    private fun wanted(hex: String) = attaches.any { it.link == hex }

    private fun attachOn(c: Conn, ch: Int) = attaches.firstOrNull { it.chLink === c && it.ch == ch }

    private fun ownState(a: Attach) = when {
        a.paused -> PeerState.PAUSED
        !a.coreUp -> PeerState.ASLEEP
        else -> PeerState.RUNNING
    }

    private fun joined(a: Attach, c: Conn, ch: Int, machine: String, device: String) {
        a.st = St.JOINED; a.ch = ch; a.chLink = c; a.farMachine = machine; a.farDevice = device; a.farState = PeerState.RUNNING
        // A serial port isn't open yet as it joins; only a paused Palm is worth saying.
        val own = if (a.serial && !a.paused) PeerState.RUNNING else ownState(a)
        if (own != PeerState.RUNNING) LinkNative.muxSendState(c.link, ch, own.wire)
    }

    private fun muxEvents(c: Conn) {
        while (c in conns && c.link != 0L) {
            val raw = LinkNative.muxNextEvent(c.link) ?: break
            val e = LinkWire.ChannelEvent.decode(raw) ?: continue
            val r = run(c.peer)
            r.idleSince = now()
            if (e.kind != EventKind.OPEN_REQUEST) controllerOn(c, e.channel)?.let { btChannelEvent(it, e); continue }
            val a = attachOn(c, e.channel)
            when (e.kind) {
                EventKind.OPEN_REQUEST -> when (e.channelKind) {
                    LinkWire.KIND_BLUETOOTH -> btRequest(c, e)
                    LinkWire.KIND_INFRARED, LinkWire.KIND_SERIAL -> channelRequest(c, e)
                    else -> LinkNative.muxClose(c.link, e.channel, LinkWire.CLOSE_GONE)
                }
                EventKind.ACCEPTED -> if (a != null) joined(a, c, a.ch, e.machine, e.device)
                EventKind.WAITING -> if (a != null) a.st = St.WAITING
                EventKind.BUSY -> if (a != null) {
                    a.st = St.BUSY; a.ch = 0; a.chLink = null; a.busyBy = e.machine; a.retryAt = now() + BUSY_RETRY_MS
                }
                EventKind.DATA -> {
                    if (a != null && a.serial && a.st == St.JOINED) serialIn(a, e.data)
                    else {
                        if (a != null && a.st == St.JOINED && a.coreUp) writeCore(a, e.data)
                        LinkNative.muxConsumed(c.link, e.channel, e.data.size)
                    }
                }
                EventKind.CLOSED -> {
                    if (a != null) { clearChannel(a); a.retryAt = now() + 500 }
                    r.held.removeAll { it.ch == e.channel }
                }
                EventKind.STATE -> if (a != null) a.farState = PeerState.of(e.code)
            }
        }
        if (c in conns) matchPeer(c.peer)
        syncCores()
    }

    private fun channelRequest(c: Conn, e: LinkWire.ChannelEvent) {
        val r = run(c.peer)
        var idle: Attach? = null
        var pending: Attach? = null
        var joined: Attach? = null
        for (a in attaches) {
            if (a.link != c.peer || a.kind != e.channelKind) continue
            when (a.st) {
                St.IDLE, St.BUSY -> if (idle == null) idle = a
                St.OPENING, St.WAITING -> if (pending == null) pending = a
                St.JOINED -> if (joined == null) joined = a
            }
        }
        if (idle != null) {
            LinkNative.muxAccept(c.link, e.channel, idle.machine, idle.device)
            joined(idle, c, e.channel, e.machine, e.device)
            return
        }
        if (pending != null) {
            // Both sides opened at once: the link's initiator keeps its own channel, the other yields.
            if (c.outgoing) { LinkNative.muxWaiting(c.link, e.channel); r.held += Held(e.channel, e.channelKind, e.machine, e.device); return }
            LinkNative.muxClose(c.link, pending.ch, LinkWire.CLOSE_NORMAL)
            LinkNative.muxAccept(c.link, e.channel, pending.machine, pending.device)
            joined(pending, c, e.channel, e.machine, e.device)
            return
        }
        if (joined != null) { LinkNative.muxBusy(c.link, e.channel, joined.farMachine, joined.farDevice); return }
        LinkNative.muxWaiting(c.link, e.channel)
        r.held += Held(e.channel, e.channelKind, e.machine, e.device)
    }

    private fun matchPeer(hex: String) {
        if (hex.isEmpty()) return
        val r = runs[hex] ?: return
        val c = r.link ?: return
        if (!c.up || c.link == 0L || linkState(c) != LinkState.UP) return
        for (a in attaches) {
            if (a.bt || a.link != hex || !(a.st == St.IDLE || a.st == St.BUSY) || a.retryAt > now()) continue
            val at = r.held.indexOfFirst { it.kind == a.kind }
            if (at >= 0) {
                val h = r.held.removeAt(at)
                if (LinkNative.muxAccept(c.link, h.ch, a.machine, a.device)) {
                    joined(a, c, h.ch, h.machine, h.device)
                    continue
                }
            }
            val ch = LinkNative.muxOpen(c.link, a.kind, a.machine, a.device)
            if (ch == 0) continue
            a.st = St.OPENING; a.ch = ch; a.chLink = c
        }
        // Held requests of a kind while every machine here of that kind is joined are busy.
        for (kind in listOf(LinkWire.KIND_INFRARED, LinkWire.KIND_SERIAL)) {
            val joined = attaches.firstOrNull { it.link == hex && it.kind == kind && it.st == St.JOINED } ?: continue
            if (attaches.any { it.link == hex && it.kind == kind && it.st != St.JOINED }) continue
            val busy = r.held.filter { it.kind == kind }
            for (h in busy) LinkNative.muxBusy(c.link, h.ch, joined.farMachine, joined.farDevice)
            r.held.removeAll(busy.toSet())
        }
        pump(c)
    }

    private fun removeAttach(a: Attach) {
        if (a.bt) dropAttachBt(a)
        unjoin(a, true)
        attaches.remove(a)
        dropCore(a)
        matchPeer(a.link)
    }

    private fun unjoin(a: Attach, closeChannel: Boolean) {
        val c = a.chLink
        if (c != null && a.ch != 0 && c in conns && c.link != 0L && closeChannel) {
            LinkNative.muxClose(c.link, a.ch, LinkWire.CLOSE_GONE)
            pump(c)
        }
        a.st = St.IDLE; a.ch = 0; a.chLink = null
        a.unacked = 0; a.pendingIn.reset(); a.outHeld = null
    }

    // The channel is gone; the machine waits for the next one.
    private fun clearChannel(a: Attach) {
        a.st = St.IDLE; a.ch = 0; a.chLink = null; a.farMachine = ""; a.farDevice = ""; a.farState = PeerState.RUNNING
        a.unacked = 0; a.pendingIn.reset()
    }

    // ---- The core's infrared and serial sockets ----

    private fun closeCore(a: Attach) {
        a.coreGeneration++
        try { a.core?.close() } catch (_: IOException) {}
        a.core = null
        // Lets a serial reader waiting on the gate see the socket is gone, and ends the writer.
        a.gate?.release()
        a.gate = null
        a.writer?.offer(ByteArray(0))
        a.writer = null
        a.outHeld = null
    }

    private fun dropCore(a: Attach) {
        closeCore(a)
        a.coreUp = false
        a.unacked = 0
        a.pendingIn.reset()
    }

    // Serial ports are open only while joined: opened as the channel joins, closed as it goes.
    private fun syncCores() {
        for (a in ArrayList(attaches)) {
            if (!a.serial) continue
            if (a.st != St.JOINED) { if (a.core != null || a.coreUp) dropCore(a) }
            else if (a.core == null && !a.coreUp && now() >= a.nextCoreTry) coreConnect(a)
        }
    }

    private fun coreConnect(a: Attach) {
        closeCore(a)
        a.nextCoreTry = now() + CORE_RETRY_MS
        if (a.socketPath.isEmpty()) return
        val s = LocalSocket()
        try {
            s.connect(LocalSocketAddress(a.socketPath, LocalSocketAddress.Namespace.FILESYSTEM))
        } catch (e: IOException) {
            try { s.close() } catch (_: IOException) {}
            return
        }
        a.core = s
        a.coreUp = true
        val generation = a.coreGeneration
        coreState(a, ownState(a))
        val gate = if (a.serial) Semaphore(1) else null
        if (a.serial) {
            // Little held in the socket either way, so back-pressure reaches the core soon.
            try { s.sendBufferSize = SERIAL_AHEAD; s.receiveBufferSize = SERIAL_AHEAD } catch (_: IOException) {}
            a.gate = gate
            val queue = LinkedBlockingQueue<ByteArray>()
            a.writer = queue
            Thread({
                try {
                    val output = s.outputStream
                    while (true) {
                        val bytes = queue.take()
                        if (bytes.isEmpty()) break
                        output.write(bytes)
                        post { if (a in attaches && a.coreGeneration == generation) serialWritten(a, bytes.size) }
                    }
                } catch (_: IOException) {
                } catch (_: InterruptedException) {}
            }, "FrondLinkSerialOut").apply { isDaemon = true }.start()
            if (a.pendingIn.size() > 0) {
                val held = a.pendingIn.toByteArray()
                a.pendingIn.reset()
                a.unacked += held.size
                queue.offer(held)
            }
        }
        // Bytes from the core come in on their own thread and are handled on the engine's.
        Thread({
            val buffer = ByteArray(if (gate != null) SERIAL_AHEAD else 4096)
            try {
                val input = s.inputStream
                while (true) {
                    gate?.acquire()
                    if (a.coreGeneration != generation) break
                    val n = input.read(buffer)
                    if (n < 0) break
                    val bytes = buffer.copyOf(n)
                    post { if (a in attaches && a.coreGeneration == generation) coreBytes(a, bytes) }
                }
            } catch (_: IOException) {
            } catch (_: InterruptedException) {}
            post { if (a in attaches && a.coreGeneration == generation) coreClosed(a) }
        }, "FrondLinkCore").apply { isDaemon = true }.start()
    }

    // Sleep, load or reset closed the core's port; infrared reopens on wake, serial while still joined.
    private fun coreClosed(a: Attach) {
        // Serial bytes the core never took are lost with the socket; their credit goes back.
        if (a.unacked > 0) serialWritten(a, a.unacked)
        closeCore(a)
        val was = a.coreUp
        a.coreUp = false
        if (was) coreState(a, ownState(a))
        a.nextCoreTry = now() + CORE_RETRY_MS
    }

    private fun writeCore(a: Attach, data: ByteArray) {
        try { a.core?.outputStream?.write(data) } catch (e: IOException) { coreClosed(a) }
    }

    private fun coreBytes(a: Attach, data: ByteArray) {
        if (a.serial) { serialOut(a, data); return }
        if (a.st != St.JOINED) return  // nobody in range
        val c = a.chLink ?: return
        if (c !in conns || c.link == 0L) return
        var at = 0
        while (at < data.size) {
            val n = minOf(2048, data.size - at)
            LinkNative.muxSend(c.link, a.ch, data, at, n)
            at += n
        }
        pump(c)
    }

    private fun coreState(a: Attach, state: PeerState) {
        val c = a.chLink ?: return
        if (a.st == St.JOINED && c in conns && c.link != 0L) {
            LinkNative.muxSendState(c.link, a.ch, state.wire)
            pump(c)
        }
    }

    // ---- Serial, lossless: bytes move only as far as the other side has room ----

    // Bytes from the link; credit goes back once the writer has given them to the core.
    private fun serialIn(a: Attach, data: ByteArray) {
        val w = a.writer
        if (a.coreUp && w != null) { a.unacked += data.size; w.offer(data) } else a.pendingIn.write(data)
    }

    private fun serialWritten(a: Attach, n: Int) {
        val c = a.chLink ?: return
        if (c !in conns || c.link == 0L) return
        val k = minOf(n, a.unacked)
        a.unacked -= k
        if (k > 0) {
            LinkNative.muxConsumed(c.link, a.ch, k)
            pump(c)
        }
    }

    // A buffer the reader took from the core: what the mux has room for goes now, the rest when it drains.
    private fun serialOut(a: Attach, data: ByteArray) {
        a.outHeld = data
        a.outAt = 0
        serialPull(a)
        a.chLink?.let { pump(it) }
    }

    // Moves held core bytes into the mux; the reader goes on once they're all in.
    private fun serialPull(a: Attach): Boolean {
        val held = a.outHeld ?: return false
        val c = a.chLink
        if (a.st != St.JOINED || c == null || c !in conns || c.link == 0L) return false
        val took = LinkNative.muxSend(c.link, a.ch, held, a.outAt, held.size - a.outAt)
        a.outAt += took
        if (a.outAt >= held.size) { a.outHeld = null; a.gate?.release() }
        return took > 0
    }

    private fun pullSerial(c: Conn): Boolean {
        var moved = false
        for (a in attaches) if (a.serial && a.chLink === c && serialPull(a)) moved = true
        return moved
    }

    // ---- Bluetooth ----
    // A machine's core connects to the engine's socket for it, once per controller. Each controller goes to
    // this Frond's radio or, on a connection whose radio is on the other side, over a Bluetooth channel to
    // that side's radio. Lossless both ways, as serial, with the mux's 64 KB window.

    private fun radioHere(hex: String): Boolean {
        val p = store.find(hex) ?: return true
        return LinkWire.radioHere(store.identity.hex, hex, p.bluetoothRadio, p.bluetoothRadioAt)
    }

    private fun controllerOn(c: Conn, ch: Int) = controllers.firstOrNull { it.chLink === c && it.ch == ch }

    private fun btStatus(a: Attach) = JSONObject().put("type", "bt").put("listening", a.btServer != null)
        .put("controllers", controllers.count { it.owner === a })

    private fun btAttach(c: ClientState, op: JSONObject) {
        if (attaches.any { it.client === c && it.bt }) return  // one per client
        val a = Attach(c, op.optString("machine"), op.optString("device"), op.optString("link"), op.optString("socket"),
            LinkWire.KIND_BLUETOOTH)
        attaches += a
        if (a.socketPath.isNotEmpty()) btListen(a)
        if (a.link == LinkWire.RADIO) startRadio()
        else {
            val r = run(a.link)
            if (r.link == null && r.dialing == null) { r.nextTry = now(); r.backoff = 0 }
        }
    }

    // Listens at the attach's socket path (in a 0700 folder); the accept thread hands each connection over.
    private fun btListen(a: Attach) {
        val path = java.io.File(a.socketPath)
        path.delete()
        val bound = LocalSocket(LocalSocket.SOCKET_STREAM)
        val server = try {
            bound.bind(LocalSocketAddress(path.path, LocalSocketAddress.Namespace.FILESYSTEM))
            // Listens on the bound socket's descriptor, which stays bound's to close.
            LocalServerSocket(bound.fileDescriptor)
        } catch (e: IOException) {
            Log.w(TAG, "bluetooth: controller socket: $e")
            shut(bound)
            return
        }
        a.btServer = server
        a.btBound = bound
        Thread({
            while (true) {
                val s = try { server.accept() } catch (e: IOException) { break }
                post { if (a in attaches && a.btServer === server) btAccept(a, s) else shut(s) }
            }
        }, "FrondLinkBtWait").apply { isDaemon = true }.start()
    }

    private fun closeBtServer(a: Attach) {
        val server = a.btServer ?: return
        // Shutting the descriptor down wakes the thread blocked in accept(), which close() alone doesn't.
        try { Os.shutdown(server.fileDescriptor, OsConstants.SHUT_RDWR) } catch (_: Exception) {}
        try { server.close() } catch (_: IOException) {}
        a.btBound?.let(::shut)
        a.btServer = null
        a.btBound = null
        java.io.File(a.socketPath).delete()
    }

    private fun dropAttachBt(a: Attach) {
        for (k in ArrayList(controllers)) if (k.owner === a) dropController(k)
        closeBtServer(a)
    }

    // The core made a connection: a new controller.
    private fun btAccept(a: Attach, s: LocalSocket) {
        val k = Controller()
        k.owner = a
        k.core = s
        controllers += k
        btRoute(k)
    }

    // One of ours goes to this Frond's radio, or over the link once it's up. Until then it waits, unread.
    private fun btRoute(k: Controller) {
        val a = k.owner ?: return
        if (k.wantsRadio || k.chLink != null) return
        if (a.link == LinkWire.RADIO || (store.find(a.link) != null && radioHere(a.link))) {
            k.wantsRadio = true
            btRadioConnect(k)
            return
        }
        val c = runs[a.link]?.link ?: return
        if (!c.up || c.link == 0L || linkState(c) != LinkState.UP) return
        val ch = LinkNative.muxOpen(c.link, LinkWire.KIND_BLUETOOTH, a.machine, a.device)
        if (ch == 0) { dropController(k); return }
        k.chLink = c; k.ch = ch; k.open = false
        k.core?.let { startNear(k, it) }
        pump(c)
    }

    // The peer puts one of its controllers on our radio.
    private fun btRequest(c: Conn, e: LinkWire.ChannelEvent) {
        if (!radioHere(c.peer) || startRadio() == null) { LinkNative.muxClose(c.link, e.channel, LinkWire.CLOSE_GONE); return }
        val k = Controller()
        k.peer = c.peer
        k.machine = e.machine
        k.chLink = c; k.ch = e.channel; k.open = true
        k.wantsRadio = true
        controllers += k
        LinkNative.muxAccept(c.link, e.channel, store.identity.name, "")
        btRadioConnect(k)
    }

    private fun btChannelEvent(k: Controller, e: LinkWire.ChannelEvent) {
        when (e.kind) {
            EventKind.ACCEPTED -> { k.open = true; btPull(k) }
            EventKind.DATA -> {
                val w = k.writer
                // Credit goes back once the near socket takes them.
                if (w != null) { k.unacked += e.data.size; w.offer(e.data) } else k.pending.write(e.data)
            }
            EventKind.BUSY, EventKind.CLOSED -> dropController(k, false)
            else -> {}
        }
    }

    // Connects a controller to this Frond's radio. btvirt makes its socket a moment after it starts, so a
    // refused connect is tried again for a while, else the controller is lost.
    private fun btRadioConnect(k: Controller) {
        k.radioTry = now() + RADIO_RETRY_MS
        if (k.radioSock != null) return
        val path = startRadio() ?: return  // shown as no radio; it waits
        val s = LocalSocket()
        try {
            s.connect(LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM))
        } catch (e: IOException) {
            shut(s)
            val starting = host.radioRunning() && now() - radioStarted < RADIO_START_MS
            if (!starting) dropController(k)
            return
        }
        k.radioSock = s
        k.radioUp = true
        val core = k.core
        if (k.owner != null && core != null) startPipe(k, core, s) else startNear(k, s)
    }

    // Ours on this Frond's radio: the core and the radio joined straight, as blocking copies.
    private fun startPipe(k: Controller, a: LocalSocket, b: LocalSocket) {
        val generation = k.nearGeneration
        for ((from, to) in listOf(a to b, b to a)) {
            Thread({
                val buffer = ByteArray(BT_AHEAD)
                try {
                    val input = from.inputStream
                    val output = to.outputStream
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        output.write(buffer, 0, n)
                    }
                } catch (_: IOException) {}
                post { if (k in controllers && k.nearGeneration == generation) dropController(k) }
            }, "FrondLinkBtPipe").apply { isDaemon = true }.start()
        }
    }

    // The near socket of a controller on a channel: a gated reader and a writer, as serial.
    private fun startNear(k: Controller, s: LocalSocket) {
        val generation = ++k.nearGeneration
        val gate = Semaphore(1)
        val queue = LinkedBlockingQueue<ByteArray>()
        k.gate = gate
        k.writer = queue
        Thread({
            try {
                val output = s.outputStream
                while (true) {
                    val bytes = queue.take()
                    if (bytes.isEmpty()) break
                    output.write(bytes)
                    post { if (k in controllers && k.nearGeneration == generation) btWritten(k, bytes.size) }
                }
            } catch (_: IOException) {
            } catch (_: InterruptedException) {}
        }, "FrondLinkBtOut").apply { isDaemon = true }.start()
        Thread({
            val buffer = ByteArray(BT_AHEAD)
            try {
                val input = s.inputStream
                while (true) {
                    gate.acquire()
                    if (k.nearGeneration != generation) break
                    val n = input.read(buffer)
                    if (n < 0) break
                    val bytes = buffer.copyOf(n)
                    post { if (k in controllers && k.nearGeneration == generation) btBytes(k, bytes) }
                }
            } catch (_: IOException) {
            } catch (_: InterruptedException) {}
            post { if (k in controllers && k.nearGeneration == generation) dropController(k) }
        }, "FrondLinkBtIn").apply { isDaemon = true }.start()
        if (k.pending.size() > 0) {
            val held = k.pending.toByteArray()
            k.pending.reset()
            k.unacked += held.size
            queue.offer(held)
        }
    }

    private fun btBytes(k: Controller, data: ByteArray) {
        k.outHeld = data
        k.outAt = 0
        btPull(k)
        k.chLink?.let { pump(it) }
    }

    // Moves held near bytes into the mux, as far as the channel has room; the reader goes on once they're in.
    private fun btPull(k: Controller): Boolean {
        val held = k.outHeld ?: return false
        val c = k.chLink
        if (!k.open || c == null || c !in conns || c.link == 0L) return false
        val took = LinkNative.muxSend(c.link, k.ch, held, k.outAt, held.size - k.outAt)
        k.outAt += took
        if (k.outAt >= held.size) { k.outHeld = null; k.gate?.release() }
        return took > 0
    }

    private fun pullBluetooth(c: Conn): Boolean {
        var moved = false
        for (k in ArrayList(controllers)) if (k.chLink === c && btPull(k)) moved = true
        return moved
    }

    private fun btWritten(k: Controller, n: Int) {
        val c = k.chLink ?: return
        if (c !in conns || c.link == 0L) return
        val taken = minOf(n, k.unacked)
        k.unacked -= taken
        if (taken > 0) {
            LinkNative.muxConsumed(c.link, k.ch, taken)
            pump(c)
        }
    }

    // The controller is gone: the core sees its controller lost, the far side's channel closes.
    private fun dropController(k: Controller, closeChannel: Boolean = true) {
        if (!controllers.remove(k)) return
        k.nearGeneration++
        val c = k.chLink
        if (closeChannel && c != null && c in conns && c.link != 0L) {
            LinkNative.muxClose(c.link, k.ch, LinkWire.CLOSE_GONE)
            pump(c)
        }
        k.gate?.release()
        k.writer?.offer(ByteArray(0))
        k.radioSock?.let(::shut)
        k.core?.let(::shut)
        k.radioSock = null
        k.core = null
    }

    private fun btLinkUp(c: Conn) {
        if (c !in conns || c.link == 0L) return
        val p = store.find(c.peer)
        LinkNative.linkSendRadio(c.link, radioHere(c.peer), p?.bluetoothRadioAt ?: 0L)
        for (k in ArrayList(controllers)) if (k in controllers && k.owner?.link == c.peer) btRoute(k)
        pump(c)
    }

    private fun btLinkDown(c: Conn) {
        for (k in ArrayList(controllers)) if (k.chLink === c) dropController(k, false)
    }

    // The peer's view of whose radio it is (LinkWire.radioUpdate).
    private fun btRadioChoices(c: Conn) {
        while (c in conns && c.link != 0L) {
            val r = LinkNative.linkNextRadio(c.link) ?: break
            val p = store.find(c.peer) ?: continue
            when (val u = LinkWire.radioUpdate(store.identity.hex, c.peer, p.bluetoothRadio, p.bluetoothRadioAt, r[0] != 0L, r[1])) {
                is LinkWire.RadioUpdate.Take -> setRadio(c.peer, u.here, u.at)
                LinkWire.RadioUpdate.Answer -> LinkNative.linkSendRadio(c.link, radioHere(c.peer), p.bluetoothRadioAt)
                LinkWire.RadioUpdate.Keep -> {}
            }
        }
    }

    private fun setRadio(hex: String, here: Boolean, at: Long) {
        store.find(hex) ?: return
        store.update(hex) { it.copy(bluetoothRadio = if (here) "here" else "there", bluetoothRadioAt = at) }
        store.save()
        btReroute(hex)
    }

    // Controllers on the side that no longer holds the radio go; the cores reconnect to the new one.
    private fun btReroute(hex: String) {
        val here = radioHere(hex)
        for (k in ArrayList(controllers)) {
            if (k !in controllers) continue
            val owner = k.owner
            if (owner != null && owner.link == hex && ((here && k.chLink != null) || (!here && k.wantsRadio))) dropController(k)
            else if (owner == null && k.peer == hex && !here) dropController(k)
        }
    }

    private fun radioWanted() = controllers.any { it.wantsRadio } || attaches.any { it.bt && it.link == LinkWire.RADIO }

    // This Frond's radio's socket, starting it if it isn't running. Null while there's none.
    private fun startRadio(): String? {
        val wasRunning = radioOn && host.radioRunning()
        if (wasRunning) return radioPath
        if (radioFailedAt != 0L && now() - radioFailedAt < RADIO_START_MS) return null
        val path = host.startRadio()
        if (path == null) { radioFailedAt = now(); return null }
        radioFailedAt = 0L
        radioOn = true
        radioPath = path
        radioStarted = now()
        radioIdleSince = now()
        return path
    }

    private fun radioTick() {
        val t = now()
        for (k in ArrayList(controllers)) if (k in controllers && k.wantsRadio && k.radioSock == null && t >= k.radioTry) btRadioConnect(k)
        if (!radioOn) return
        if (radioWanted()) { radioIdleSince = t; return }
        if (t - radioIdleSince < RADIO_IDLE_MS) return
        host.stopRadio()
        radioOn = false
    }

    private fun peerBluetooth(hex: String): String? {
        val parts = ArrayList<String>()
        for (a in attaches) if (a.bt && a.link == hex && controllers.any { it.owner === a }) parts += a.machine
        for (k in controllers) if (k.owner == null && k.peer == hex && k.machine !in parts) parts += k.machine
        return if (parts.isEmpty()) null else "Bluetooth: " + parts.joinToString(", ")
    }

    private fun shut(c: Closeable) {
        if (c is LocalSocket) {
            try { Os.shutdown(c.fileDescriptor, OsConstants.SHUT_RDWR) } catch (_: Exception) {}
        }
        try { c.close() } catch (_: IOException) {}
    }

    // ---- Discovery ----

    private fun discoveryActive() = watchers() > 0 || pairOut.client != null || attaches.any { it.link != LinkWire.RADIO }

    private fun beacon(type: Int, to: InetSocketAddress? = null) {
        val d = udp ?: return
        val datagram = LinkWire.beacon(type, store.identity.id, listenPort, store.identity.name)
        try {
            if (to != null) { d.send(ByteBuffer.wrap(datagram), to); return }
            val targets = LinkedHashSet<InetAddress>()
            targets += InetAddress.getByAddress(byteArrayOf(-1, -1, -1, -1))
            try {
                for (i in NetworkInterface.getNetworkInterfaces()?.toList() ?: emptyList()) {
                    if (!i.isUp || i.isLoopback) continue
                    for (address in i.interfaceAddresses) address.broadcast?.takeIf { it is Inet4Address }?.let { targets += it }
                }
            } catch (_: Exception) {}
            for (t in targets) try { d.send(ByteBuffer.wrap(datagram), InetSocketAddress(t, host.discoveryPort)) } catch (_: IOException) {}
        } catch (e: IOException) {
        }
    }

    private val udpBuffer = ByteBuffer.allocate(512)

    private fun datagrams() {
        val d = udp ?: return
        while (true) {
            udpBuffer.clear()
            val from = try { d.receive(udpBuffer) as? InetSocketAddress } catch (e: IOException) { null } ?: return
            if (!LinkWire.allowedPeer(from.address)) continue
            val b = LinkWire.parseBeacon(udpBuffer.array(), udpBuffer.position()) ?: continue
            if (b.id.contentEquals(store.identity.id)) continue
            if (b.type == LinkWire.QUERY && discoveryActive()) beacon(LinkWire.BEACON, from)
            val hex = LinkWire.hex(b.id)
            val p = store.find(hex)
            if (p != null && p.paired) {
                val r = run(hex)
                val moved = r.seen != from.address || r.seenPort != b.port || now() - r.seenAt > NEARBY_MS
                r.seen = from.address
                r.seenPort = b.port
                r.seenAt = now()
                // A peer that moved or came back is dialled at once, at most once a second, starting at the new address.
                if (moved && wanted(hex) && r.link == null && r.dialing == null && now() - r.beaconDialAt >= LinkWire.backoff(0)) {
                    r.nextTry = now(); r.target = 0; r.beaconDialAt = now()
                }
            } else {
                nearby[hex] = Nearby(b.name, from.address, b.port, now())
            }
        }
    }

    // ---- Status ----

    private fun status(a: Attach): JSONObject {
        if (a.bt && a.link == LinkWire.RADIO)
            return btStatus(a).put("peer", "This Frond").put("state", if (host.radioAvailable()) "radio" else "noradio")
        val p = store.find(a.link)
        val r = runs[a.link]
        val state = when {
            p == null -> "unknown"
            !host.networkAllowed() -> "nonetwork"
            p.keyChanged || (r != null && r.keyMismatch && r.link == null) -> "keychanged"
            r != null && r.forgotten -> "forgotten"
            a.st == St.JOINED -> "joined"
            a.st == St.WAITING -> "waiting"
            a.st == St.BUSY -> "busy"
            r != null && r.failed && r.link == null -> "notfound"
            else -> "connecting"
        }
        if (a.bt) {
            // Bluetooth has no channel of its own to wait for: once linked, it's on one side's radio.
            val o = btStatus(a).put("peer", p?.name ?: "")
            var st = state
            if (p != null) {
                val here = radioHere(a.link)
                o.put("radio", if (here) "here" else "there")
                if (st == "connecting") st = if (r?.link?.up == true) { if (!here) "there" else if (host.radioAvailable()) "here" else "noradio" } else "connecting"
            }
            return o.put("state", st)
        }
        return JSONObject().put("type", if (a.serial) "serial" else "ir").put("peer", p?.name ?: "").put("state", state).put("machine", a.farMachine)
            .put("device", a.farDevice).put("busyBy", a.busyBy).put("core", a.coreUp).put("far", a.farState.text)
    }

    private fun peerChannels(hex: String): String =
        listOf(LinkWire.KIND_INFRARED to "Infrared: ", LinkWire.KIND_SERIAL to "Serial: ").mapNotNull { (kind, title) ->
            val parts = attaches.filter { it.link == hex && it.kind == kind && it.st == St.JOINED }
                .map { "${it.machine} – ${it.farMachine.ifEmpty { it.farDevice }}" }
            if (parts.isEmpty()) null else title + parts.joinToString(", ")
        }.plus(listOfNotNull(peerBluetooth(hex))).joinToString(" · ")

    private fun snapshot(): JSONObject {
        val list = JSONArray()
        for (p in store.peers) {
            val r = runs[p.id]
            val up = r?.link?.up == true
            val o = JSONObject().put("id", p.id).put("name", p.name).put("peerName", p.peerName).put("address", p.address)
                .put("keyChanged", p.keyChanged).put("up", up).put("lastSeen", p.lastSeen)
            if (r != null) {
                o.put("forgotten", r.forgotten)
                if (r.keyMismatch && !up) o.put("keyChanged", true)
                if (up) o.put("rtt", LinkNative.linkRtt(r.link!!.link))
                val seen = r.seen
                if (seen != null && now() - r.seenAt < NEARBY_MS) o.put("seen", seen.hostAddress)
            }
            o.put("channels", peerChannels(p.id))
            o.put("bluetoothRadio", if (radioHere(p.id)) "here" else "there")
            list.put(o)
        }
        val near = JSONArray()
        for ((id, n) in nearby) near.put(JSONObject().put("id", id).put("name", n.name).put("address", n.address.hostAddress).put("port", n.port))
        return JSONObject().put("type", "state").put("id", store.identity.hex).put("name", store.identity.name).put("port", listenPort)
            .put("connections", list).put("nearby", near)
    }

    private fun tick() {
        val t = now()
        for (c in ArrayList(conns)) {
            if (c !in conns) continue
            if (c.closing) {
                if ((t - c.created > 2000 && !c.connected) || t - c.closingAt > CLOSING_MS) removeConn(c)
                else flush(c)
                continue
            }
            if (c.kind == Kind.UNKNOWN && t - c.created > FIRST_FRAME_MS) { closeConn(c, false); continue }
            if (c.outgoing && !c.connected && t - c.created > 3000) { removeConn(c); continue }
            if (c.link != 0L && c.connected) { LinkNative.linkTick(c.link, t); afterLink(c) }
            else if (c.kind == Kind.LINK && !c.outgoing && t - c.created > FIRST_FRAME_MS && !c.up) closeConn(c, false)
            if (c in conns && c.pairing != 0L) { LinkNative.pairTick(c.pairing, t); pump(c); pairDone(c) }
            else if (c.kind == Kind.PAIR_OUT && t - c.created > PAIR_OUT_MS) closeConn(c, false)
        }
        // Links: dial what's wanted, drop what nothing uses.
        for (p in store.peers) {
            val want = wanted(p.id)
            if (!want && !runs.containsKey(p.id)) continue
            val r = run(p.id)
            if (want && r.link == null && r.dialing == null && t >= r.nextTry && host.networkAllowed()) dial(p.id)
            val link = r.link
            if (link != null && !want && r.held.isEmpty() && t - r.idleSince > LINK_IDLE_MS) {
                if (attaches.none { it.chLink === link } && controllers.none { it.chLink === link }) closeConn(link)
            } else if (want || r.held.isNotEmpty()) r.idleSince = t
        }
        // Cores: reconnect after sleep, load or reset.
        for (a in ArrayList(attaches)) if (!a.serial && !a.bt && !a.coreUp && a.core == null && t >= a.nextCoreTry) coreConnect(a)
        radioTick()
        syncCores()
        for (a in ArrayList(attaches)) if ((a.st == St.BUSY || a.st == St.IDLE) && a.retryAt <= t) matchPeer(a.link)
        // Discovery.
        val active = discoveryActive()
        if (active != wasActive) host.discovery(active)
        if (active && (!wasActive || t - lastBeacon >= BEACON_MS)) {
            if (!wasActive) beacon(LinkWire.QUERY)
            beacon(LinkWire.BEACON)
            lastBeacon = t
        }
        wasActive = active
        nearby.entries.removeAll { t - it.value.at > NEARBY_MS }
        recent.entries.removeAll { it.value.isEmpty() || t - it.value.last() > 10000 }
        if (code != 0L && t - codeAt >= PAIR_CODE_MS) endPairRequest("expired")
        // Status.
        for (a in attaches) {
            val s = status(a)
            if (s.toString() != a.lastStatus?.toString()) { a.lastStatus = s; sendTo(a.client, s) }
        }
        if (watchers() > 0) {
            val s = snapshot()
            if (s.toString() != lastSnapshot?.toString()) { lastSnapshot = s; broadcastWatchers(s) }
        } else lastSnapshot = null
    }

    companion object {
        private const val TAG = "Frond"
        // Bytes queued for a link socket before the engine stops taking more from the mux, so infrared never
        // waits long behind other bytes. The kernel buffer is capped too.
        const val WRITE_AHEAD = 4096
        const val SEND_BUFFER = 8192
        const val BEACON_MS = 3000L
        const val NEARBY_MS = 10000L
        const val FIRST_FRAME_MS = 10000L
        const val LINK_IDLE_MS = 15000L
        // A closing connection's last bytes get this long to go.
        const val CLOSING_MS = 3000L
        const val CORE_RETRY_MS = 250L
        // Serial bytes held for the core socket each way before the core's own queue fills and its CTS drops.
        const val SERIAL_AHEAD = 4096
        const val BUSY_RETRY_MS = 5000L
        const val PAIR_COOLDOWN_MS = 60000L
        const val PAIR_CODE_MS = 120000L
        const val PAIR_OUT_MS = 120000L
        // Connections not yet authenticated, and new connections per address in 10 s.
        const val MAX_PENDING = 16
        const val PER_ADDRESS = 10
        // Bluetooth bytes read ahead from a core or the radio at a time; the rest waits in its socket.
        const val BT_AHEAD = 16384
        const val RADIO_RETRY_MS = 100L
        // btvirt gets this long to make its socket, and keeps running this long with no controller on it.
        const val RADIO_START_MS = 5000L
        const val RADIO_IDLE_MS = 5000L
    }
}
