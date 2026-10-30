package com.rm.frond

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.system.Os
import android.util.Log
import java.io.File
import java.net.Inet4Address
import java.net.InetAddress

// DNS for the virtual Wi-Fi router. libslirp forwards the Palm's lookups at 10.0.2.3 to the servers in
// a resolv.conf, which Android doesn't have, so the emulator process writes files/resolv.conf from the
// default network and names it in SLIRP_RESOLV_CONF (read by the libslirp built in build-network.py).
// libslirp checks the file again about once a second, so a network change reaches it without a restart.
object HostDns {
    const val FILE = "resolv.conf"

    // resolv.conf lines for the IPv4 servers; the router only forwards over IPv4.
    fun conf(servers: List<InetAddress>): String =
        servers.filterIsInstance<Inet4Address>().joinToString("") { "nameserver ${it.hostAddress}\n" }

    private var callback: ConnectivityManager.NetworkCallback? = null

    // Before the core starts. Keeps the file up to date until stop.
    fun start(context: Context) {
        val file = File(context.filesDir, FILE)
        Os.setenv("SLIRP_RESOLV_CONF", file.path, true)
        val connectivity = context.getSystemService(ConnectivityManager::class.java) ?: return
        connectivity.activeNetwork?.let { connectivity.getLinkProperties(it) }?.let { write(file, it) }
        if (callback != null) return
        val watch = object : ConnectivityManager.NetworkCallback() {
            override fun onLinkPropertiesChanged(network: Network, properties: LinkProperties) = write(file, properties)
        }
        try {
            connectivity.registerDefaultNetworkCallback(watch)
            callback = watch
        } catch (e: RuntimeException) {
            Log.w(FrondActivity.TAG, "can't follow the network's DNS servers: $e")
        }
    }

    fun stop(context: Context) {
        val watch = callback ?: return
        callback = null
        try {
            context.getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(watch)
        } catch (e: RuntimeException) {
        }
    }

    // Replaced in one step, so libslirp never reads half a file.
    @Synchronized
    private fun write(file: File, properties: LinkProperties) {
        val text = conf(properties.dnsServers)
        if (file.isFile && file.readText() == text) return
        try {
            val part = File(file.parentFile, "$FILE.part")
            part.writeText(text)
            if (!part.renameTo(file)) part.delete()
            Log.i(FrondActivity.TAG, "DNS servers: ${text.lines().filter { it.isNotEmpty() }.joinToString { it.removePrefix("nameserver ") }.ifEmpty { "none" }}")
        } catch (e: java.io.IOException) {
            Log.w(FrondActivity.TAG, "can't write $FILE: $e")
        }
    }
}
