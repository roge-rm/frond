package com.rm.frond

import java.net.InetSocketAddress
import java.net.Socket
import java.nio.channels.ServerSocketChannel
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import com.rm.frond.ui.relayText

class NetRelayTest {
    @Test
    fun defaultPorts() {
        assertEquals(5034, UsbLink.defaultPort)
        assertEquals(UsbLink.DEFAULT_PORT, UsbLink.defaultPort)
        // Connections use 5035.
        assertEquals(setOf(5034, 5035), setOf(UsbLink.defaultPort, LinkWire.DEFAULT_PORT))
        // gdb's usual remote port.
        assertEquals(3333, DebugLink.defaultPort)
        assertEquals(DebugLink.DEFAULT_PORT, DebugLink.defaultPort)
    }

    @Test
    fun listenersBindLoopbackUnlessNetworkIsEnabled() {
        val haveIpv6 = runCatching {
            ServerSocketChannel.open().use { it.bind(InetSocketAddress(TcpListeners.ipv6Loopback, 0)) }
        }.isSuccess
        val local = TcpListeners.open(0, BeamMode.Listen(5034).allowNetwork, 4)
        try {
            val expected = listOf(TcpListeners.ipv4Loopback) +
                if (haveIpv6) listOf(TcpListeners.ipv6Loopback) else emptyList()
            assertEquals(expected, local.map { it.socket().inetAddress })
            val port = local.first().socket().localPort
            assertTrue(local.all { it.socket().localPort == port })
            for (listener in local) {
                Socket().use { client ->
                    client.connect(listener.localAddress, 1000)
                    listener.accept().use { assertTrue(it.isConnected) }
                }
            }
        } finally {
            local.forEach { it.close() }
        }
        val network = TcpListeners.open(0, BeamMode.Listen(5034, allowNetwork = true).allowNetwork, 4).single()
        network.use { assertTrue(it.socket().inetAddress.isAnyLocalAddress) }
    }

    @Test
    fun debuggerReleaseConnectsOnce() {
        // The core's gdb port opened at launch never leaves while the core runs.
        assertNull(DebugLink.coreLeftReason("", 40123))
        val loopback = java.net.InetAddress.getByName("127.0.0.1")
        DebugLink.reservePort()!!.use { reserved ->
            val port = reserved.localPort
            assertTrue(port in 1..65535)
            // Nothing listening yet: the caller tries again.
            assertFalse(DebugLink.release(port))
            // Another bind without SO_REUSEADDR can't take the held port.
            assertTrue(runCatching { java.net.Socket().use { it.bind(java.net.InetSocketAddress(loopback, port)) } }.isFailure)
            // The core binds with SO_REUSEADDR and listens on it while it's held.
            java.net.ServerSocket().use { core ->
                core.reuseAddress = true
                core.bind(java.net.InetSocketAddress(loopback, port), 1)
                assertTrue(DebugLink.release(port))
                // The core sees a gdb that connected and closed at once.
                core.accept().use { assertEquals(-1, it.getInputStream().read()) }
            }
        }
    }

    @Test
    fun addressesTakeEachDefault() {
        assertEquals("10.0.2.2" to 5034, UsbLink.address(" 10.0.2.2 "))
        assertEquals("fe80::1" to 5034, UsbLink.address("[fe80::1]"))
        assertEquals("desk.local" to 6000, UsbLink.address("desk.local:6000"))
        assertNull(UsbLink.address("host:"))
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
        // The old listen:PORT took network peers, so an upgraded install keeps taking them.
        assertEquals(BeamMode.Listen(5033, allowNetwork = true), BeamMode.decode("listen:5033"))
        assertEquals(BeamMode.Listen(5033), BeamMode.decode(BeamMode.Listen(5033).encode()))
        assertEquals("listen-local:5033", BeamMode.Listen(5033).encode())
        val network = BeamMode.Listen(5033, allowNetwork = true)
        assertEquals(network, BeamMode.decode(network.encode()))
        assertTrue(relayText(NetRelay.Status(BeamMode.Listen(5033), NetRelay.Phase.WAITING, listenAddresses = "127.0.0.1:5033")).endsWith(", network connections are off"))
        assertEquals("Waiting for a peer at 10.0.0.2:5033", relayText(NetRelay.Status(network, NetRelay.Phase.WAITING, listenAddresses = "10.0.0.2:5033")))
        assertFalse(Settings().debuggerNetwork)
        assertFalse(Settings().linkNetwork)
    }

    @Test
    fun linkNetworkStaysOnForPairedInstalls() {
        val dir = java.nio.file.Files.createTempDirectory("frond-links").toFile()
        try {
            assertFalse(Settings.legacyLinkNetwork(dir))
            val folder = LinkStore.folder(dir)
            folder.mkdirs()
            val entry = { paired: Boolean ->
                """{"version":1,"entries":[{"id":"${"ab".repeat(16)}","key":"${"cd".repeat(32)}","name":"Desk","paired":$paired}]}"""
            }
            java.io.File(folder, "connections.json").writeText(entry(false))
            assertFalse(Settings.legacyLinkNetwork(dir))
            java.io.File(folder, "connections.json").writeText(entry(true))
            assertTrue(Settings.legacyLinkNetwork(dir))
        } finally {
            dir.deleteRecursively()
        }
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
