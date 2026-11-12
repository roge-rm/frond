package com.rm.frond

import android.content.Context
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.UUID

object DanaCardProtocol {
    fun sessionCopyError(states: List<String?>, slots: Int = 2): String? {
        if (states.size != slots || states.any { it == null || (it != "none" && state(it) == null) })
            return if (slots == 2) "Couldn't read both card slots." else "Couldn't read the card slot."
        return if (states.any { it != "none" }) "Android can't copy a session with independent cards yet. Export each card separately." else null
    }
    data class State(val inserted: Boolean, val identity: String, val writeProtected: Boolean, val protocol: String, val failed: Boolean)
    fun state(text: String): State? {
        val words = text.split(' ')
        if (words.size !in 3..5 || words[0] !in listOf("inserted", "ejected") ||
            !words[1].matches(Regex("[0-9a-f]{32}")) || words[2] !in listOf("protected", "writable")) return null
        val flags = words.drop(3)
        if (flags.distinct().size != flags.size || flags.any { it !in listOf("io-error", "mmc") }) return null
        return State(words[0] == "inserted", words[1], words[2] == "protected", if ("mmc" in flags) "mmc" else "sd", "io-error" in flags)
    }
    fun quote(path: String): String {
        require(path.startsWith('/') && path.length <= 4096 && path.none { it == '\n' || it == '\r' || it == '\u0000' })
        return "\"" + path.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
    }
    // Whether a .frondsd header's payload size, a little-endian uint64 at byte 16, is one the family takes.
    fun frondsdFits(head: ByteArray, family: CardFamily): Boolean {
        if (head.size < 24 || !head.copyOf(8).contentEquals("FRONDSD\u0000".toByteArray(Charsets.US_ASCII))) return false
        var payload = 0L
        for (n in 7 downTo 0) payload = (payload shl 8) or (head[16 + n].toLong() and 0xff)
        return family.accepts(payload)
    }
    fun attach(path: String, identity: String, writeProtected: Boolean, family: CardFamily): String {
        require(identity.matches(Regex("[0-9a-f]{32}")))
        require(family.family in listOf("sdsc", "legacy-mmc-spi"))
        return "${quote(path)} $identity ${if (writeProtected) "protected" else "writable"} ${family.protocol}"
    }
}

class DanaCards(private val context: Context, private val link: ControlLink, val device: Device) {
    data class SlotState(val slot: StorageSlot, val known: Boolean = false, val card: DanaCardProtocol.State? = null)
    data class Entry(val id: String, val name: String, val family: String, val path: File)
    var states by mutableStateOf(device.storageSlots.map { SlotState(it) })
        private set
    var entries by mutableStateOf(emptyList<Entry>())
        private set
    private val root get() = File(context.filesDir, "dana-cards").also { it.mkdirs() }
    fun refresh() {
        states = device.storageSlots.map { slot ->
            val reply = link.request(slot.commandPrefix + "state")
            val state = if (reply.ok) DanaCardProtocol.state(reply.message) else null
            SlotState(slot, reply.ok && (reply.message == "none" || state != null), state)
        }
        entries = root.listFiles().orEmpty().filter { it.extension == "json" }.mapNotNull { file ->
            try {
                val meta = JSONObject(file.readText())
                val id = file.nameWithoutExtension
                if (!id.matches(Regex("[0-9a-f]{32}"))) null else {
                    val path = File(root, "$id.frondsd")
                    if (!path.isFile) null else Entry(id, meta.getString("name"), meta.getString("family"), path)
                }
            } catch (_: Exception) { null }
        }.sortedBy { it.name.lowercase() }
    }
    private fun request(slot: StorageSlot, operation: String, argument: String = "") {
        val reply = link.request(slot.commandPrefix + operation, argument)
        if (!reply.ok) throw IOException(reply.message)
    }
    fun command(slot: StorageSlot, operation: String) {
        require(operation in listOf("eject", "insert", "take-out"))
        request(slot, operation)
    }
    fun attach(slot: StorageSlot, entry: Entry, writeProtected: Boolean) {
        val family = slot.families.firstOrNull { it.family == entry.family } ?: throw IOException("This slot doesn't take that card family.")
        request(slot, "attach", DanaCardProtocol.attach(entry.path.absolutePath, entry.id, writeProtected, family))
    }
    fun make(slot: StorageSlot, family: CardFamily, size: Int, name: String, writeProtected: Boolean) {
        require(family in slot.families && family.accepts(size.toLong() shl 20))
        val id = UUID.randomUUID().toString().replace("-", "")
        val path = File(root, "$id.frondsd")
        val reply = CardTool.run(arrayOf("make", "sd", size.toString(), path.path), null, null)
        val meta = File(root, "$id.json")
        try {
            if (!reply.startsWith("ok ")) throw IOException(reply)
            meta.writeText(JSONObject().put("name", name).put("family", family.family).toString())
            attach(slot, Entry(id, name, family.family, path), writeProtected)
        } catch (e: Exception) {
            meta.delete(); path.delete()
            throw e
        }
    }
    fun import(slot: StorageSlot, family: CardFamily, uri: Uri, writeProtected: Boolean) {
        require(family in slot.families)
        val id = UUID.randomUUID().toString().replace("-", "")
        val path = File(root, "$id.frondsd")
        val raw = File(root, "$id.input")
        val meta = File(root, "$id.json")
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Couldn't read the card.")
            input.use { if (!BoundedCardImage.copy(it, raw, family.maxBytes + (1L shl 20))) throw IOException("That image is too large.") }
            val head = ByteArray(24)
            raw.inputStream().use { it.read(head) }
            if (head.copyOf(8).contentEquals("FRONDSD\u0000".toByteArray(Charsets.US_ASCII))) {
                if (!DanaCardProtocol.frondsdFits(head, family)) throw IOException("That size isn't supported by this card family.")
                if (!raw.renameTo(path)) throw IOException("Couldn't keep the card.")
            } else {
                if (!family.accepts(raw.length())) throw IOException("That size isn't supported by this card family.")
                val result = CardTool.run(arrayOf("import", raw.path, "sd", path.path), null, null)
                if (!result.startsWith("ok ")) throw IOException(result)
            }
            val name = "${family.label} ${id.take(8)}"
            meta.writeText(JSONObject().put("name", name).put("family", family.family).toString())
            attach(slot, Entry(id, name, family.family, path), writeProtected)
        } catch (e: Exception) {
            // A card the first attach refuses would stay in the library and never attach.
            meta.delete(); path.delete()
            throw e
        } finally { raw.delete() }
    }
    fun export(slot: StorageSlot, uri: Uri) {
        val copy = File(context.cacheDir, "dana-card-${UUID.randomUUID()}.frondsd")
        try {
            request(slot, "export", DanaCardProtocol.quote(copy.absolutePath))
            val output = context.contentResolver.openOutputStream(uri, "wt") ?: throw IOException("Couldn't write the card.")
            output.use { copy.inputStream().use { input -> input.copyTo(it) } }
        } catch (e: Exception) {
            try { android.provider.DocumentsContract.deleteDocument(context.contentResolver, uri) } catch (_: Exception) {}
            throw e
        } finally { copy.delete() }
    }
}
