package com.rm.frond

import android.content.ContentResolver
import android.net.Uri
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.util.Arrays

object ZaurusImages {
    const val CF = "microdrive.img"
    const val NAND = "zaurus-nand.bin"
    const val PROM = "zaurus-prom.bin"
    const val CF_ID = "microdrive.id"
    const val CF_COMMIT = "microdrive.commit"
    private const val CF_JOURNAL_PREFIX = "microdrive.journal-"
    private val cfToken = Regex("[0-9a-f]{32}")
    const val EXTERNAL_CF = "cf.frondcf"
    const val CF_SLOT = "cf-slot.json"
    const val MAX_EXTERNAL_CF_BYTES = 2L shl 30
    val companions = listOf(NAND, CF, PROM, CF_ID, CF_COMMIT, EXTERNAL_CF, CF_SLOT)

    private const val RAW_NAND_BYTES = 1024L * 32 * 528
    private const val DBK_BYTES = RAW_NAND_BYTES + 1025 * 16
    private const val PROM_BYTES = 8L shl 20
    private const val MAX_CF_BYTES = (1L shl 28) * 512
    private const val NAND_ERROR = "Choose an SL-C3000 SYSTC300.DBK or a raw NAND image with spare bytes."
    private const val PROM_ERROR = "Choose an 8 MiB SL-C3000 CS0 boot PROM."
    private const val CF_ERROR = "Choose a raw CF image with 512-byte sectors."
    private const val FONT_ERROR = "Choose an 8 MiB SL-C3000 CS0 dump."

    fun limit(name: String): Long = when (name) {
        NAND -> DBK_BYTES
        PROM -> PROM_BYTES
        CF -> MAX_CF_BYTES
        CF_ID, CF_COMMIT -> 32
        CF_SLOT -> 1024
        EXTERNAL_CF -> MAX_EXTERNAL_CF_BYTES + (1L shl 20)
        else -> 0
    }

    fun journalName(name: String) = name.startsWith(CF_JOURNAL_PREFIX) && cfToken.matches(name.removePrefix(CF_JOURNAL_PREFIX))

    fun ignoresJournal(name: String) =
        name.startsWith(CF_JOURNAL_PREFIX) || name.startsWith(".$CF_JOURNAL_PREFIX")

    fun portableError(dir: File, saved: Boolean): String? {
        val cf = File(dir, CF)
        if (!cf.isFile || cf.length() !in 512..MAX_CF_BYTES || cf.length() % 512 != 0L) return CF_ERROR
        val check = File(dir, CF_ID)
        if (saved && !check.isFile) return "Open and save the Zaurus once to link its internal CF image."
        val commit = File(dir, CF_COMMIT)
        if ((check.exists() && !validToken(check)) || (commit.exists() && !validToken(commit)))
            return "The Zaurus internal CF identity or checkpoint is invalid."
        if (dir.list()?.any(::journalName) == true)
            return "Open the Zaurus and save it to finish the internal CF write."
        val nand = File(dir, NAND)
        if ((!saved || nand.exists()) && (!nand.isFile || !validNand(nand))) return NAND_ERROR
        val prom = File(dir, PROM)
        if (prom.exists() && !validProm(prom)) return PROM_ERROR
        if (File(dir, CF_SLOT).exists() && ZaurusCfSlot.read(dir) == null)
            return "The removable CF card state is invalid."
        return null
    }

    private fun validToken(file: File) = file.isFile && file.length() == 32L && cfToken.matches(file.readText())

    private fun validProm(file: File): Boolean =
        file.isFile && file.length() == PROM_BYTES &&
            java.io.DataInputStream(file.inputStream()).use { it.readInt() != -1 }

    // Add device's Zaurus picks as document URIs. With `create` the CF is built from the `hdd1` archive at
    // `sizeGb`, and `prom` is only its LC Font source, as on the desktop; otherwise `cf` is imported.
    data class Picks(val nand: String? = null, val cf: String? = null, val prom: String? = null,
                     val create: Boolean = false, val hdd1: String? = null, val sizeGb: Int = ZaurusCf.DEFAULT_GB) {
        val ready get() = nand != null && (if (create) hdd1 != null else cf != null)

        fun putInto(json: org.json.JSONObject): org.json.JSONObject = json.put("zaurusNand", nand).put("zaurusCf", cf)
            .put("zaurusProm", prom).put("zaurusCreate", create).put("zaurusHdd1", hdd1).put("zaurusSizeGb", sizeGb)

        companion object {
            fun from(json: org.json.JSONObject): Picks {
                fun uri(name: String) = json.optString(name).takeIf(String::isNotEmpty)
                val size = json.optInt("zaurusSizeGb", ZaurusCf.DEFAULT_GB).takeIf { it in ZaurusCf.sizes } ?: ZaurusCf.DEFAULT_GB
                return Picks(uri("zaurusNand"), uri("zaurusCf"), uri("zaurusProm"), json.optBoolean("zaurusCreate"),
                    uri("zaurusHdd1"), size)
            }
        }
    }

