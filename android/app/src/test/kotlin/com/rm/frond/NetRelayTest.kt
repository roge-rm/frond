package com.rm.frond

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NetRelayTest {
    @Test
    fun defaultPorts() {
        assertEquals(5030, IrBeam.defaultPort)
        assertEquals(IrBeam.DEFAULT_PORT, IrBeam.defaultPort)
        assertEquals(5033, SerialLink.defaultPort)
        assertEquals(SerialLink.DEFAULT_PORT, SerialLink.defaultPort)
        assertEquals(5034, UsbLink.defaultPort)
        assertEquals(UsbLink.DEFAULT_PORT, UsbLink.defaultPort)
        // Bluetooth has its own relay on 5032.
        assertEquals(setOf(5030, 5032, 5033, 5034), setOf(IrBeam.defaultPort, BtLink.DEFAULT_PORT, SerialLink.defaultPort, UsbLink.defaultPort))
        // gdb's usual remote port.
        assertEquals(3333, DebugLink.defaultPort)
        assertEquals(DebugLink.DEFAULT_PORT, DebugLink.defaultPort)
    }

    @Test
    fun debuggerReleaseConnectsOnce() {
        // The core's gdb port opened at launch never leaves while the core runs.
        assertNull(DebugLink.coreLeftReason("", 40123))
        val port = DebugLink.freePort()!!
        assertTrue(port in 1..65535)
        // Nothing listening yet: the caller tries again.
        assertFalse(DebugLink.release(port))
        java.net.ServerSocket(port, 1, java.net.InetAddress.getByName("127.0.0.1")).use { core ->
            assertTrue(DebugLink.release(port))
            // The core sees a gdb that connected and closed at once.
            core.accept().use { assertEquals(-1, it.getInputStream().read()) }
        }
    }

    @Test
    fun addressesTakeEachDefault() {
        assertEquals("10.0.2.2" to 5033, SerialLink.address("10.0.2.2"))
        assertEquals("10.0.2.2" to 5034, UsbLink.address(" 10.0.2.2 "))
        assertEquals("fe80::1" to 5034, UsbLink.address("[fe80::1]"))
        assertEquals("desk.local" to 6000, SerialLink.address("desk.local:6000"))
        assertNull(UsbLink.address("host:"))
    }

    @Test
    fun serialState() {
        // DesktopControl::SerialState and HostSerial::Describe.
        assertNull(SerialLink.coreLeftReason("tcp 127.0.0.1:40123 waiting", 40123))
        assertNull(SerialLink.coreLeftReason("tcp 127.0.0.1:40123 connected", 40123))
        assertEquals("The emulator's serial port stopped.", SerialLink.coreLeftReason("off", 40123))
        assertEquals("Could not listen.", SerialLink.coreLeftReason("off Could not listen.", 40123))
        assertEquals("The emulator's serial port stopped.", SerialLink.coreLeftReason("tcp 127.0.0.1:40124 waiting", 40123))
        assertEquals("The emulator's serial port stopped.", SerialLink.coreLeftReason("pty /dev/pts/3 connected", 40123))
    }

    @Test
    fun usbState() {
        // DesktopControl::UsbState and UsbHostBridge::Describe.
        assertNull(UsbLink.coreLeftReason("tcp 127.0.0.1:40123 waiting detached", 40123))
        assertNull(UsbLink.coreLeftReason("tcp 127.0.0.1:40123 connected running", 40123))
        assertEquals("The emulator's USB stopped.", UsbLink.coreLeftReason("off", 40123))
        assertEquals("The emulator's USB stopped.", UsbLink.coreLeftReason("tcp 127.0.0.1:401234 waiting detached", 40123))
    }

    @Test
    fun modesKeepTheirPort() {
        // Saved modes carry their port, whichever relay reads them.
        assertEquals(BeamMode.Connect("10.0.2.2", 5034), BeamMode.decode(BeamMode.Connect("10.0.2.2", 5034).encode()))
        assertEquals(BeamMode.Listen(5033), BeamMode.decode("listen:5033"))
    }

    @Test
    fun heldBytesKeepOrderAndCap() {
        val held = HeldBytes(10)
        assertTrue(held.offer(byteArrayOf(1, 2, 3, 9), 3))
        assertTrue(held.offer(byteArrayOf(4, 5, 6, 7, 8, 9, 10), 7))
        // An eleventh byte would pass the cap, so nothing of it is kept.
        assertFalse(held.offer(byteArrayOf(11), 1))
        assertEquals(10, held.length)
        assertArrayEquals(byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), held.drain())
        assertEquals(0, held.length)
        assertTrue(held.offer(byteArrayOf(42), 1))
        held.clear()
        assertArrayEquals(ByteArray(0), held.drain())
        assertEquals(64 * 1024, NetRelay.HELD_MAX)
    }
}
