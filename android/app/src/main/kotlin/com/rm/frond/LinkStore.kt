package com.rm.frond

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.channels.FileLock
import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermissions

// Connections' store in <files>/connections, in the desktop's format: identity.json (this install's id,
// name, X25519 secret and TCP port) and connections.json v1 (paired peers with pinned keys). Both 0600.
// Only the process running the link engine writes them, while it holds connections/.lock through a
// FileChannel lock; anyone may read them, as files are replaced whole.
class LinkStore(val folder: File, private val keys: Keys = NativeKeys) {
    interface Keys {
        fun newSecret(): ByteArray
        fun randomId(): ByteArray
    }

    object NativeKeys : Keys {
        override fun newSecret() = LinkNative.newSecret()
        override fun randomId() = LinkNative.randomBytes(16)
    }

    data class Identity(val id: ByteArray, val name: String, val secret: ByteArray, val port: Int) {
        val hex get() = LinkWire.hex(id)
    }

    data class Peer(
        val id: String,
        // Shown here, renameable.
        val name: String,
        // What the peer calls itself.
        val peerName: String,
        val key: ByteArray,
        // Typed in Address… or Add by address, else empty.
        val address: String = "",
        // Where a handshake we dialled last succeeded. Beacons never change it.
        val lastHost: String = "",
        val lastPort: Int = 0,
        val paired: Boolean = true,
        // ms since the epoch.
        val lastSeen: Long = 0,
        // Whose radio Bluetooth uses with this peer, here or there, and when someone chose it (ms since the
        // epoch, 0 if nobody has: then the side with the lower install id holds it).
        val bluetoothRadio: String = "here",
        val bluetoothRadioAt: Long = 0,
        val keyChanged: Boolean = false,
    ) {
        override fun equals(other: Any?) = other is Peer && id == other.id && name == other.name && peerName == other.peerName &&
            key.contentEquals(other.key) && address == other.address && lastHost == other.lastHost && lastPort == other.lastPort &&
            paired == other.paired && lastSeen == other.lastSeen && bluetoothRadio == other.bluetoothRadio &&
            bluetoothRadioAt == other.bluetoothRadioAt && keyChanged == other.keyChanged
        override fun hashCode() = id.hashCode()
    }

    private var lockFile: RandomAccessFile? = null
    private var lock: FileLock? = null
    lateinit var identity: Identity
        private set
    var peers: List<Peer> = emptyList()

    val locked get() = lock != null

    // Takes the lock, waiting up to timeoutMs for another process to let it go, then loads or makes the
    // identity. Returns an error, or null.
    fun open(defaultName: String, timeoutMs: Long = 3000): String? {
        if (!folder.isDirectory && !folder.mkdirs()) return "Couldn't make ${folder.path}."
        private(folder, "rwx------")
        val file = RandomAccessFile(File(folder, ".lock"), "rw")
        val until = System.nanoTime() + timeoutMs * 1_000_000
        var held: FileLock? = null
        while (held == null) {
            held = try { file.channel.tryLock() } catch (e: IOException) { null } catch (e: java.nio.channels.OverlappingFileLockException) { null }
            if (held == null) {
                if (System.nanoTime() > until) {
                    file.close()
                    return "Connections are in use by another part of Frond."
                }
                Thread.sleep(50)
            }
        }
        lockFile = file
        lock = held
        identity = readIdentity(folder) ?: run {
            val bad = File(folder, "identity.json")
            if (bad.exists() && !bad.renameTo(File(folder, "identity.json.bad"))) bad.delete()
            Identity(keys.randomId(), defaultName.ifBlank { "Frond" }.take(64), keys.newSecret(), LinkWire.DEFAULT_PORT).also {
                if (!saveIdentity(it)) { close(); return "Couldn't save the identity." }
            }
        }
        peers = readPeers(folder)
        return null
    }

    fun close() {
        try { lock?.release() } catch (_: IOException) {}
        try { lockFile?.close() } catch (_: IOException) {}
        lock = null
        lockFile = null
    }

    fun setName(name: String) {
        identity = identity.copy(name = name)
        saveIdentity(identity)
    }

    fun find(hex: String) = peers.firstOrNull { it.id == hex }

    fun update(hex: String, change: (Peer) -> Peer) {
        peers = peers.map { if (it.id == hex) change(it) else it }
    }