    fun stage(resolver: ContentResolver, picks: Picks, dir: File, cancelled: () -> Boolean,
              progress: (Long, Long) -> Unit) {
        val nand = File(dir, NAND)
        copy(resolver, Uri.parse(requireNotNull(picks.nand)), nand, DBK_BYTES, NAND_ERROR, cancelled)
        if (!validNand(nand)) throw IOException(NAND_ERROR)
        val cf = File(dir, CF)
        if (picks.create) {
            // The CS0 dump only fills the LC Fonts here; the machine keeps no boot PROM.
            val font = picks.prom?.let { uri ->
                File(dir, ".font-rom").also { file ->
                    copy(resolver, Uri.parse(uri), file, PROM_BYTES, FONT_ERROR, cancelled)
                    if (!validProm(file)) throw IOException(FONT_ERROR)
                }
            }
            try {
                ZaurusCf.create(resolver, Uri.parse(requireNotNull(picks.hdd1)), font, picks.sizeGb, cf, cancelled, progress)
            } catch (e: Throwable) {
                cf.delete()
                throw e
            } finally {
                font?.delete()
            }
            return
        }
        picks.prom?.let { uri ->
            val prom = File(dir, PROM)
            copy(resolver, Uri.parse(uri), prom, PROM_BYTES, PROM_ERROR, cancelled)
            if (!validProm(prom)) throw IOException(PROM_ERROR)
        }
        copy(resolver, Uri.parse(requireNotNull(picks.cf)), cf, MAX_CF_BYTES, CF_ERROR, cancelled, sparse = true, progress = progress)
        if (cf.length() == 0L || cf.length() % 512 != 0L) throw IOException(CF_ERROR)
    }

    private fun validNand(file: File): Boolean {
        if (file.length() == RAW_NAND_BYTES) return true
        if (file.length() != DBK_BYTES) return false
        val record = ByteArray(16)
        return RandomAccessFile(file, "r").use { nand ->
            fun read32() = Integer.reverseBytes(nand.readInt())
            nand.readFully(record, 0, 12)
            if (!record.copyOf(12).contentEquals("[I]JFFS2TOP:".toByteArray(Charsets.US_ASCII)) ||
                read32() != 0x6a0000) return@use false
            val blockBytes = 32 * 528
            for (block in 0 until 1024) {
                nand.seek(16L + block * (16L + blockBytes))
                if (read32() != block * 32 * 512) return@use false
                nand.readFully(record, 0, 4)
                if (String(record, 0, 4, Charsets.US_ASCII) !in listOf("[GL]", "[GP]", "[E] ")) return@use false
            }
            true
        }
    }

    private fun copy(resolver: ContentResolver, uri: Uri, target: File, maxBytes: Long, message: String,
                     cancelled: () -> Boolean, sparse: Boolean = false, progress: (Long, Long) -> Unit = { _, _ -> }) {
        if (cancelled()) throw kotlinx.coroutines.CancellationException()
        val length = HostFiles.size(resolver, uri)?.takeIf { it >= 0 }
        if (length != null && length > maxBytes) throw IOException(message)
        val source = resolver.openInputStream(uri) ?: throw IOException("Couldn't read the selected file.")
        source.use { copyImage(it, target, maxBytes, length, sparse, message, cancelled, progress) }
    }

    // Skips full zero-filled blocks in the destination so sparse CF images stay small on disk.
    fun copyImage(input: InputStream, target: File, maximum: Long, length: Long? = null, sparse: Boolean = true,
                  message: String = "The Zaurus image is too large.", cancelled: () -> Boolean = { false },
                  progress: (Long, Long) -> Unit = { _, _ -> }) {
        val bytes = ByteArray(1 shl 20)
        val zeros = if (sparse) ByteArray(bytes.size) else null
        RandomAccessFile(target, "rw").use { output ->
            output.setLength(0)
            var at = 0L
            var lastPercent = -1L
            while (true) {
                if (cancelled()) throw kotlinx.coroutines.CancellationException()
                var n = 0
                while (n < bytes.size) {
                    val count = input.read(bytes, n, bytes.size - n)
                    if (count < 0) break
                    if (count > 0) n += count
                    else {
                        val one = input.read()
                        if (one < 0) break
                        bytes[n++] = one.toByte()
                    }
                }
                if (n == 0) break
                val next = at + n
                if (next > maximum || length != null && next > length) throw IOException(message)
                val empty = zeros != null && if (n == bytes.size) Arrays.equals(bytes, zeros) else (0 until n).all { bytes[it] == 0.toByte() }
                if (empty) output.seek(next) else output.write(bytes, 0, n)
                at = next
                if (length != null && length > 0) {
                    val percent = at * 100 / length
                    if (percent != lastPercent) {
                        progress(at, length)
                        lastPercent = percent
                    }
                }
            }
            if (length != null && at != length) throw IOException("The selected file ended early.")
            output.setLength(at)
            output.fd.sync()
        }
    }
}

internal data class ZaurusCfSlot(val identity: String, val protected: Boolean, val inserted: Boolean) {
    val argument get() = identity + (if (protected) " protected" else " writable") + if (inserted) "" else " ejected"

    fun write(dir: File) = SessionMetadata.write(File(dir, ZaurusImages.CF_SLOT), org.json.JSONObject()
        .put("swap", true).put("identity", identity).put("protected", protected).put("inserted", inserted))

    companion object {
        private val hex = Regex("[0-9a-f]{64}")

        fun parse(reply: String, inserted: Boolean): ZaurusCfSlot? {
            val words = reply.split(' ')
            if (words.size != 2 || !hex.matches(words[0]) || words[1] !in listOf("protected", "writable")) return null
            return ZaurusCfSlot(words[0], words[1] == "protected", inserted)
        }

        fun read(dir: File): ZaurusCfSlot? = try {
            val file = File(dir, ZaurusImages.CF_SLOT)
            if (file.length() !in 1..ZaurusImages.limit(file.name)) return null
            val json = org.json.JSONObject(file.readText())
            val id = json.getString("identity")
            if (!json.getBoolean("swap") || !hex.matches(id)) null
            else ZaurusCfSlot(id, json.getBoolean("protected"), json.getBoolean("inserted"))
        } catch (_: Exception) {
            null
        }
    }
}
