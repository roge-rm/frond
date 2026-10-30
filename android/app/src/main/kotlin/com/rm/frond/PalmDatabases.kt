package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream

// Installs, launches and exports Palm databases in the running core, like the desktop's File menu
// (install, launch-app, export and export-db on the control socket). Files pass through private
// copies in the app cache, as the core only takes paths. Every call blocks; run them off the main thread.
class PalmDatabases(private val context: Context, private val link: ControlLink) {
    class Failed(message: String) : Exception(message)

    // A database from the header of its .prc or .pdb.
    data class Info(val name: String, val type: String, val creator: String, val resource: Boolean) {
        val application get() = resource && type == "appl"
    }

    private val folder = File(context.cacheDir, "databases")

    private fun run(command: String, argument: String): ControlLink.Reply {
        val reply = link.request(command, argument, timeoutSeconds = 120)
        if (!reply.ok) throw Failed(message(reply))
        return reply
    }

    // Installs a picked .prc or .pdb and returns the core's result, e.g. "Application installed.".
    fun install(uri: Uri): String {
        folder.mkdirs()
        val copy = File(folder, "install.pdb")
        try {
            copyIn(uri, copy)
            return run("install", copy.path).message
        } finally {
            copy.delete()
        }
    }

    fun launch(name: String) {
        run("launch-app", name)
    }

    // Writes every RAM database, or the one named, as a ZIP to a picked document.
    // The picked document is removed again when the export fails, so no empty file is left.
    fun export(to: Uri, name: String? = null) {
        try {
            withExport(name) { zip ->
                val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
                output.use { o -> zip.inputStream().use { it.copyTo(o, 1 shl 16) } }
            }
        } catch (e: Exception) {
            try { DocumentsContract.deleteDocument(context.contentResolver, to) } catch (_: Exception) {}
            throw e
        }
    }

    // The RAM databases, read from an export of all of them.
    fun list(): List<Info> = withExport(null) { zip -> zip.inputStream().use { readArchive(it) } }

    private fun <T> withExport(name: String?, use: (File) -> T): T {
        folder.mkdirs()
        val zip = File(folder, "export.zip")
        try {
            if (name == null) run("export", zip.path) else run("export-db", zip.path + "\n" + name)
            return use(zip)
        } finally {
            zip.delete()
        }
    }

    private fun copyIn(uri: Uri, to: File) {
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw Failed("Couldn't read that file.")
            input.use { i -> to.outputStream().use { i.copyTo(it, 1 shl 16) } }
        } catch (e: IOException) {
            to.delete()
            throw Failed("Couldn't read that file.")
        }
    }

    companion object {
        const val HEADER_BYTES = 78
        // dmHdrAttrResDB: a resource database (.prc).
        private const val RESOURCE = 0x0001

        // Name, attributes, type and creator from a Palm database header, or null if it's too short.
        fun header(bytes: ByteArray): Info? {
            if (bytes.size < HEADER_BYTES) return null
            val end = (0 until 32).firstOrNull { bytes[it] == 0.toByte() } ?: 32
            if (end == 0) return null
            val name = String(bytes, 0, end, Charsets.ISO_8859_1)
            val attributes = ((bytes[32].toInt() and 0xff) shl 8) or (bytes[33].toInt() and 0xff)
            fun code(at: Int) = String(bytes, at, 4, Charsets.ISO_8859_1)
            return Info(name, code(60), code(64), attributes and RESOURCE != 0)
        }

        // The databases in an export ZIP, sorted by name.
        fun readArchive(input: InputStream): List<Info> {
            val found = mutableListOf<Info>()
            ZipInputStream(input).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue
                    val head = ByteArray(HEADER_BYTES)
                    var got = 0
                    while (got < HEADER_BYTES) {
                        val n = zip.read(head, got, HEADER_BYTES - got)
                        if (n < 0) break
                        got += n
                    }
                    if (got == HEADER_BYTES) header(head)?.let { found += it }
                }
            }
            return found.sortedBy { it.name.lowercase() }
        }

        // The core's reply as a message. Not ready means the guest is in the middle of something.
        fun message(reply: ControlLink.Reply): String =
            if (reply.code == "not-ready") "Palm OS isn't ready. Go back to the Palm, let it finish what it's doing, then try again."
            else reply.message
    }
}