    fun put(peer: Peer) {
        peers = if (find(peer.id) != null) peers.map { if (it.id == peer.id) peer else it } else peers + peer
    }

    fun remove(hex: String) {
        peers = peers.filter { it.id != hex }
    }

    // The pinned table for LinkNative.linkAnswer: 48 bytes per paired peer, id then key.
    fun pairedTable(): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        for (p in peers) if (p.paired) {
            val id = LinkWire.unhex(p.id, 16) ?: continue
            if (p.key.size != 32) continue
            out.write(id)
            out.write(p.key)
        }
        return out.toByteArray()
    }

    private fun saveIdentity(i: Identity) = writePrivate(File(folder, "identity.json"), JSONObject()
        .put("version", 1).put("id", i.hex).put("name", i.name).put("secret", LinkWire.hex(i.secret)).put("port", i.port))

    fun save(): Boolean {
        val entries = JSONArray()
        for (p in peers) entries.put(JSONObject().put("id", p.id).put("name", p.name).put("peerName", p.peerName)
            .put("key", LinkWire.hex(p.key)).put("address", p.address).put("manual", p.address.isNotEmpty())
            .put("paired", p.paired).put("lastSeen", p.lastSeen).put("bluetoothRadio", p.bluetoothRadio)
            .put("bluetoothRadioAt", p.bluetoothRadioAt).put("keyChanged", p.keyChanged).put("lastHost", p.lastHost).put("lastPort", p.lastPort))
        return writePrivate(File(folder, "connections.json"), JSONObject().put("version", 1).put("entries", entries))
    }

    companion object {
        fun folder(filesDir: File) = File(filesDir, "connections")

        private fun private(file: File, mode: String) {
            try { Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString(mode)) } catch (_: Exception) {}
        }

        // Written to a temporary file at 0600 and renamed over the old one.
        fun writePrivate(file: File, json: JSONObject): Boolean {
            val temporary = File(file.path + ".tmp")
            return try {
                temporary.delete()
                temporary.createNewFile()
                private(temporary, "rw-------")
                temporary.outputStream().use { out ->
                    out.write(json.toString(2).toByteArray(Charsets.UTF_8))
                    out.fd.sync()
                }
                temporary.renameTo(file)
            } catch (e: IOException) {
                false
            } finally {
                temporary.delete()
            }
        }

        fun readIdentity(folder: File): Identity? = try {
            val o = JSONObject(File(folder, "identity.json").readText())
            val id = LinkWire.unhex(o.optString("id"), 16)
            val secret = LinkWire.unhex(o.optString("secret"), 32)
            if (o.optInt("version") != 1 || id == null || secret == null) null
            else Identity(id, o.optString("name"), secret, o.optInt("port", LinkWire.DEFAULT_PORT))
        } catch (e: Exception) {
            null
        }

        // Read without the lock, as the pickers do.
        fun readPeers(folder: File): List<Peer> {
            val entries = try {
                JSONObject(File(folder, "connections.json").readText()).optJSONArray("entries")
            } catch (e: Exception) {
                null
            } ?: return emptyList()
            val list = mutableListOf<Peer>()
            for (i in 0 until entries.length()) {
                val e = entries.optJSONObject(i) ?: continue
                val id = e.optString("id")
                val key = LinkWire.unhex(e.optString("key"), 32)
                if (LinkWire.unhex(id, 16) == null || key == null) continue
                val peerName = e.optString("peerName")
                list += Peer(
                    id = id, name = e.optString("name").ifEmpty { peerName }, peerName = peerName, key = key,
                    address = e.optString("address"), lastHost = e.optString("lastHost"), lastPort = e.optInt("lastPort"),
                    paired = e.optBoolean("paired", true), lastSeen = e.optLong("lastSeen"),
                    bluetoothRadio = if (e.optString("bluetoothRadio", "here") == "there") "there" else "here",
                    bluetoothRadioAt = e.optDouble("bluetoothRadioAt", 0.0).let { if (it.isNaN()) 0L else it.toLong() }, keyChanged = e.optBoolean("keyChanged", false),
                )
            }
            return list
        }

        // The paired connections by id and name, for the pickers.
        fun choices(folder: File): List<Pair<String, String>> = readPeers(folder).filter { it.paired }.map { it.id to it.name }
    }
}
