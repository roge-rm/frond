package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

// A saved Palm in files/sessions/<id>/. The core saves to the .frond sessionFile finds there,
// or <id>.frond when there's none yet; <id>.json in that folder holds its name, device and
// times. A session that hasn't been saved yet starts from the device's firmware. An old one was saved by an earlier Frond (a numbered
// session file or a device name this version doesn't know, then device is null); it can
// only be deleted.
data class Session(
    val id: String,
    val name: String,
    val device: Device?,
    val created: Long,
    val used: Long,
    val folder: File,
    val old: Boolean = false,
    val settings: MachineSettings = MachineSettings(),
) {
    // Looked up each time, as the first save names the file.
    val file: File get() = sessionPathIn(folder)
    val saved: Boolean get() = file.isFile
    val savedAt: Long get() = file.lastModified()
    val size: Long get() = packableSize(folder)
}

object Sessions {
    // SessionFile5's magic and newest version, in common/session/session_file5.cpp. Version 6 is the
    // first to name the device; earlier ones are refused. Version 7 is readable here.
    private const val MAGIC = 0x19800819L
    private const val VERSION = 8L
    private const val FIRST_NAMED_VERSION = 6L
    private const val DEVICE_NAME_SIZE = 32

    fun folder(context: Context) = File(context.filesDir, "sessions").also { it.mkdirs() }
    fun machine(context: Context, id: String) = File(folder(context), id)

    private fun meta(context: Context, id: String) = File(machine(context, id), "$id.json")
    // Where Android puts a session file it creates (imports, migration, recovery).
    private fun data(context: Context, id: String) = File(machine(context, id), "session.frond")

    fun list(context: Context): List<Session> {
        removeStaleSaves(context)
        migrate(context)
        return folder(context).listFiles { f -> f.isDirectory }.orEmpty()
            .mapNotNull { read(context, it.name) }
            .sortedByDescending { maxOf(it.used, if (it.saved) it.savedAt else 0L) }
    }

    // Loose <id>.frond + <id>.json become sessions/<id>/session.frond and <id>.json.
    private fun migrate(context: Context) {
        val root = folder(context)
        root.listFiles { f -> f.isFile && f.name.endsWith(".json") }.orEmpty().forEach { json ->
            val id = json.name.removeSuffix(".json")
            if (id.isEmpty() || id.startsWith(".")) return@forEach
            val dest = File(root, id)
            dest.mkdirs()
            File(root, "$id.frond").takeIf { it.isFile && sessionFile(dest) == null }?.let { it.renameTo(File(dest, "session.frond")) }
            val target = File(dest, json.name)
            if (json.canonicalFile != target.canonicalFile) json.renameTo(target)
        }
        root.listFiles { f -> f.isFile && f.name.endsWith(".frond") && !ignoredName(f.name) }.orEmpty().forEach { frond ->
            val id = frond.name.removeSuffix(".frond")
            if (id.isEmpty()) return@forEach
            val dest = File(root, id)
            dest.mkdirs()
            if (sessionFile(dest) == null) frond.renameTo(File(dest, "session.frond"))
        }
    }

    private fun removeStaleSaves(context: Context) = removeStaleTemps(folder(context), System.currentTimeMillis())

    // MD5 of the NOR image stored in a session file, which the core checks before low-latency
    // audio (native/main.cpp). Reads the SessionFile5 header and inflates as far as the NOR.
    fun norMd5(file: File): String? = try {
        val frond = if (file.isDirectory) sessionFile(file) ?: return null else file
        java.io.DataInputStream(java.io.BufferedInputStream(frond.inputStream())).use { input ->
            fun read32(): Long {
                val b = ByteArray(4)
                input.readFully(b)
                return (b[0].toLong() and 0xff) or ((b[1].toLong() and 0xff) shl 8) or
                    ((b[2].toLong() and 0xff) shl 16) or ((b[3].toLong() and 0xff) shl 24)
            }
            fun read64(): Long {
                val lo = read32()
                val hi = read32()
                return lo or (hi shl 32)
            }
            if (read32() != MAGIC) return null
            val version = read32()
            if (version !in FIRST_NAMED_VERSION..VERSION) return null
            input.readFully(ByteArray(DEVICE_NAME_SIZE)) // device name
            read32(); read32() // display mode, RAM size
            val metadataSize: Long
            val norSize: Long
            if (version >= 7L) {
                metadataSize = read64()
                norSize = read64()
                read64(); read64(); read64() // NAND, memory, savestate
                val mediaBytes = read32() * (if (version == 7L) 92L else 100L)
                if (mediaBytes > 0) input.skip(mediaBytes)
            } else {
                metadataSize = read32()
                norSize = read32()
                read32(); read32(); read32(); read32() // NAND, memory, savestate, SD
            }
            val inflated = java.util.zip.InflaterInputStream(input, java.util.zip.Inflater(), 1 shl 16)
            val digest = java.security.MessageDigest.getInstance("MD5")
            val buffer = ByteArray(1 shl 16)
            var skip = metadataSize
            while (skip > 0) {
                val n = inflated.read(buffer, 0, minOf(skip, buffer.size.toLong()).toInt())
                if (n < 0) return null
                skip -= n
            }
            var left = norSize
            while (left > 0) {
                val n = inflated.read(buffer, 0, minOf(left, buffer.size.toLong()).toInt())
                if (n < 0) return null
                digest.update(buffer, 0, n)
                left -= n
            }
            digest.digest().joinToString("") { "%02x".format(it) }
        }
    } catch (e: Exception) {
        null
    }

