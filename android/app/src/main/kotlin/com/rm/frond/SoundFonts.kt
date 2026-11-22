package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.io.InputStream

// SoundFonts picked for the Zodiac's Music output, copied into the app's files so the core can read them by path.
// One copy per file name: picking a file with the same name again replaces it.
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
        val target = File(folder(context), cleanName(name))
        val partial = File(folder(context), ".${target.name}.part")
        folder(context).mkdirs()
        val input = resolver.openInputStream(uri) ?: throw IOException("Couldn't open the file.")
        try {
            input.use { copy(it, partial, size, cancelled, progress) }
            if (cancelled()) throw IOException(COPY_CANCELLED)
            if (!partial.renameTo(target)) throw IOException("Couldn't keep the SoundFont.")
        } finally {
            partial.delete()
        }
        return target.path
    }

    private fun copy(input: InputStream, to: File, size: Long, cancelled: () -> Boolean, progress: (Long, Long) -> Unit) {
        val buffer = ByteArray(1 shl 16)
        var head = 0
        while (head < 12) {
            val n = input.read(buffer, head, 12 - head)
            if (n < 0) break
            head += n
        }
        if (!isSoundFont(buffer.copyOf(head))) throw IOException("That isn't a SoundFont (.sf2) file.")
        var done = head.toLong()
        to.outputStream().use { out ->
            out.write(buffer, 0, head)
            while (!cancelled()) {
                val n = input.read(buffer)
                if (n < 0) break
                out.write(buffer, 0, n)
                done += n
                progress(done, size)
            }
        }
    }

    // Deletes the copies no machine uses. keep holds the paths in use.
    fun prune(context: Context, keep: Set<String>) {
        folder(context).listFiles()?.forEach { if (it.isFile && it.path !in keep && !it.name.startsWith(".")) it.delete() }
    }
}
