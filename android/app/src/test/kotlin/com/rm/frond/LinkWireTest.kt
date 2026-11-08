package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.InetAddress

class LinkWireTest {
    private val id = ByteArray(16) { (0xa0 + it).toByte() }

    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test fun enumsFollowLinkLibraryOrder() {
        // link/wire.h FirstFrame, link/link.h LinkState and LinkError, link/pairing.h PairResult, link/mux.h ChannelEvent::Kind.
        assertEquals(listOf("NEED_MORE", "NOISE", "PAIR", "BAD_VERSION", "GARBAGE"), LinkWire.FirstFrame.entries.map { it.name })
        assertEquals(listOf("HANDSHAKE", "HELLO", "UP", "DOWN"), LinkWire.LinkState.entries.map { it.name })
        assertEquals(6, LinkWire.LinkError.KEY_CHANGED.ordinal)
        assertEquals(5, LinkWire.LinkError.UNKNOWN_PEER.ordinal)
        assertEquals(1, LinkWire.PairResult.PAIRED.ordinal)
        assertEquals(4, LinkWire.PairResult.LOCKED_OUT.ordinal)
        assertEquals(listOf("OPEN_REQUEST", "ACCEPTED", "WAITING", "BUSY", "DATA", "CLOSED", "STATE"), LinkWire.EventKind.entries.map { it.name })
        assertEquals(listOf(0, 1, 2, 3), LinkWire.PeerState.entries.map { it.wire })
        assertEquals(LinkWire.PeerState.PAUSED, LinkWire.PeerState.of(2))
    }

    @Test fun beaconMatchesTheHub() {
        val b = LinkWire.beacon(LinkWire.BEACON, id, 5035, "Pixel 8")
        // "FRLB", version 1, type, id, port, name.
        val expected = bytes('F'.code, 'R'.code, 'L'.code, 'B'.code, 1, 1) + id + bytes(0x13, 0xab, 7) + "Pixel 8".toByteArray()
        assertArrayEquals(expected, b)
        val back = LinkWire.parseBeacon(b)!!
        assertEquals(LinkWire.BEACON, back.type)
        assertArrayEquals(id, back.id)
        assertEquals(5035, back.port)
        assertEquals("Pixel 8", back.name)
        assertEquals(LinkWire.QUERY, LinkWire.parseBeacon(LinkWire.beacon(LinkWire.QUERY, id, 1, ""))!!.type)
        // Short, other versions and other magic are ignored.
        assertNull(LinkWire.parseBeacon(b.copyOf(20)))
        assertNull(LinkWire.parseBeacon(b.copyOf().also { it[4] = 2 }))
        assertNull(LinkWire.parseBeacon(b.copyOf().also { it[3] = 'X'.code.toByte() }))
        // Only the first n bytes of a receive buffer count.
        assertNotNull(LinkWire.parseBeacon(b + ByteArray(100), b.size))
    }

    @Test fun pairRequestMatchesTheHub() {
        val f = LinkWire.pairRequest(id, "dan-laptop")
        val body = bytes('F'.code, 'R'.code, 'L'.code, 1, 'P'.code, 0) + id + bytes(10) + "dan-laptop".toByteArray()
        assertArrayEquals(bytes(0, body.size) + body, f)
        val (inner, used) = LinkWire.frameAt(f, f.size)!!
        assertEquals(f.size, used)
        val r = LinkWire.parsePairFrame(inner) as LinkWire.PairFrame.Request
        assertArrayEquals(id, r.id)
        assertEquals("dan-laptop", r.name)
        assertNull(LinkWire.frameAt(f, f.size - 1))
        // A refuse frame: "FRL" 1 'R' reason.
        val refused = LinkWire.parsePairFrame(bytes('F'.code, 'R'.code, 'L'.code, 1, 'R'.code, 7)) as LinkWire.PairFrame.Refused
        assertEquals(LinkWire.REFUSE_NOT_PAIRING, refused.reason)
        assertTrue(LinkWire.parsePairFrame(bytes('F'.code, 'R'.code, 'L'.code, 1, 'P'.code, 1) + id + bytes(0)) is LinkWire.PairFrame.Bad)
        assertTrue(LinkWire.parsePairFrame(bytes('F'.code, 'R'.code, 'L'.code, 2, 'P'.code, 0) + id + bytes(0)) is LinkWire.PairFrame.Bad)
        assertTrue(LinkWire.parsePairFrame(bytes('F'.code, 'R'.code, 'L'.code, 1, 'P'.code, 0, 1, 2)) is LinkWire.PairFrame.Bad)
        // Names are cut to 255 bytes on the wire.
        val long = LinkWire.pairRequest(id, "x".repeat(300))
        assertEquals(2 + 6 + 16 + 1 + 255, long.size)
    }