    fun read(context: Context, id: String): Session? = try {
        // An empty id would name the sessions folder itself.
        if (id.isEmpty()) return null
        val jsonFile = meta(context, id)
        if (!jsonFile.isFile) return null
        val json = SessionMetadata.read(jsonFile) {
            val device = Device.byId(JSONObject(meta(context, id).readText()).optString("device"))
            Settings.legacyMachine(context, device)
        }
        val device = Device.byId(json.getString("device"))
        val folder = machine(context, id)
        val file = sessionPathIn(folder)
        val old = device == null || (file.isFile && (header(file)?.version ?: 0L) < FIRST_NAMED_VERSION)
        Session(id, json.getString("name"), device, json.getLong("created"), json.optLong("used"), folder, old,
            MachineSettings.from(json.optJSONObject("settings")))
    } catch (e: Exception) {
        null
    }

    private fun write(context: Context, s: Session) {
        val device = s.device ?: throw IOException("old session")
        machine(context, s.id).mkdirs()
        val json = JSONObject()
            .put("name", s.name).put("device", device.id)
            .put("created", s.created).put("used", s.used).put("settings", s.settings.json())
        SessionMetadata.write(meta(context, s.id), json)
    }

    // "Palm TX", then "Palm TX 2" and so on.
    fun freeName(context: Context, base: String): String {
        val taken = list(context).map { it.name }.toSet()
        if (base !in taken) return base
        var n = 2
        while ("$base $n" in taken) n++
        return "$base $n"
    }

    fun create(context: Context, device: Device, name: String = freeName(context, device.label), settings: MachineSettings = MachineSettings()): Session {
        val id = UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        return Session(id, name, device, now, 0L, machine(context, id), settings = settings).also { write(context, it) }
    }

    fun rename(context: Context, s: Session, name: String) = SessionMetadata.update(meta(context, s.id)) {
        if (name.isNotBlank()) it.put("name", name.trim())
    }

    fun markUsed(context: Context, s: Session) = SessionMetadata.update(meta(context, s.id)) {
        it.put("used", System.currentTimeMillis())
    }

    fun updateSettings(context: Context, id: String, change: (MachineSettings) -> MachineSettings) {
        read(context, id) ?: throw IOException("session no longer exists")
        SessionMetadata.update(meta(context, id)) { json ->
            json.put("settings", change(MachineSettings.from(json.optJSONObject("settings"))).json())
        }
    }

    // For a machine folder or a file in one, the session file the core reads and saves there;
    // anything else as it is.
    fun resolve(context: Context, path: String?): String? = forPath(context, path)?.file?.path ?: path

    fun forPath(context: Context, path: String?): Session? {
        migrate(context)
        val file = path?.let(::File) ?: return null
        val root = folder(context).canonicalFile
        val canon = try { file.canonicalFile } catch (e: Exception) { file.absoluteFile }
        val parent = canon.parentFile ?: return null
        if (canon.isDirectory && parent.canonicalFile == root) return read(context, canon.name)
        if (parent.canonicalFile == root && canon.extension == "frond") return read(context, canon.nameWithoutExtension)
        val grand = parent.parentFile ?: return null
        if (grand.canonicalFile == root) return read(context, parent.name)
        return null
    }

    fun delete(context: Context, s: Session) {
        require(s.id.isNotEmpty())
        SessionMetadata.locked(meta(context, s.id)) {
            machine(context, s.id).deleteRecursively()
        }
    }

    fun export(context: Context, s: Session, to: Uri): String? =
        exportPackable(s.folder,
            { context.contentResolver.openOutputStream(to, "wt") },
            { try { android.provider.DocumentsContract.deleteDocument(context.contentResolver, to) } catch (_: Exception) {} })

