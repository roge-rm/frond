package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException

// Installs, launches and exports Palm databases in the running core, like the desktop's File menu
// (install, launch-app, export and export-db on the control socket). Files pass through private
// copies in the app cache, as the core only takes paths. Every call blocks; run them off the main thread.
class PalmDatabases(private val context: Context, private val link: ControlLink) {
    class Failed(message: String) : Exception(message)

    // A RAM database as the core lists it, from its database header.
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
        val reply = link.request("launch-app", name, timeoutSeconds = 120)
        if (!reply.ok) throw Failed(if (reply.code == "launch") launchFailed(name) else message(reply))
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

    // The RAM databases, from the core's database directory (db-list), sorted by name.
    fun list(): List<Info> = parseList(run("db-list", "").message)

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
        // dmHdrAttrResDB: a resource database (.prc).
        private const val RESOURCE = 0x0001

        // db-list's reply: per line, type, creator and attributes in hex, then the name's bytes in hex.
        // Lines that don't parse are left out.
        fun parseList(text: String): List<Info> {
            fun bytes(hex: String) = ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
            fun code(hex: String) = String(bytes(hex), Charsets.ISO_8859_1)
            val found = text.lineSequence().mapNotNull { line ->
                val parts = line.split(' ')
                if (parts.size != 4 || parts[0].length != 8 || parts[1].length != 8 || parts[3].isEmpty() ||
                    parts[3].length % 2 != 0) return@mapNotNull null
                try {
                    val attributes = parts[2].toInt(16)
                    Info(String(bytes(parts[3]), Charsets.ISO_8859_1), code(parts[0]), code(parts[1]), attributes and RESOURCE != 0)
                } catch (e: NumberFormatException) {
                    null
                }
            }
            return found.toList().sortedBy { it.name.lowercase() }
        }

        // The core couldn't find or open the application. Built-in ones go by their database name,
        // which often isn't the name in the launcher.
        fun launchFailed(name: String) =
            "Couldn't find an application with the database name \"$name\", or Palm OS is still starting. " +
                "Built-in applications often have other database names: Memos is Memos-PMem."

        // The core's reply as a message. Not ready means the guest is in the middle of something.
        fun message(reply: ControlLink.Reply): String =
            if (reply.code == "not-ready") "Palm OS isn't ready. Go back to the Palm, let it finish what it's doing, then try again."
            else reply.message
    }
}
