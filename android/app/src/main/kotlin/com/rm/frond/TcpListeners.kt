package com.rm.frond

import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.StandardSocketOptions
import java.nio.channels.ServerSocketChannel

internal object TcpListeners {
    val ipv4Loopback: InetAddress = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))
    val ipv6Loopback: InetAddress = InetAddress.getByAddress(ByteArray(16).apply { this[15] = 1 })

    fun open(port: Int, allowNetwork: Boolean, backlog: Int, fallbackPort: Boolean = false): List<ServerSocketChannel> {
        val address = if (allowNetwork) null else ipv4Loopback
        val primary = try {
            bind(address, port, backlog)
        } catch (e: IOException) {
            if (!fallbackPort || port == 0) throw e
            bind(address, 0, backlog)
        }
        if (allowNetwork) return listOf(primary)
        // IPv6 may be disabled on this host.
        val ipv6 = try {
            bind(ipv6Loopback, primary.socket().localPort, backlog)
        } catch (_: IOException) {
            return listOf(primary)
        }
        return listOf(primary, ipv6)
    }

    private fun bind(address: InetAddress?, port: Int, backlog: Int): ServerSocketChannel {
        val listener = ServerSocketChannel.open()
        try {
            listener.setOption(StandardSocketOptions.SO_REUSEADDR, true)
            listener.bind(if (address == null) InetSocketAddress(port) else InetSocketAddress(address, port), backlog)
            return listener
        } catch (e: Exception) {
            listener.close()
            throw e
        }
    }

    fun addresses(listeners: List<ServerSocketChannel>): String = listeners.joinToString(", ") { listener ->
        val socket = listener.socket()
        val host = when {
            socket.inetAddress.isAnyLocalAddress -> if (socket.inetAddress.address.size == 16) "::" else "0.0.0.0"
            socket.inetAddress == ipv6Loopback -> "::1"
            else -> socket.inetAddress.hostAddress.orEmpty()
        }
        NetRelay.hostPort(host, socket.localPort)
    }
}