    // Saves the running Palm to a picked document, like the desktop's Save a Copy. The core saves to
    // a private folder first and keeps running from and saving to its own session. Blocks; run it off
    // the main thread. Returns an error message or null. The copy is a zip of packable files; SAF
    // cannot promise atomic replacement of an existing document.
    fun saveCopy(context: Context, link: ControlLink, to: Uri): String? {
        val folder = File(context.cacheDir, "sessions").also { it.mkdirs() }
        // A copy left by a process killed mid-save would lend its old card to this one.
        val copy = File(folder, "copy").also { it.deleteRecursively(); it.mkdirs() }
        try {
            val reply = link.request("save", copy.path, timeoutSeconds = 120)
            if (!reply.ok) throw IOException(reply.message)
            val output = context.contentResolver.openOutputStream(to, "wt") ?: throw IOException("no stream")
            output.use { zipPackable(copy, it) }
            return null
        } catch (e: Exception) {
            try { android.provider.DocumentsContract.deleteDocument(context.contentResolver, to) } catch (_: Exception) {}
            return "Couldn't save the copy."
        } finally {
            copy.deleteRecursively()
        }
    }

    // A session saved before an SD card exchange, like the desktop's recovery folder:
    // files/recovery/before-exchange/, and before-exchange.json with the name it came from,
    // written once the save is complete. Dropped after the exchange, kept as a session if it fails.
    fun recoveryFolder(context: Context) = File(context.filesDir, "recovery")
    fun recoveryMachine(context: Context) = File(recoveryFolder(context), "before-exchange")
    fun recoveryFile(context: Context) = sessionPathIn(recoveryMachine(context))
    private fun recoveryMeta(context: Context) = File(recoveryMachine(context), "before-exchange.json")

    private fun recoverySession(context: Context): File = recoveryFile(context)

    // Room a recovery save needs: the saved session's size with some slack, or for an unsaved one its
    // firmware and RAM, as the save is compressed but can't be smaller than nothing.
    fun recoveryNeeds(savedSize: Long?, device: Device): Long =
        if (savedSize != null && savedSize > 0) savedSize + savedSize / 4 + (16L shl 20)
        else device.norSize + device.nandSize + (64L shl 20)

    // Saves the running session as the recovery copy. Returns an error message or null.
    fun saveRecovery(context: Context, link: ControlLink, name: String, session: String? = null): String? {
        dropRecovery(context)
        recoveryMachine(context).mkdirs()
        val reply = link.request("save", recoveryMachine(context).path, timeoutSeconds = 120)
        if (!reply.ok || !recoverySession(context).isFile) {
            dropRecovery(context)
            return reply.message.ifEmpty { "Couldn't save the session." }
        }
        try {
            val meta = recoveryMeta(context)
            val temporary = File(meta.path + ".tmp")
            temporary.writeText(JSONObject().put("name", name)
                .put("settings", forPath(context, session)?.settings?.json()).toString())
            if (!temporary.renameTo(meta)) throw IOException("can't write ${meta.name}")
        } catch (e: Exception) {
            dropRecovery(context)
            return "Couldn't save the session."
        }
        return null
    }

    fun dropRecovery(context: Context) {
        recoveryFolder(context).listFiles().orEmpty().forEach { it.deleteRecursively() }
    }

    // Moves a complete recovery copy into the session list as "<name> before exchange" and returns
    // that name, or null when there was none. A save that didn't finish is dropped.
    fun adoptRecovery(context: Context): String? {
        val file = recoverySession(context)
        val meta = recoveryMeta(context)
        try {
            if (!file.isFile || !meta.isFile) return null
            val device = Device.byId(header(file)?.device) ?: return null
            val recovery = try { JSONObject(meta.readText()) } catch (e: Exception) { JSONObject() }
            val from = recovery.optString("name", device.label)
            val settings = MachineSettings.from(recovery.optJSONObject("settings"), Settings.legacyMachine(context, device))
            val id = UUID.randomUUID().toString()
            val dest = machine(context, id).also { it.mkdirs() }
            if (!moveRecoveryCards(recoveryMachine(context), file, dest) || !file.renameTo(data(context, id))) {
                dest.deleteRecursively()
                return null
            }
            // A recovery of a recovery keeps one "before exchange"; freeName numbers it.
            val name = freeName(context, "${from.removeSuffix(" before exchange")} before exchange")
            try {
                write(context, Session(id, name, device, System.currentTimeMillis(), 0L, machine(context, id), settings = settings))
            } catch (e: IOException) {
                dest.deleteRecursively()
                return null
            }
            return name
        } finally {
            dropRecovery(context)
        }
    }

    private class Header(val magic: Long, val version: Long, val device: String?)