    @Test fun channelEventsDecode() {
        val raw = bytes(4, 0, 3, 1, 3) + "TX".let { bytes(it.length) + it.toByteArray() } + bytes(0) + bytes(9, 8, 7)
        val e = LinkWire.ChannelEvent.decode(raw)!!
        assertEquals(LinkWire.EventKind.DATA, e.kind)
        assertEquals(3, e.channel)
        assertEquals(LinkWire.KIND_INFRARED, e.channelKind)
        assertEquals(3, e.code)
        assertEquals("TX", e.machine)
        assertEquals("", e.device)
        assertArrayEquals(bytes(9, 8, 7), e.data)
        assertNull(LinkWire.ChannelEvent.decode(bytes(4, 0)))
        assertNull(LinkWire.ChannelEvent.decode(bytes(9, 0, 1, 1, 0, 0, 0)))
    }

    @Test fun onlyLocalAddresses() {
        for (a in listOf("127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.1", "192.168.1.20", "169.254.3.4", "::1", "fe80::1", "fd00::5", "::ffff:192.168.1.2"))
            assertTrue(a, LinkWire.allowedPeer(InetAddress.getByName(a)))
        for (a in listOf("8.8.8.8", "172.32.0.1", "172.15.0.1", "192.169.0.1", "2001:db8::1", "::ffff:8.8.8.8"))
            assertFalse(a, LinkWire.allowedPeer(InetAddress.getByName(a)))
        assertFalse(LinkWire.allowedPeer(null))
    }

    @Test fun addressesAndHex() {
        assertEquals("192.168.1.20" to 5035, LinkWire.hostPort("192.168.1.20"))
        assertEquals("192.168.1.20" to 6000, LinkWire.hostPort("192.168.1.20:6000"))
        assertEquals("fe80::1" to 5036, LinkWire.hostPort("[fe80::1]:5036"))
        assertNull(LinkWire.hostPort(""))
        assertEquals("a0a1", LinkWire.hex(bytes(0xa0, 0xa1)))
        assertArrayEquals(id, LinkWire.unhex(LinkWire.hex(id), 16))
        assertNull(LinkWire.unhex("A0A1", 2))
        assertNull(LinkWire.unhex("a0a", 2))
        assertEquals(listOf(1000L, 2000L, 5000L, 10000L, 30000L, 30000L), (0..5).map { LinkWire.backoff(it) })
    }

    @Test fun textsMatchTheDesktop() {
        assertEquals("482 913", LinkWire.showCode("482913"))
        assertEquals("Wrong code", LinkWire.pairResultText("wrongcode", "Pixel"))
        assertEquals("Open Connections on Pixel first.", LinkWire.pairResultText("notpairing", "Pixel"))
        assertEquals("Couldn't reach Pixel.", LinkWire.pairResultText("unreachable", "Pixel"))
        val row = LinkWire.ConnectionRow("id", "Desk", "dan-laptop", "", up = true, rtt = 4, keyChanged = false, forgotten = false, seen = "", channels = "Infrared: TX – E2")
        assertEquals("Connected · 4 ms · Infrared: TX – E2", LinkWire.connectionStatus(row))
        assertEquals("Key changed, pair again", LinkWire.connectionStatus(row.copy(keyChanged = true)))
        assertEquals("Not paired on dan-laptop, pair again", LinkWire.connectionStatus(row.copy(up = false, forgotten = true)))
        assertEquals("Seen at 192.168.1.20", LinkWire.connectionStatus(row.copy(up = false, seen = "192.168.1.20")))
        assertEquals("Not seen", LinkWire.connectionStatus(row.copy(up = false)))
        val ir = LinkWire.IrStatus(state = "joined", peer = "Desk", machine = "Palm TX")
        assertEquals("Infrared: Desk, joined to Palm TX", LinkWire.irText(ir))
        assertEquals("Infrared: Desk, joined to Tungsten E2", LinkWire.irText(ir.copy(machine = "", device = "Tungsten E2")))
        assertEquals("Infrared: Desk, Palm asleep", LinkWire.irText(ir.copy(far = "asleep")))
        assertEquals("Infrared: Desk, Palm paused", LinkWire.irText(ir.copy(far = "paused")))
        assertEquals("Infrared: waiting for a machine on Desk", LinkWire.irText(ir.copy(state = "waiting")))
        assertEquals("Infrared on Desk is in use by Palm TX", LinkWire.irText(ir.copy(state = "busy", busyBy = "Palm TX")))
        assertEquals("Infrared: Desk not found", LinkWire.irText(ir.copy(state = "notfound")))
        assertEquals("Infrared: Desk key changed, pair again", LinkWire.irText(ir.copy(state = "keychanged")))
        assertEquals("Infrared: connecting to Desk", LinkWire.irText(ir.copy(state = "connecting")))
    }

    @Test fun snapshotRowsParse() {
        val state = JSONObject("""{"type":"state","connections":[{"id":"aa","name":"Desk","peerName":"dan-laptop","up":true,"rtt":3,
            "keyChanged":false,"channels":"","seen":"10.0.0.2"},{"id":"bb","name":"Old","keyChanged":true}],"nearby":[]}""")
        val rows = ConnectionsModel.parseRows(state)
        assertEquals(listOf("Desk", "Old"), rows.map { it.name })
        assertEquals("Connected · 3 ms", LinkWire.connectionStatus(rows[0]))
        assertEquals("Key changed, pair again", LinkWire.connectionStatus(rows[1]))
    }
}
