package com.rm.frond

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
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
}
