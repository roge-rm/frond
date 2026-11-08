package com.rm.frond

import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermissions

class LinkStoreTest {
    @get:Rule val tmp = TemporaryFolder()

    private object FakeKeys : LinkStore.Keys {
        var n = 0
        override fun newSecret() = ByteArray(32) { (it + 1).toByte() }
        override fun randomId() = ByteArray(16) { (it + 0x10 + n++).toByte() }
    }

    private fun folder() = File(tmp.root, "connections")
    private fun mode(f: File) = PosixFilePermissions.toString(Files.getPosixFilePermissions(f.toPath()))
    private fun key(b: Int) = ByteArray(32) { b.toByte() }
    private fun id(b: Int) = LinkWire.hex(ByteArray(16) { b.toByte() })

    @Test fun makesAPrivateIdentityOnceAndKeepsIt() {
        val a = LinkStore(folder(), FakeKeys)
        assertNull(a.open("Pixel 8"))
        val first = a.identity
        assertEquals("Pixel 8", first.name)
        assertEquals(LinkWire.DEFAULT_PORT, first.port)
        assertEquals("rwx------", mode(folder()))
        assertEquals("rw-------", mode(File(folder(), "identity.json")))
        val saved = JSONObject(File(folder(), "identity.json").readText())
        assertEquals(1, saved.getInt("version"))
        assertEquals(first.hex, saved.getString("id"))
        assertEquals(LinkWire.hex(first.secret), saved.getString("secret"))
        a.setName("Dan's phone")
        a.close()
        val b = LinkStore(folder(), FakeKeys)
        assertNull(b.open("Other"))
        assertArrayEquals(first.id, b.identity.id)
        assertArrayEquals(first.secret, b.identity.secret)
        assertEquals("Dan's phone", b.identity.name)
        b.close()
    }

    @Test fun aDamagedIdentityIsSetAsideAndRemade() {
        folder().mkdirs()
        File(folder(), "identity.json").writeText("{\"version\":1,\"id\":\"zz\"}")
        val s = LinkStore(folder(), FakeKeys)
        assertNull(s.open("Pixel"))
        assertTrue(File(folder(), "identity.json.bad").isFile)
        assertNotNull(LinkStore.readIdentity(folder()))
        s.close()
    }

    @Test fun onlyOneHolderOfTheLockAtOnce() {
        val a = LinkStore(folder(), FakeKeys)
        assertNull(a.open("A"))
        assertTrue(a.locked)
        // Same process, another channel: refused after the wait.
        val b = LinkStore(folder(), FakeKeys)
        assertEquals("Connections are in use by another part of Frond.", b.open("B", timeoutMs = 100))
        assertFalse(b.locked)
        a.close()
        assertNull(b.open("B", timeoutMs = 100))
        b.close()
        // The lock file stays, so a second process can't lock an unlinked inode.
        assertTrue(File(folder(), ".lock").exists())
        RandomAccessFile(File(folder(), ".lock"), "rw").use { assertNotNull(it.channel.tryLock()) }
    }

    @Test fun peersRoundTripInTheDesktopFormat() {
        val s = LinkStore(folder(), FakeKeys)
        assertNull(s.open("Phone"))
        s.put(LinkStore.Peer(id(1), "Desk", "dan-laptop", key(7), address = "192.168.1.20", lastHost = "192.168.1.20", lastPort = 5035, lastSeen = 42))
        s.put(LinkStore.Peer(id(2), "Old", "old", key(8), paired = false))
        assertTrue(s.save())
        assertEquals("rw-------", mode(File(folder(), "connections.json")))
        val o = JSONObject(File(folder(), "connections.json").readText())
        assertEquals(1, o.getInt("version"))
        val e = o.getJSONArray("entries").getJSONObject(0)
        assertEquals(id(1), e.getString("id"))
        assertEquals(LinkWire.hex(key(7)), e.getString("key"))
        assertTrue(e.getBoolean("manual"))
        assertEquals("192.168.1.20", e.getString("lastHost"))
        assertEquals(5035, e.getInt("lastPort"))
        assertEquals("here", e.getString("bluetoothRadio"))
        val back = LinkStore.readPeers(folder())
        assertEquals(s.peers, back)
        // The pickers list paired ones only.
        assertEquals(listOf(id(1) to "Desk"), LinkStore.choices(folder()))
        // The pinned table for the listener: id then key, paired only.
        assertArrayEquals(LinkWire.unhex(id(1), 16)!! + key(7), s.pairedTable())
        s.update(id(1)) { it.copy(name = "Desktop") }
        s.remove(id(2))
        assertTrue(s.save())
        assertEquals(listOf("Desktop"), LinkStore.readPeers(folder()).map { it.name })
        s.close()
    }

    @Test fun readsWhatTheDesktopHubWrites() {
        folder().mkdirs()
        File(folder(), "connections.json").writeText(JSONObject().put("version", 1).put("entries", org.json.JSONArray()
            .put(JSONObject().put("id", id(3)).put("name", "").put("peerName", "Pixel").put("key", LinkWire.hex(key(9))))
            .put(JSONObject().put("id", "bad").put("key", LinkWire.hex(key(9))))
            .put(JSONObject().put("id", id(4)).put("key", "short"))).toString())
        val peers = LinkStore.readPeers(folder())
        assertEquals(1, peers.size)
        // No name of its own: it goes by what the peer calls itself.
        assertEquals("Pixel", peers[0].name)
        assertTrue(peers[0].paired)
        assertEquals(0, peers[0].lastPort)
        assertEquals(emptyList<LinkStore.Peer>(), LinkStore.readPeers(File(tmp.root, "none")))
    }
}
