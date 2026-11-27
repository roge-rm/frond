package com.rm.frond

import com.code_intelligence.jazzer.junit.FuzzTest

// The typed addresses NetRelay, UsbLink and the Connections screen take: whatever comes back is a
// usable host and port, and shows as text that reads back the same.
class RelayAddressFuzzTest {
    @FuzzTest
    fun addresses(data: ByteArray) {
        val text = String(data, Charsets.UTF_8)
        NetRelay.port(text)?.let { check(it in 1..65535) }
        val (host, port) = NetRelay.address(text, 5030) ?: return
        check(host.isNotEmpty() && host.none { it.isWhitespace() }) { "host '$host' from '$text'" }
        check(port in 1..65535)
        val shown = NetRelay.hostPort(host, port)
        check(NetRelay.address(shown, 5030) == host to port) { "'$text' shows as '$shown', which reads as ${NetRelay.address(shown, 5030)}" }
    }
}