    // Reads the session header: little-endian 32-bit magic and version, then the device's fixed
    // name, NUL-padded to 32 bytes. Versions before 6 have no name.
    private fun header(file: File): Header? = try {
        java.io.DataInputStream(file.inputStream()).use { input ->
            val b = ByteArray(8 + DEVICE_NAME_SIZE)
            input.readFully(b, 0, 8)
            fun at(i: Int) = (b[i].toLong() and 0xff) or ((b[i + 1].toLong() and 0xff) shl 8) or
                ((b[i + 2].toLong() and 0xff) shl 16) or ((b[i + 3].toLong() and 0xff) shl 24)
            val version = at(4)
            val device = if (version >= FIRST_NAMED_VERSION) {
                input.readFully(b, 8, DEVICE_NAME_SIZE)
                val name = b.copyOfRange(8, b.size)
                String(name, 0, name.indexOf(0).let { if (it < 0) name.size else it }, Charsets.US_ASCII)
            } else {
                null
            }
            Header(at(0), version, device)
        }
    } catch (e: IOException) {
        null
    }

    // Copies a picked .frond or a zip of packable files in as a new session. Returns an error message or null.
    fun import(context: Context, from: Uri): String? {
        val id = UUID.randomUUID().toString()
        activeImports.add(id)
        val dest = machine(context, id).also { it.mkdirs() }
        val staged = File(dest, ".import.part").also { it.mkdirs() }
        try {
            val input = context.contentResolver.openInputStream(from) ?: return "Couldn't read that file."
            input.use { stream ->
                val peek = stream.markSupported().let { if (it) stream else java.io.BufferedInputStream(stream) }
                peek.mark(4)
                val magic = ByteArray(4)
                val n = peek.read(magic)
                peek.reset()
                if (n >= 2 && magic[0] == 0x50.toByte() && magic[1] == 0x4b.toByte()) {
                    if (!unzipPackable(peek, staged)) return "Couldn't read that file."
                } else {
                    val target = File(staged, "session.frond")
                    target.outputStream().use { peek.copyTo(it, 1 shl 20) }
                }
            }
            val frond = sessionFile(staged) ?: return "That isn't a Frond session."
            val header = header(frond) ?: Header(0L, 0L, null)
            val device = Device.byId(header.device)
            val error = when {
                header.magic != MAGIC -> "That isn't a Frond session."
                header.version > VERSION -> "That session is from a newer Frond."
                header.version < FIRST_NAMED_VERSION -> "That session is from an older Frond and can't be opened."
                device == null || device !in Device.shown -> "That session is for a device Frond can't run here yet."
                else -> null
            }
            if (error != null) return error
            if (header.version >= 7L) {
                checkImportedSiblings(staged, frond)?.let { return it }
            }
            if (frond != File(dest, "session.frond") && !frond.renameTo(File(dest, "session.frond")))
                return "Couldn't save the session."
            for (sibling in listOf("sd.frondsd", "ms.frondms", "ms.frondmsp")) {
                File(staged, sibling).takeIf { it.isFile }?.renameTo(File(dest, sibling))
            }
            File(dest, "session.frond.settings.json").takeIf { it.isFile }?.delete()
            val name = freeName(context, displayName(context, from)?.substringBeforeLast('.')?.takeIf { it.isNotBlank() } ?: device!!.label)
            write(context, Session(id, name, device!!, System.currentTimeMillis(), 0L, dest))
            return null
        } catch (e: Exception) {
            dest.deleteRecursively()
            return "Couldn't save the session."
        } finally {
            File(dest, ".import.part").deleteRecursively()
            if (!data(context, id).isFile) dest.deleteRecursively()
            activeImports.remove(id)
        }
    }

    private fun displayName(context: Context, uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }
}

internal fun le32(bytes: ByteArray, i: Int): Long =
    (bytes[i].toLong() and 0xff) or ((bytes[i + 1].toLong() and 0xff) shl 8) or
        ((bytes[i + 2].toLong() and 0xff) shl 16) or ((bytes[i + 3].toLong() and 0xff) shl 24)

internal fun le64(bytes: ByteArray, i: Int): Long = le32(bytes, i) or (le32(bytes, i + 4) shl 32)

private fun java.io.InputStream.readExact(n: Int): ByteArray {
    val out = ByteArray(n)
    var got = 0
    while (got < n) {
        val r = read(out, got, n - got)
        if (r < 0) return out.copyOf(got)
        got += r
    }
    return out
}

