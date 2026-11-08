package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LinkTextTest {
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test fun emojiIsPlainUtf8() {
        // Modified UTF-8 would give six bytes, a surrogate pair of three each.
        assertArrayEquals(bytes(0x44, 0x61, 0x6e, 0x20, 0xf0, 0x9f, 0x8c, 0xb4), LinkText.encode("Dan 🌴"))
        assertEquals("Dan 🌴", LinkText.decode(bytes(0x44, 0x61, 0x6e, 0x20, 0xf0, 0x9f, 0x8c, 0xb4)))
    }

    @Test fun roundTrips() {
        for (s in listOf("", "Frond", "Pixel 9 Pro", "Café", "日本語", "😀👍", "a\u0000b"))
            assertEquals(s, LinkText.decode(LinkText.encode(s)))
        // NUL is a plain zero byte, as the C++ side keeps it.
        assertArrayEquals(bytes(0x61, 0, 0x62), LinkText.encode("a\u0000b"))
        assertEquals("", LinkText.decode(null))
    }

    @Test fun pairPeerSplits() {
        val id = ByteArray(16) { it.toByte() }
        val key = ByteArray(32) { (0x40 + it).toByte() }
        val p = LinkText.peer(id + key + LinkText.encode("Tablet 📱"))!!
        assertArrayEquals(id, p.id)
        assertArrayEquals(key, p.key)
        assertEquals("Tablet 📱", p.name)
        assertEquals("", LinkText.peer(id + key)!!.name)
        assertNull(LinkText.peer(ByteArray(47)))
    }
}
