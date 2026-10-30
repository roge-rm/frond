package com.rm.frond

import java.net.InetAddress
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkTest {
    @Test
    fun modesMatchTheCore() {
        // NetworkSimulation's mode names.
        assertEquals(
            setOf("disabled", "connected", "timeout", "disconnected", "internet", "wireless"),
            NetworkMode.entries.map { it.id }.toSet(),
        )
        assertEquals(NetworkMode.WIRELESS, NetworkMode.byId("wireless"))
        assertEquals(NetworkMode.INTERNET, NetworkMode.byId("internet"))
        assertNull(NetworkMode.byId("Unknown network mode."))
        assertNull(NetworkMode.byId(null))
    }

    @Test
    fun onlyTheTxHasNetwork() {
        assertTrue(Device.TX.network)
        assertFalse(Device.E2.network)
    }

    @Test
    fun resolvConfKeepsIpv4ServersInOrder() {
        val servers = listOf("10.0.2.3", "2001:4860:4860::8888", "8.8.4.4").map { InetAddress.getByName(it) }
        assertEquals("nameserver 10.0.2.3\nnameserver 8.8.4.4\n", HostDns.conf(servers))
    }

    @Test
    fun resolvConfWithoutIpv4IsEmpty() {
        assertEquals("", HostDns.conf(emptyList()))
        assertEquals("", HostDns.conf(listOf(InetAddress.getByName("::1"))))
    }
}