// Header, size and CRC of each v7 or v8 sibling. A stub shorter than the TOC is no media.
// v8 descriptors are 100 bytes and end with the SD content generation, which a .frondsd v2
// header must match. A v2 file has one CRC32 per 1 MiB chunk after the payload, and a header
// CRC over the header (its own field zeroed) and that table.
internal fun checkImportedSiblings(dir: File, frond: File): String? {
    val mismatch = "That session's card files don't match."
    val bytes = try { frond.readBytes() } catch (e: IOException) { return "Couldn't read that file." }
    if (bytes.size < 8) return "Couldn't read that file."
    val version = le32(bytes, 4)
    if (version != 7L && version != 8L) return null
    val descSize = if (version == 7L) 92 else 100
    data class Card(val kind: Long, val physical: Long, val generation: Long)
    val cards = mutableListOf<Card>()
    if (bytes.size >= 40 + 52) {
        val mediaCount = le32(bytes, 88)
        val descBytes = mediaCount * descSize
        if (mediaCount > 2L || (mediaCount != 0L && (descBytes / descSize != mediaCount || bytes.size < 40 + 52 + descBytes)))
            return mismatch
        for (i in 0 until mediaCount.toInt()) {
            val at = 40 + 52 + i * descSize
            val kind = le32(bytes, at)
            val nameBytes = bytes.copyOfRange(at + 4, at + 36)
            val nameEnd = nameBytes.indexOf(0).let { if (it < 0) 32 else it }
            val name = String(nameBytes, 0, nameEnd, Charsets.US_ASCII)
            val logical = le64(bytes, at + 76)
            val physical = le64(bytes, at + 84)
            val generation = if (descSize == 100) le64(bytes, at + 92) else 0L
            if (kind == 2L && generation != 0L) return mismatch
            when (kind) {
                1L -> if (name != "sd.frondsd" || logical != physical || physical < 512 || physical % 512 != 0L)
                    return mismatch
                2L -> if (name != "ms.frondms" || Device.MEMORY_STICK_SIZES.none {
                        logical == it.toLong() shl 20 && physical == Device.memoryStickPhysicalBytes(it) })
                    return mismatch
                // Memory Stick Pro: flat sectors in an SD-style file.
                3L -> if (name != "ms.frondmsp" || logical != physical || Device.MEMORY_STICK_PRO_SIZES.none { logical == it.toLong() shl 20 })
                    return mismatch
                else -> return mismatch
            }
            cards.add(Card(kind, physical, generation))
        }
    }
    var expectSd = false
    var expectMs = false
    var expectPro = false
    for (card in cards) {
        if (card.kind == 1L || card.kind == 3L) {
            if (if (card.kind == 1L) expectSd else expectPro) return mismatch
            if (card.kind == 1L) expectSd = true else expectPro = true
            val sd = File(dir, if (card.kind == 1L) "sd.frondsd" else "ms.frondmsp")
            if (!sd.isFile) return mismatch
            sd.inputStream().use { input ->
                val hdr = input.readExact(64)
                if (hdr.size != 64 || !hdr.copyOfRange(0, 8).contentEquals("FRONDSD\u0000".toByteArray(Charsets.US_ASCII)))
                    return mismatch
                val sdVersion = le32(hdr, 8)
                if ((sdVersion != 1L && sdVersion != 2L) || le32(hdr, 12) != 512L) return mismatch
                val payload = le64(hdr, 16)
                if (payload != card.physical) return mismatch
                val chunk = 1L shl 20
                val chunks = (payload + chunk - 1) / chunk
                if (sdVersion == 1L) {
                    if (card.generation != 0L || sd.length() != 64 + payload) return mismatch
                    if (hdr.copyOfRange(28, 64).any { it != 0.toByte() }) return mismatch
                } else {
                    if (le64(hdr, 28) != card.generation || le64(hdr, 36) != 64 + payload ||
                        le32(hdr, 44) != chunk || sd.length() != 64 + payload + 4 * chunks)
                        return mismatch
                    if (hdr.copyOfRange(48, 64).any { it != 0.toByte() }) return mismatch
                }
                val crc = CRC32()
                val chunkCrcs = LongArray(chunks.toInt())
                val buf = ByteArray(65536)
                var done = 0L
                while (done < payload) {
                    val want = minOf(buf.size.toLong(), payload - done, chunk - done % chunk).toInt()
                    val n = input.read(buf, 0, want)
                    if (n <= 0) return mismatch
                    crc.update(buf, 0, n)
                    done += n
                    if (sdVersion == 2L && (done % chunk == 0L || done == payload)) {
                        chunkCrcs[((done - 1) / chunk).toInt()] = crc.value
                        crc.reset()
                    }
                }
                if (sdVersion == 1L) {
                    if (crc.value != le32(hdr, 24)) return mismatch
                } else {
                    val table = input.readExact(4 * chunks.toInt())
                    if (table.size != 4 * chunks.toInt()) return mismatch
                    for (i in 0 until chunks.toInt()) if (le32(table, 4 * i) != chunkCrcs[i]) return mismatch
                    val headerCrc = CRC32()
                    headerCrc.update(hdr.copyOf().also { for (i in 24 until 28) it[i] = 0 })
                    headerCrc.update(table)
                    if (headerCrc.value != le32(hdr, 24)) return mismatch
                }
            }
        } else {
            if (expectMs) return mismatch
            expectMs = true
            val ms = File(dir, "ms.frondms")
            if (!ms.isFile) return mismatch
            ms.inputStream().use { input ->
                val hdr = input.readExact(64)
                if (hdr.size != 64 || !hdr.copyOfRange(0, 8).contentEquals("FRONDMS\u0000".toByteArray(Charsets.US_ASCII)))
                    return mismatch
                // The header's blocks and pages per block must make the descriptor's size.
                if (le32(hdr, 8) != 1L || le32(hdr, 12) * le32(hdr, 16) * 521 != card.physical ||
                    le32(hdr, 16) != (if (card.physical < Device.memoryStickPhysicalBytes(16)) 16L else 32L) ||
                    le32(hdr, 20) != 512L || le32(hdr, 24) != 9L)
                    return mismatch
                if (ms.length() != 64 + card.physical) return mismatch
            }
        }
    }
    if (!expectSd && File(dir, "sd.frondsd").isFile) return mismatch
    if (!expectMs && File(dir, "ms.frondms").isFile) return mismatch
    if (!expectPro && File(dir, "ms.frondmsp").isFile) return mismatch
    if (expectMs && expectPro) return mismatch
    return null
}

