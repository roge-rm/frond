package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.UUID

// A saved Palm. The core writes and reads <id>.frond (SessionFile5); <id>.json holds its name,
// device and times. A session that hasn't been saved yet starts from the device's firmware.
data class Session(
    val id: String,
    val name: String,
    val device: Device,
    val created: Long,
    val used: Long,
    val file: File,
) {
    val saved: Boolean get() = file.isFile
    val savedAt: Long get() = file.lastModified()
    val size: Long get() = file.length()
}

object Sessions {
    // SessionFile5's magic and newest version, in common/session/session_file5.cpp.
    private const val MAGIC = 0x19800819L
    private const val VERSION = 5L

    fun folder(context: Context) = File(context.filesDir, "sessions").also { it.mkdirs() }

    private fun meta(context: Context, id: String) = File(folder(context), "$id.json")
    private fun data(context: Context, id: String) = File(folder(context), "$id.frond")

    fun list(context: Context): List<Session> {
        removeStaleSaves(context)
        return folder(context).listFiles { f -> f.name.endsWith(".json") }.orEmpty()
            .mapNotNull { read(context, it.name.removeSuffix(".json")) }
            .sortedByDescending { maxOf(it.used, if (it.saved) it.savedAt else 0L) }
    }

    // The core saves to <id>.frond.tmp-XXXXXX and renames it into place; a process killed mid-save
    // leaves the temporary file. A running save keeps touching its file, so only old ones go.
    private fun removeStaleSaves(context: Context) {
        val cutoff = System.currentTimeMillis() - 10 * 60 * 1000
        folder(context).listFiles { f -> f.name.contains(".frond.tmp-") && f.lastModified() < cutoff }.orEmpty().forEach { it.delete() }
    }

    // MD5 of the NOR image stored in a session file, which the core checks before low-latency
    // audio (native/main.cpp). Reads the SessionFile5 header and inflates as far as the NOR.
    fun norMd5(file: File): String? = try {
        java.io.DataInputStream(java.io.BufferedInputStream(file.inputStream())).use { input ->
            fun read32(): Long {
                val b = ByteArray(4)
                input.readFully(b)
                return (b[0].toLong() and 0xff) or ((b[1].toLong() and 0xff) shl 8) or
                    ((b[2].toLong() and 0xff) shl 16) or ((b[3].toLong() and 0xff) shl 24)
            }
            if (read32() != MAGIC) return null
            val version = read32()
            if (version !in 1..VERSION) return null
            read32() // device type
            if (version > 3) read32() // display mode
            if (version > 1) read32() // RAM size
            val metadataSize = read32()
            val norSize = read32()
            read32(); read32(); read32() // NAND, memory, savestate
            if (version >= 5) read32() // SD
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
        val json = JSONObject(meta(context, id).readText())
        val device = Device.byId(json.getString("device")) ?: throw IOException("unknown device")
        Session(id, json.getString("name"), device, json.getLong("created"), json.optLong("used"), data(context, id))
    } catch (e: Exception) {
        null
    }

    private fun write(context: Context, s: Session) {
        val json = JSONObject()
            .put("name", s.name).put("device", s.device.id)
            .put("created", s.created).put("used", s.used)
        val file = meta(context, s.id)
        val temporary = File(file.path + ".tmp")
        temporary.writeText(json.toString())
        if (!temporary.renameTo(file)) throw IOException("can't write ${file.name}")
    }

    // "Palm TX", then "Palm TX 2" and so on.
    fun freeName(context: Context, base: String): String {
        val taken = list(context).map { it.name }.toSet()
        if (base !in taken) return base
        var n = 2
        while ("$base $n" in taken) n++
        return "$base $n"
    }

    fun create(context: Context, device: Device, name: String = freeName(context, device.label)): Session {
        val id = UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        return Session(id, name, device, now, 0L, data(context, id)).also { write(context, it) }
    }

    fun rename(context: Context, s: Session, name: String) = write(context, s.copy(name = name.trim().ifEmpty { s.name }))

    fun markUsed(context: Context, s: Session) = write(context, s.copy(used = System.currentTimeMillis()))

    fun delete(context: Context, s: Session) {
        s.file.delete()
        meta(context, s.id).delete()
    }

    fun export(context: Context, s: Session, to: Uri): String? = try {
        val output = context.contentResolver.openOutputStream(to, "wt") ?: throw IOException("no stream")
        output.use { o -> s.file.inputStream().use { it.copyTo(o, 1 shl 20) } }
        null
    } catch (e: Exception) {
        "Couldn't export the session."
    }

    // Saves the running Palm to a picked document, like the desktop's Save a Copy. The core saves to
    // a private file first and keeps running from and saving to its own session. Blocks; run it off
    // the main thread. Returns an error message or null.
    fun saveCopy(context: Context, link: ControlLink, to: Uri): String? {
        val folder = File(context.cacheDir, "sessions").also { it.mkdirs() }
        val copy = File(folder, "copy.frond")
        try {
            val reply = link.request("save", copy.path, timeoutSeconds = 120)
            if (!reply.ok) throw IOException(reply.message)
            val output = context.contentResolver.openOutputStream(to, "wt") ?: throw IOException("no stream")
            output.use { o -> copy.inputStream().use { it.copyTo(o, 1 shl 20) } }
            return null
        } catch (e: Exception) {
            try { android.provider.DocumentsContract.deleteDocument(context.contentResolver, to) } catch (_: Exception) {}
            return "Couldn't save the copy."
        } finally {
            copy.delete()
        }
    }

    // Reads the session header: magic, version and device type, all little-endian 32-bit.
    private fun header(file: File): Triple<Long, Long, Long>? = try {
        java.io.DataInputStream(file.inputStream()).use { input ->
            val b = ByteArray(12)
            input.readFully(b)
            fun at(i: Int) = (b[i].toLong() and 0xff) or ((b[i + 1].toLong() and 0xff) shl 8) or
                ((b[i + 2].toLong() and 0xff) shl 16) or ((b[i + 3].toLong() and 0xff) shl 24)
            Triple(at(0), at(4), at(8))
        }
    } catch (e: IOException) {
        null
    }

    // Copies a picked .frond file in as a new session. Returns an error message or null.
    fun import(context: Context, from: Uri): String? {
        val id = UUID.randomUUID().toString()
        // Copied to .part and renamed after the checks, so a stopped import leaves no session behind.
        val target = File(data(context, id).path + ".part")
        if (!Firmware.copy(context, from, target)) return "Couldn't read that file."
        val (magic, version, type) = header(target) ?: Triple(0L, 0L, -1L)
        val device = Device.byCoreType(type.toInt())
        val error = when {
            magic != MAGIC -> "That isn't a Frond session."
            version > VERSION -> "That session is from a newer Frond."
            device == null || device !in Device.shown -> "That session is for a device Frond can't run here yet."
            else -> null
        }
        if (error != null) {
            target.delete()
            return error
        }
        if (!target.renameTo(data(context, id))) {
            target.delete()
            return "Couldn't save the session."
        }
        val name = freeName(context, displayName(context, from)?.substringBeforeLast('.')?.takeIf { it.isNotBlank() } ?: device!!.label)
        write(context, Session(id, name, device!!, System.currentTimeMillis(), 0L, data(context, id)))
        return null
    }

    private fun displayName(context: Context, uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }
}
