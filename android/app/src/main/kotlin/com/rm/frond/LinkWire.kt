package com.rm.frond

import java.io.ByteArrayOutputStream
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress

// What the link engine shares with the desktop hub outside the link/ library: the enums LinkNative
// returns by ordinal (link/*.h), the beacon and pair request frames (desktop/LinkHub.cpp), addresses
// and the status texts. Pure Kotlin, so the JVM tests cover it.
object LinkWire {
    const val DEFAULT_PORT = 5035

    enum class FirstFrame { NEED_MORE, NOISE, PAIR, BAD_VERSION, GARBAGE }
    enum class LinkState { HANDSHAKE, HELLO, UP, DOWN }
    enum class LinkError { NONE, CLOSED, PROTOCOL, DECRYPT, VERSION, UNKNOWN_PEER, KEY_CHANGED, TIMEOUT }
    enum class PairResult { PENDING, PAIRED, WRONG_CODE, EXPIRED, LOCKED_OUT, VERSION, PROTOCOL, TIMEOUT }
    enum class EventKind { OPEN_REQUEST, ACCEPTED, WAITING, BUSY, DATA, CLOSED, STATE }

    // ChannelKind, CloseReason, PeerState and RefuseReason values on the wire.
    const val KIND_INFRARED = 1
    const val KIND_SERIAL = 2
    const val KIND_BLUETOOTH = 3
    // A Bluetooth attach's link for This Frond's radio, with no connection.
    const val RADIO = "radio"
    const val CLOSE_NORMAL = 0
    const val CLOSE_GONE = 4
    enum class PeerState(val wire: Int, val text: String) {
        RUNNING(0, "running"), ASLEEP(1, "asleep"), PAUSED(2, "paused"), RESET(3, "reset");
        companion object { fun of(wire: Int) = entries.firstOrNull { it.wire == wire } ?: RUNNING }
    }
    const val REFUSE_VERSION = 1
    const val REFUSE_NOT_PAIRING = 7
    const val REFUSE_PROTOCOL = 8

    // A channel event from LinkNative.muxNextEvent: kind u8, channel u16, channel kind u8, code u8,
    // machine str8, device str8, then the data.
    class ChannelEvent(val kind: EventKind, val channel: Int, val channelKind: Int, val code: Int,
                       val machine: String, val device: String, val data: ByteArray) {
        companion object {
            fun decode(b: ByteArray): ChannelEvent? {
                val r = Reader(b)
                val kind = r.u8()
                val ch = r.u16()
                val ck = r.u8()
                val code = r.u8()
                val machine = r.str8()
                val device = r.str8()
                if (!r.ok || kind >= EventKind.entries.size) return null
                return ChannelEvent(EventKind.entries[kind], ch, ck, code, machine, device, r.rest())
            }
        }
    }

    // ---- Bytes ----

    class Reader(private val b: ByteArray, private var at: Int = 0, private val end: Int = b.size) {
        var ok = true
            private set
        val left get() = end - at
        fun take(n: Int): ByteArray? {
            if (!ok || n > left) { ok = false; return null }
            return b.copyOfRange(at, at + n).also { at += n }
        }
        fun u8(): Int = take(1)?.let { it[0].toInt() and 0xff } ?: 0
        fun u16(): Int = take(2)?.let { ((it[0].toInt() and 0xff) shl 8) or (it[1].toInt() and 0xff) } ?: 0
        fun str8(): String = take(u8())?.toString(Charsets.UTF_8) ?: ""
        fun rest(): ByteArray = b.copyOfRange(at, end).also { at = end }
    }

    // A u8 length then the UTF-8 bytes, cut to 255 bytes as putStr8 does.
    fun putStr8(out: ByteArrayOutputStream, s: String) {
        val bytes = s.toByteArray(Charsets.UTF_8).let { if (it.size > 255) it.copyOf(255) else it }
        out.write(bytes.size)
        out.write(bytes)
    }

    fun putU16(out: ByteArrayOutputStream, v: Int) {
        out.write((v shr 8) and 0xff)
        out.write(v and 0xff)
    }

    fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it.toInt() and 0xff) }

    // Lower-case hex of exactly n bytes, or null.
    fun unhex(text: String, n: Int): ByteArray? {
        if (text.length != n * 2 || text.any { it !in '0'..'9' && it !in 'a'..'f' }) return null
        return ByteArray(n) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    }

    // ---- Frames ----

    private val MAGIC = byteArrayOf('F'.code.toByte(), 'R'.code.toByte(), 'L'.code.toByte())
    private const val WIRE_VERSION = 1
    const val HEADER_LEN = 5

    // A's request before CPace, and B's answer: frame length, header "FRL" 1 'P', step 0, id, name.
    fun pairRequest(id: ByteArray, name: String): ByteArray {
        val body = ByteArrayOutputStream()
        body.write(MAGIC)
        body.write(WIRE_VERSION)
        body.write('P'.code)
        body.write(0)
        body.write(id)
        putStr8(body, name)
        val b = body.toByteArray()
        return ByteArrayOutputStream().apply { putU16(this, b.size); write(b) }.toByteArray()
    }

    sealed class PairFrame {
        class Request(val id: ByteArray, val name: String) : PairFrame()
        class Refused(val reason: Int) : PairFrame()
        object Bad : PairFrame()
    }

    // The body of a frame (no length) read as a request or answer, or a refuse frame.
    fun parsePairFrame(body: ByteArray): PairFrame {
        if (body.size < HEADER_LEN || !body.copyOfRange(0, 3).contentEquals(MAGIC)) return PairFrame.Bad
        val mode = body[4].toInt().toChar()
        if (mode == 'R') return PairFrame.Refused(if (body.size > HEADER_LEN) body[HEADER_LEN].toInt() and 0xff else REFUSE_PROTOCOL)
        if (mode != 'P' || body[3].toInt() != WIRE_VERSION) return PairFrame.Bad
        val r = Reader(body, HEADER_LEN)
        val step = r.u8()
        val id = r.take(16)
        val name = r.str8()
        if (!r.ok || step != 0 || id == null) return PairFrame.Bad
        return PairFrame.Request(id, name.take(64))
    }

    // One whole frame's body from the start of buf, and its length with the u16 prefix; null while short.
    fun frameAt(buf: ByteArray, n: Int): Pair<ByteArray, Int>? {
        if (n < 2) return null
        val len = ((buf[0].toInt() and 0xff) shl 8) or (buf[1].toInt() and 0xff)
        if (n < 2 + len) return null
        return buf.copyOfRange(2, 2 + len) to 2 + len
    }

    // ---- Discovery ----
    // Beacon: "FRLB", version 1, type (1 beacon, 2 query), install id, TCP port, name.

    const val BEACON = 1
    const val QUERY = 2

    class Beacon(val type: Int, val id: ByteArray, val port: Int, val name: String)

    fun beacon(type: Int, id: ByteArray, port: Int, name: String): ByteArray = ByteArrayOutputStream().apply {
        write(MAGIC); write('B'.code); write(1); write(type); write(id); putU16(this, port); putStr8(this, name)
    }.toByteArray()

    fun parseBeacon(b: ByteArray, n: Int = b.size): Beacon? {
        if (n < 4 || !b.copyOfRange(0, 3).contentEquals(MAGIC) || b[3] != 'B'.code.toByte()) return null
        val r = Reader(b, 4, n)
        val version = r.u8()
        val type = r.u8()
        val id = r.take(16)
        val port = r.u16()
        val name = r.str8().take(64)
        if (!r.ok || version != 1 || id == null) return null
        return Beacon(type, id, port, name)
    }

    // ---- Addresses ----

    // host, host:port or [v6]:port; the port is DEFAULT_PORT when missing. Null when it can't be used.
    fun hostPort(text: String): Pair<String, Int>? = NetRelay.address(text, DEFAULT_PORT)

    fun showHostPort(host: String, port: Int) = NetRelay.hostPort(host, port)

    // An IPv4 or IPv6 literal, which InetAddress takes without a name lookup.
    fun numericHost(host: String): Boolean =
        host.isNotEmpty() && (':' in host || host.all { it.isDigit() || it == '.' })

    // Peers that may dial in: loopback, private, link-local and ULA addresses.
    fun allowedPeer(address: InetAddress?): Boolean {
        val b = address?.address ?: return false
        if (address is Inet4Address) {
            val a = b[0].toInt() and 0xff
            val c = b[1].toInt() and 0xff
            return a == 127 || a == 10 || (a == 172 && c in 16..31) || (a == 192 && c == 168) || (a == 169 && c == 254)
        }
        if (address !is Inet6Address) return false
        if (address.isLoopbackAddress) return true
        // IPv4-mapped addresses follow the IPv4 rule.
        if (b.take(10).all { it.toInt() == 0 } && b[10].toInt() == -1 && b[11].toInt() == -1)
            return allowedPeer(InetAddress.getByAddress(b.copyOfRange(12, 16)))
        val first = b[0].toInt() and 0xff
        val second = b[1].toInt() and 0xff
        return (first == 0xfe && (second and 0xc0) == 0x80) || (first and 0xfe) == 0xfc
    }

    val BACKOFF = longArrayOf(1000, 2000, 5000, 10000, 30000)
    fun backoff(step: Int) = BACKOFF[step.coerceIn(0, BACKOFF.size - 1)]

    fun pairResultName(r: PairResult) = when (r) {
        PairResult.PAIRED -> "paired"
        PairResult.WRONG_CODE -> "wrongcode"
        PairResult.EXPIRED -> "expired"
        PairResult.LOCKED_OUT -> "lockedout"
        PairResult.VERSION -> "version"
        PairResult.TIMEOUT -> "timeout"
        else -> "failed"
    }

    // ---- Text, as the desktop shows it ----

    fun pairResultText(result: String, name: String) = when (result) {
        "wrongcode" -> "Wrong code"
        "lockedout" -> "Too many wrong codes. Try again in a minute."
        "expired" -> "The code ran out. Pair again."
        "notpairing" -> "Open Connections on $name first."
        "unreachable" -> "Couldn't reach $name."
        "version" -> "$name runs a Frond that can't pair with this one."
        else -> "Pairing didn't finish."
    }

    // "482913" as "482 913".
    fun showCode(code: String) = if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code

    data class ConnectionRow(
        val id: String, val name: String, val peerName: String, val address: String,
        val up: Boolean, val rtt: Long, val keyChanged: Boolean, val forgotten: Boolean, val seen: String, val channels: String,
        // Whose radio Bluetooth uses with it: here or there.
        val bluetoothRadio: String = "here",
    )

    fun connectionStatus(c: ConnectionRow): String = when {
        c.keyChanged -> "Key changed, pair again"
        c.forgotten -> "Not paired on ${c.peerName}, pair again"
        c.up -> "Connected · ${c.rtt} ms" + if (c.channels.isNotEmpty()) " · ${c.channels}" else ""
        c.seen.isNotEmpty() -> "Seen at ${c.seen}"
        else -> "Not seen"
    }

    // The engine's ir event as the machine's status line.
    data class IrStatus(
        val state: String = "connecting", val peer: String = "", val machine: String = "", val device: String = "",
        val busyBy: String = "", val far: String = "running", val core: Boolean = false,
    )

    fun irText(s: IrStatus): String = channelText("Infrared", s)

    // title is Infrared or Serial. A serial port whose far side closed shows as closed rather than asleep.
    fun channelText(title: String, s: IrStatus): String {
        val far = s.machine.ifEmpty { s.device }
        return when (s.state) {
            "joined" -> when (s.far) {
                "asleep" -> if (title == "Serial") "$title: ${s.peer}, port closed" else "$title: ${s.peer}, Palm asleep"
                "paused" -> "$title: ${s.peer}, Palm paused"
                else -> "$title: ${s.peer}, joined to $far"
            }
            "waiting" -> "$title: waiting for a machine on ${s.peer}"
            "busy" -> "$title on ${s.peer} is in use by ${s.busyBy}"
            "notfound" -> "$title: ${s.peer} not found"
            "keychanged" -> "$title: ${s.peer} key changed, pair again"
            "forgotten" -> "$title: pair again with ${s.peer}"
            "unknown" -> "$title: Unknown connection"
            "nonetwork" -> "$title: local network access is off"
            else -> "$title: connecting to ${s.peer}"
        }
    }

    // ---- Bluetooth radio ----
    // Bluetooth over a connection uses one side's radio. The later choice wins, at the same time the lower
    // install id's; with no choice yet (at 0) the side with the lower install id holds it. As desktop/LinkHub.cpp.

    fun radioHere(localHex: String, peerHex: String, radio: String, at: Long) =
        if (at > 0) radio != "there" else localHex < peerHex

    sealed class RadioUpdate {
        object Keep : RadioUpdate()
        // Ours is newer: send it back.
        object Answer : RadioUpdate()
        data class Take(val here: Boolean, val at: Long) : RadioUpdate()
    }

    // The peer's Radio message (senderHolds as the peer says it, at) against what's stored here.
    fun radioUpdate(localHex: String, peerHex: String, radio: String, storedAt: Long, senderHolds: Boolean, at: Long): RadioUpdate {
        val theyMeanHere = !senderHolds
        val same = theyMeanHere == radioHere(localHex, peerHex, radio, storedAt)
        if (at > storedAt || (at == storedAt && at > 0 && !same && peerHex < localHex))
            return if (!same || at != storedAt) RadioUpdate.Take(theyMeanHere, at) else RadioUpdate.Keep
        return if (at < storedAt) RadioUpdate.Answer else RadioUpdate.Keep
    }

    // The engine's bt event.
    data class BtStatus(val state: String = "connecting", val peer: String = "", val listening: Boolean = true, val controllers: Int = 0)

    fun bluetoothText(s: BtStatus): String = when {
        !s.listening -> "Bluetooth: couldn't make the controller socket"
        s.state == "radio" -> "Bluetooth: This Frond's radio"
        s.state == "noradio" -> "Bluetooth: this Frond has no radio"
        s.state == "here" -> "Bluetooth: this Frond's radio, with ${s.peer}"
        s.state == "there" -> "Bluetooth: ${s.peer}'s radio"
        s.state == "notfound" -> "Bluetooth: ${s.peer} not found"
        s.state == "keychanged" -> "Bluetooth: ${s.peer} key changed, pair again"
        s.state == "forgotten" -> "Bluetooth: pair again with ${s.peer}"
        s.state == "unknown" -> "Bluetooth: Unknown connection"
        s.state == "nonetwork" -> "Bluetooth: local network access is off"
        else -> "Bluetooth: connecting to ${s.peer}"
    }
}