// The core saves to .frond.tmp-XXXXXX, .frondsd.tmp-XXXXXX and .frondms.tmp-XXXXXX and renames
// them into place; a process killed mid-save leaves them. A running save keeps touching its
// file, so only old ones go.
internal fun saveTempName(name: String) =
    !liveCardName(name) && (name.contains(".frond.tmp-") || name.contains(".frondsd.tmp-") || name.contains(".frondms.tmp-") ||
        (name.startsWith(".gen-") && name.contains("-tmp-")))

// A running machine's candidate card and its generation files. The core flocks the candidate and
// removes abandoned ones itself when that machine starts again.
internal fun liveCardName(name: String) = name.contains(SdCard.LIVE_CANDIDATE)

internal fun removeStaleTemps(root: File, now: Long) {
    val cutoff = now - 10 * 60 * 1000
    fun clean(dir: File) {
        dir.listFiles { f -> f.isFile && saveTempName(f.name) && f.lastModified() < cutoff }.orEmpty().forEach { it.delete() }
    }
    clean(root)
    root.listFiles { it.isDirectory }.orEmpty().forEach { clean(it) }
    removeStaleImports(root, now)
}

// Ids of the imports running in this process. Their folders are never stale, however long the
// document takes to read.
internal val activeImports: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()

// An import stages into <id>/.import.part and writes <id>.json last; a process killed before
// that leaves a folder no session lists. Only old ones not being imported now go.
internal fun removeStaleImports(root: File, now: Long) {
    val cutoff = now - 10 * 60 * 1000
    root.listFiles { it.isDirectory }.orEmpty().forEach { dir ->
        if (dir.name in activeImports) return@forEach
        if (!File(dir, ".import.part").isDirectory || File(dir, "${dir.name}.json").exists()) return@forEach
        if (dir.walkTopDown().any { it.lastModified() >= cutoff }) return@forEach
        dir.deleteRecursively()
    }
}

internal fun ignoredName(name: String) =
    name.contains(".frond.tmp-") || name.contains(".frondsd.tmp-") ||
        name.contains(".frondms.tmp-") || name.contains(".frondsd.journal-") || name.contains(".frondmsp.journal-") ||
        name.contains(".gen-")

internal fun packableName(name: String): Boolean {
    if (ignoredName(name) || '/' in name || '\\' in name) return false
    return name == "sd.frondsd" || name == "ms.frondms" || name == "ms.frondmsp" || name.endsWith(".frond.settings.json") ||
        (name.endsWith(".frond") && !name.endsWith(".frond.new"))
}

internal fun packableSize(dir: File): Long =
    dir.listFiles()?.filter { it.isFile && packableName(it.name) }?.sumOf { it.length() } ?: 0L

// The machine folder's session file, as frondMachine::sessionFile in the core finds it:
// <folder name>.frond, then session.frond, then the only other .frond. Save temps and card
// journals don't count.
internal fun sessionFile(dir: File): File? {
    if (!dir.isDirectory) return null
    val named = File(dir, "${dir.name}.frond")
    if (named.isFile && !ignoredName(named.name)) return named
    val android = File(dir, "session.frond")
    if (android.isFile && !ignoredName(android.name)) return android
    return dir.listFiles { f -> f.isFile && !ignoredName(f.name) && f.name.endsWith(".frond") }?.singleOrNull()
}

// Where the core saves in `dir`: its session file, or <folder name>.frond before the first save
// (sessionPathIn in the core).
internal fun sessionPathIn(dir: File): File = sessionFile(dir) ?: File(dir, "${dir.name}.frond")

