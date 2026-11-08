package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class RelayAddressTest {
    @Test
    fun ports() {
        assertEquals(5030, NetRelay.port("5030"))
        assertEquals(1, NetRelay.port(" 1 "))
        assertEquals(65535, NetRelay.port("65535"))
        assertNull(NetRelay.port("0"))
        assertNull(NetRelay.port("65536"))
        assertNull(NetRelay.port(""))
        assertNull(NetRelay.port("port"))
        assertNull(NetRelay.port("-1"))
    }

    @Test
    fun addresses() {
        assertEquals("192.168.1.20" to 5031, SerialLink.address("192.168.1.20:5031"))
        assertEquals("10.0.2.2" to SerialLink.DEFAULT_PORT, SerialLink.address(" 10.0.2.2 "))
        assertEquals("desk.local" to 6000, SerialLink.address("desk.local:6000"))
        assertEquals("fe80::1" to 5032, SerialLink.address("[fe80::1]:5032"))
        assertEquals("fe80::1" to SerialLink.DEFAULT_PORT, SerialLink.address("[fe80::1]"))
        assertEquals("fe80::1" to SerialLink.DEFAULT_PORT, SerialLink.address("fe80::1"))
        assertNull(SerialLink.address(""))
        assertNull(SerialLink.address(":5030"))
        assertNull(SerialLink.address("host:"))
        assertNull(SerialLink.address("host:70000"))
        assertNull(SerialLink.address("a host:5030"))
        assertNull(SerialLink.address("[]:5030"))
        assertNull(SerialLink.address("[fe80::1]5030"))
    }

    @Test
    fun hostPort() {
        assertEquals("10.0.2.2:5030", NetRelay.hostPort("10.0.2.2", 5030))
        assertEquals("[fe80::1]:5030", NetRelay.hostPort("fe80::1", 5030))
    }

    @Test
    fun modes() {
        for (mode in listOf(BeamMode.Off, BeamMode.Listen(5030), BeamMode.Connect("10.0.2.2", 5031), BeamMode.Connect("fe80::1", 5032))) {
            assertEquals(mode, BeamMode.decode(mode.encode()))
        }
        assertEquals("off", BeamMode.Off.encode())
        assertEquals("listen:5030", BeamMode.Listen(5030).encode())
        assertEquals("connect:10.0.2.2:5031", BeamMode.Connect("10.0.2.2", 5031).encode())
        assertNull(BeamMode.decode(null))
        assertNull(BeamMode.decode("listen:0"))
        assertNull(BeamMode.decode("tcp:5030"))
    }

    @Test
    fun coreMode() {
        // HostSerial::Parse takes off or tcp:PORT.
        assertEquals("off", NetRelay.coreMode(null))
        assertEquals("tcp:40123", NetRelay.coreMode(40123))
    }
}
