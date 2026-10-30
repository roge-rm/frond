package com.rm.frond

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BtLinkTest {
    @Test
    fun addresses() {
        assertEquals("192.168.1.20" to 5040, BtLink.address("192.168.1.20:5040"))
        assertEquals("10.0.2.2" to BtLink.DEFAULT_PORT, BtLink.address("10.0.2.2"))
        assertEquals("fe80::1" to BtLink.DEFAULT_PORT, BtLink.address("[fe80::1]"))
        assertEquals("fe80::1" to 6000, BtLink.address("[fe80::1]:6000"))
        assertNull(BtLink.address("host:0"))
        assertNull(BtLink.address(""))
        // Infrared keeps its own default.
        assertEquals("10.0.2.2" to IrBeam.DEFAULT_PORT, IrBeam.address("10.0.2.2"))
    }

    @Test
    fun modes() {
        for (mode in listOf(BtMode.Off, BtMode.Host(5032), BtMode.Join("10.0.2.2", 5033), BtMode.Join("fe80::1", 5032))) {
            assertEquals(mode, BtMode.decode(mode.encode()))
        }
        assertEquals("off", BtMode.Off.encode())
        assertEquals("host:5032", BtMode.Host(5032).encode())
        assertEquals("join:10.0.2.2:5033", BtMode.Join("10.0.2.2", 5033).encode())
        assertEquals("join:[fe80::1]:5032", BtMode.Join("fe80::1", 5032).encode())
        assertEquals(BtMode.Join("desk", 5032), BtMode.decode("join:desk"))
        assertNull(BtMode.decode(null))
        assertNull(BtMode.decode("host:0"))
        assertNull(BtMode.decode("host:"))
        assertNull(BtMode.decode("join:"))
        assertNull(BtMode.decode("listen:5032"))
    }

    @Test
    fun coreMode() {
        // The core's bluetooth-mode takes off or an absolute Unix socket path.
        assertEquals("off", BtLink.coreMode(null))
        assertEquals("/data/user/0/com.rm.frond/no_backup/bt/bt-server-bredr", BtLink.coreMode(File("/data/user/0/com.rm.frond/no_backup/bt/bt-server-bredr")))
    }
}