// The content generation of the SD card or Memory Stick Pro (kind 1 or 3) a v8 .frond
// describes, with its kind, or 0 for none.
internal fun sdCardOf(frond: File?): Pair<Long, Long> = try {
    val head = frond?.inputStream()?.use { it.readExact(92 + 200) } ?: ByteArray(0)
    if (head.size < 92 || le32(head, 4) != 8L) 0L to 1L
    else (0 until minOf(le32(head, 88), 2L).toInt()).map { 92 + it * 100 }
        .firstOrNull { it + 100 <= head.size && le32(head, it).let { kind -> kind == 1L || kind == 3L } }
        ?.let { le64(head, it + 92) to le32(head, it) } ?: (0L to 1L)
} catch (e: IOException) {
    0L to 1L
}

internal fun sdGenerationOf(frond: File?): Long = sdCardOf(frond).first

// A save the core didn't finish leaves sd.frondsd behind the .frond: the card it describes is
// sd.frondsd.journal-<G>.full, or sd.frondsd one generation behind plus sd.frondsd.journal-<G>.
// Exports settle it as a load would, so they hold a plain sd.frondsd at G. Both null when
// sd.frondsd is right as it is, or nothing can settle it. A Memory Stick Pro works the same way
// with ms.frondmsp; `sibling` names the card file.
internal class SdSettle(val full: File?, val journal: File?, val sibling: String = "sd.frondsd")

internal fun sdSettleOf(dir: File, frond: File? = sessionFile(dir)): SdSettle {
    val (want, kind) = sdCardOf(frond)
    val sibling = if (kind == 3L) "ms.frondmsp" else "sd.frondsd"
    val none = SdSettle(null, null, sibling)
    if (want == 0L) return none
    val sd = File(dir, sibling)
    val hdr = try { if (sd.isFile) sd.inputStream().use { it.readExact(64) } else ByteArray(0) } catch (e: IOException) { ByteArray(0) }
    val v2 = hdr.size == 64 && hdr.copyOfRange(0, 8).contentEquals("FRONDSD\u0000".toByteArray(Charsets.US_ASCII)) &&
        le32(hdr, 8) == 2L
    val have = if (v2) le64(hdr, 28) else 0L
    if (v2 && have == want) return none
    val full = File(dir, "$sibling.journal-$want.full")
    if (full.isFile) return SdSettle(full, null, sibling)
    val journal = File(dir, "$sibling.journal-$want")
    if (v2 && have + 1 == want && journal.isFile) return SdSettle(null, journal, sibling)
    return none
}

// A checked journal: its header fields, the new table and where each record's data starts.
internal class SdJournalIndex(val payload: Long, val to: Long, val table: ByteArray, val records: List<LongArray>)

// Checks a whole journal against sd.frondsd's header: trailer CRC, generations and every bound.
internal fun readSdJournalIndex(journal: File, sdHeader: ByteArray, sdLength: Long): SdJournalIndex? =
    java.io.RandomAccessFile(journal, "r").use { file ->
        val size = file.length()
        if (size < 64 + 16) return null
        val head = ByteArray(64).also { file.seek(0); file.readFully(it) }
        val trailer = ByteArray(16).also { file.seek(size - 16); file.readFully(it) }
        if (!head.copyOfRange(0, 8).contentEquals("FRONDJNL".toByteArray(Charsets.US_ASCII)) || le32(head, 8) != 1L ||
            le32(head, 12) != 0L || !trailer.copyOfRange(0, 8).contentEquals("FRONDEND".toByteArray(Charsets.US_ASCII)) ||
            le32(trailer, 12) != 0L)
            return null
        val payload = le64(head, 16)
        val from = le64(head, 24)
        val to = le64(head, 32)
        val count = le64(head, 40)
        val chunks = le64(head, 48)
        if (le32(sdHeader, 8) != 2L || le64(sdHeader, 16) != payload || le64(sdHeader, 28) != from || to != from + 1 ||
            chunks != (payload + (1L shl 20) - 1) / (1L shl 20) || sdLength != 64 + payload + 4 * chunks)
            return null
        val body = size - 16
        val crc = CRC32()
        val buf = ByteArray(1 shl 20)
        file.seek(0)
        var done = 0L
        while (done < body) {
            val n = minOf(buf.size.toLong(), body - done).toInt()
            file.readFully(buf, 0, n)
            crc.update(buf, 0, n)
            done += n
        }
        if (crc.value != le32(trailer, 8)) return null
        val records = ArrayList<LongArray>()
        var at = 64L
        val rec = ByteArray(12)
        for (i in 0 until count) {
            if (at + 12 > body) return null
            file.seek(at)
            file.readFully(rec)
            val offset = le64(rec, 0)
            val length = le32(rec, 8)
            if (offset < 0 || offset > payload || length > payload - offset || at + 12 + length > body) return null
            records.add(longArrayOf(offset, length, at + 12))
            at += 12 + length
        }
        if (body - at != 4 * chunks) return null
        val table = ByteArray((4 * chunks).toInt()).also { file.seek(at); file.readFully(it) }
        SdJournalIndex(payload, to, table, records)
    }

