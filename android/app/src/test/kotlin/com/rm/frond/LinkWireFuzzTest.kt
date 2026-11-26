package com.rm.frond

import com.code_intelligence.jazzer.junit.FuzzTest

// LinkWire's frames as LinkEngine reads them from a TCP connection, beacons from UDP and the
// channel events LinkNative hands back. Seeds in resources/com/rm/frond/LinkWireFuzzTestInputs.
class LinkWireFuzzTest {
    // The bytes arrive in chunks sized by the first byte, and are taken apart the way LinkEngine's
    // pre-buffer does: frameAt until it needs more, each body through parsePairFrame.
    @FuzzTest
    fun pairFrames(data: ByteArray) {
        if (data.isEmpty()) return
        val step = (data[0].toInt() and 0x3f) + 1
        var pre = ByteArray(0)
        var at = 1
        while (at < data.size) {
            pre += data.copyOfRange(at, minOf(data.size, at + step))
            at += step
            while (true) {
                val (body, used) = LinkWire.frameAt(pre, pre.size) ?: break
                check(used == body.size + 2 && used <= pre.size)
                pre = pre.copyOfRange(used, pre.size)
                when (val f = LinkWire.parsePairFrame(body)) {
                    is LinkWire.PairFrame.Request -> {
                        check(f.id.size == 16 && f.name.length <= 64)
                        // What B sends back is read the same way.
                        val again = LinkWire.pairRequest(f.id, f.name)
                        val (b2, n2) = checkNotNull(LinkWire.frameAt(again, again.size))
                        check(n2 == again.size)
                        val r = LinkWire.parsePairFrame(b2) as LinkWire.PairFrame.Request
                        check(r.id.contentEquals(f.id) && r.name == f.name) { "pair name ${f.name} came back as ${r.name}" }
                    }
                    is LinkWire.PairFrame.Refused -> check(f.reason in 0..255)
                    LinkWire.PairFrame.Bad -> {}
                }
            }
        }
    }

    // A datagram in a bigger receive buffer, as LinkEngine passes the buffer and its position.
    @FuzzTest
    fun beacons(data: ByteArray) {
        val buffer = data.copyOf(data.size + 64)
        val b = LinkWire.parseBeacon(buffer, data.size) ?: return
        check(b.id.size == 16 && b.port in 0..65535 && b.name.length <= 64)
        val again = LinkWire.parseBeacon(LinkWire.beacon(b.type, b.id, b.port, b.name))
        check(again != null && again.type == b.type && again.id.contentEquals(b.id) && again.port == b.port &&
            again.name == b.name) { "beacon name ${b.name} came back as ${again?.name}" }
    }

    @FuzzTest
    fun channelEvents(data: ByteArray) {
        val e = LinkWire.ChannelEvent.decode(data) ?: return
        check(e.channel in 0..0xffff && e.channelKind in 0..255 && e.code in 0..255)
        check(e.machine.length <= 255 && e.device.length <= 255 && e.data.size <= data.size)
    }

    @FuzzTest
    fun hex(data: ByteArray) {
        val text = String(data, Charsets.ISO_8859_1)
        for (n in listOf(16, 32, text.length / 2)) {
            val b = LinkWire.unhex(text, n) ?: continue
            check(b.size == n && LinkWire.hex(b) == text)
        }
    }
}
