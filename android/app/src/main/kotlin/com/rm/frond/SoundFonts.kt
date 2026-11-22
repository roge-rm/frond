package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.CRC32

// SoundFonts picked for the Zodiac's Music output, copied into the app's files so the core can read them by path.
// A file with the name of a copy already there but other contents gets its CRC-32 in its name, so one machine's
// pick never replaces another's.
object SoundFonts {
    fun folder(context: Context) = File(context.filesDir, "soundfonts")

    // A SoundFont 2 file starts RIFF, a length, then sfbk.
    fun isSoundFont(head: ByteArray): Boolean =
        head.size >= 12 && String(head, 0, 4, Charsets.US_ASCII) == "RIFF" && String(head, 8, 4, Charsets.US_ASCII) == "sfbk"

    // A file name safe to keep: no folders, nothing hidden, ends in .sf2.
    fun cleanName(name: String?): String {
        val base = (name ?: "").substringAfterLast('/').replace(Regex("[^A-Za-z0-9 ._()-]"), "_").trim().trimStart('.')
        val named = base.ifEmpty { "SoundFont" }
        return if (named.endsWith(".sf2", ignoreCase = true)) named else "$named.sf2"
    }

    // Copies the picked file into the folder and gives its path. Throws IOException with a message to show.
    fun import(context: Context, uri: Uri, cancelled: () -> Boolean, progress: (Long, Long) -> Unit): String {
        val resolver = context.contentResolver
        var name: String? = null
        var size = -1L
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                name = c.getString(0)
                if (!c.isNull(1)) size = c.getLong(1)
            }
        }
        val folder = folder(context)
        val clean = cleanName(name)
        val partial = File(folder, ".$clean.part")
        folder.mkdirs()
        val input = resolver.openInputStream(uri) ?: throw IOException("Couldn't open the file.")
        try {
            val crc = input.use { copy(it, partial, size, cancelled, progress) }
            if (cancelled()) throw IOException(COPY_CANCELLED)
            return keep(folder, clean, partial, crc).path
        } finally {
            partial.delete()
        }
    }

    // A name for other contents under a name already taken: the CRC-32 before .sf2.
    fun hashedName(name: String, crc: Long): String = "${name.removeSuffix(".sf2").removeSuffix(".SF2")}-%08x.sf2".format(crc)

    // Moves a finished copy (its CRC-32 crc) in as name, or as hashedName when name holds other contents. A file
    // already there with the same contents is kept and the copy dropped. Gives the file the machine uses.
    fun keep(folder: File, name: String, partial: File, crc: Long): File {
        for (target in listOf(File(folder, name), File(folder, hashedName(name, crc)))) {
            if (!target.exists()) {
                if (!partial.renameTo(target)) throw IOException("Couldn't keep the SoundFont.")
                return target
            }
            if (target.length() == partial.length() && crcOf(target) == crc) {
                partial.delete()
                return target
            }
        }
        throw IOException("Couldn't keep the SoundFont.")
    }

    fun crcOf(file: File): Long {
        val crc = CRC32()
        val buffer = ByteArray(1 shl 16)
        file.inputStream().use { input ->
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                crc.update(buffer, 0, n)
            }
        }
        return crc.value
    }

    // Copies input to to and gives its CRC-32.
    private fun copy(input: InputStream, to: File, size: Long, cancelled: () -> Boolean, progress: (Long, Long) -> Unit): Long {
        val buffer = ByteArray(1 shl 16)
        var head = 0
        while (head < 12) {
            val n = input.read(buffer, head, 12 - head)
            if (n < 0) break
            head += n
        }
        if (!isSoundFont(buffer.copyOf(head))) throw IOException("That isn't a SoundFont (.sf2) file.")
        var done = head.toLong()
        val crc = CRC32()
        crc.update(buffer, 0, head)
        to.outputStream().use { out ->
            out.write(buffer, 0, head)
            while (!cancelled()) {
                val n = input.read(buffer)
                if (n < 0) break
                out.write(buffer, 0, n)
                crc.update(buffer, 0, n)
                done += n
                progress(done, size)
            }
        }
        return crc.value
    }

    // Deletes the copies no machine uses. keep holds the paths in use.
    fun prune(context: Context, keep: Set<String>) {
        folder(context).listFiles()?.forEach { if (it.isFile && it.path !in keep && !it.name.startsWith(".")) it.delete() }
    }
}
