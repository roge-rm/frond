package com.rm.frond

import com.code_intelligence.jazzer.junit.FuzzTest
import java.io.File
import java.nio.file.Files

// Fuzz scratch goes in /dev/shm where there is one: LinkStore fsyncs every save, which crawls on a
// busy disk (Gradle points java.io.tmpdir at build/).
internal fun fuzzTempDir(prefix: String): File {
    val shm = File("/dev/shm")
    return if (shm.isDirectory && shm.canWrite()) Files.createTempDirectory(shm.toPath(), prefix).toFile()
        else Files.createTempDirectory(prefix).toFile()
}

// identity.json and connections.json as LinkStore reads them: the peers it keeps are usable and
// come back the same after a save.
class LinkStoreFuzzTest {
    private object FakeKeys : LinkStore.Keys {
        override fun newSecret() = ByteArray(32) { 1 }
        override fun randomId() = ByteArray(16) { 2 }
    }

    private fun <T> inFolder(work: (File) -> T): T {
        val root = fuzzTempDir("frond-linkstore-fuzz")
        try {
            return work(File(root, "connections").also { it.mkdirs() })
        } finally {
            root.deleteRecursively()
        }
    }

    @FuzzTest
    fun identity(data: ByteArray) = inFolder { folder ->
        File(folder, "identity.json").writeBytes(data)
        val i = LinkStore.readIdentity(folder) ?: return@inFolder
        check(i.id.size == 16 && i.secret.size == 32)
        check(i.port in 0..65535) { "identity port ${i.port}" }
    }

    @FuzzTest
    fun peers(data: ByteArray) = inFolder { folder ->
        File(folder, "connections.json").writeBytes(data)
        val peers = LinkStore.readPeers(folder)
        for (p in peers) {
            check(LinkWire.unhex(p.id, 16) != null && p.key.size == 32)
            check(p.bluetoothRadio == "here" || p.bluetoothRadio == "there")
            check(p.lastPort in 0..65535) { "lastPort ${p.lastPort}" }
        }
        val store = LinkStore(folder, FakeKeys)
        store.peers = peers
        check(store.save())
        val again = LinkStore.readPeers(folder)
        check(again == peers) { "peers changed on a save: $peers then $again" }
        store.pairedTable().let { check(it.size % 48 == 0) }
    }
}