// Writes sd.frondsd with the journal applied: header at the new generation with its CRC, payload
// with each record laid over it in journal order, then the new table. Checks everything first,
// so nothing is written for a damaged journal.
internal fun writeSettledSd(sd: File, journal: File, out: java.io.OutputStream) {
    val hdr = sd.inputStream().use { it.readExact(64) }
    if (hdr.size != 64) throw IOException("short card")
    val index = readSdJournalIndex(journal, hdr, sd.length()) ?: throw IOException("damaged journal")
    val block = 1L shl 20
    val byBlock = HashMap<Long, MutableList<LongArray>>()
    for (r in index.records) {
        if (r[1] == 0L) continue
        for (b in r[0] / block..(r[0] + r[1] - 1) / block) byBlock.getOrPut(b) { ArrayList() }.add(r)
    }
    val header = hdr.copyOf()
    for (i in 0 until 8) header[28 + i] = (index.to ushr (8 * i)).toByte()
    for (i in 24 until 28) header[i] = 0
    val crc = CRC32().also { it.update(header); it.update(index.table) }.value
    for (i in 0 until 4) header[24 + i] = (crc ushr (8 * i)).toByte()
    out.write(header)
    java.io.RandomAccessFile(sd, "r").use { card ->
        java.io.RandomAccessFile(journal, "r").use { j ->
            val buf = ByteArray(block.toInt())
            var pos = 0L
            while (pos < index.payload) {
                val n = minOf(block, index.payload - pos).toInt()
                card.seek(64 + pos)
                card.readFully(buf, 0, n)
                for (r in byBlock[pos / block].orEmpty()) {
                    val start = maxOf(r[0], pos)
                    val end = minOf(r[0] + r[1], pos + n)
                    j.seek(r[2] + (start - r[0]))
                    j.readFully(buf, (start - pos).toInt(), (end - start).toInt())
                }
                out.write(buf, 0, n)
                pos += n
            }
        }
    }
    out.write(index.table)
}

// Moves the cards `frond` needs from a recovery folder into `dest`: sd.frondsd with the journal
// an unfinished save left (the load replays it), or the full copy in its place, and ms.frondms.
// A Memory Stick Pro's ms.frondmsp moves like sd.frondsd. False when one won't move.
internal fun moveRecoveryCards(from: File, frond: File, dest: File): Boolean {
    val settle = sdSettleOf(from, frond)
    val moves = buildList {
        val sd = settle.full ?: File(from, settle.sibling)
        if (sd.isFile) add(sd to settle.sibling)
        settle.journal?.let { add(it to it.name) }
        File(from, "ms.frondms").takeIf { it.isFile }?.let { add(it to "ms.frondms") }
        val other = if (settle.sibling == "sd.frondsd") "ms.frondmsp" else "sd.frondsd"
        File(from, other).takeIf { it.isFile }?.let { add(it to other) }
    }
    return moves.all { (file, name) -> file.renameTo(File(dest, name)) }
}

// Zips `dir` to the stream `open` gives. On any failure `delete` removes the target, which a
// damaged journal leaves half written. Returns an error message or null.
internal fun exportPackable(dir: File?, open: () -> java.io.OutputStream?, delete: () -> Unit): String? = try {
    if (dir == null) throw IOException("no folder")
    (open() ?: throw IOException("no stream")).use { zipPackable(dir, it) }
    null
} catch (e: Exception) {
    delete()
    "Couldn't export the session."
}

internal fun zipPackable(dir: File, output: java.io.OutputStream) {
    val settle = sdSettleOf(dir)
    ZipOutputStream(output).use { zip ->
        val files = dir.listFiles()?.filter { it.isFile && packableName(it.name) }.orEmpty()
            .filterNot { settle.full != null && it.name == settle.sibling }
        val entries = (files.map { it.name to it } + listOfNotNull(settle.full?.let { settle.sibling to it }))
            .sortedBy { it.first }
        for ((name, file) in entries) {
            zip.putNextEntry(ZipEntry(name))
            if (name == settle.sibling && settle.journal != null) writeSettledSd(file, settle.journal, zip)
            else file.inputStream().use { it.copyTo(zip, 1 shl 20) }
            zip.closeEntry()
        }
    }
}

internal fun unzipPackable(input: java.io.InputStream, dest: File): Boolean = try {
    dest.mkdirs()
    ZipInputStream(input).use { zip ->
        while (true) {
            val entry = zip.nextEntry ?: break
            val name = File(entry.name).name
            if (entry.isDirectory || !packableName(name)) continue
            File(dest, name).outputStream().use { zip.copyTo(it, 1 shl 20) }
        }
    }
    sessionFile(dest) != null
} catch (e: Exception) {
    false
}
