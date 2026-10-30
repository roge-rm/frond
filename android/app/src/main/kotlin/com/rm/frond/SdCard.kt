package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.security.MessageDigest

// The running core's SD card, through the control socket (native/DesktopControl.h sd-* commands).
// The card belongs to the session: the core keeps it in memory and saves it inside the .frond file,
// inserted or ejected, like the desktop. Files move in and out the desktop's way: the card is
// ejected, exported to a private image in files/sd-exchange/, edited there with FatImage, and the
// edited image is inserted again. Every call blocks; run them off the main thread.
class SdCard(private val context: Context, private val link: ControlLink) {
    enum class State { NONE, INSERTED, EJECTED, UNKNOWN }

    class Failed(message: String) : Exception(message)

    private val folder = File(context.filesDir, "sd-exchange")
    // The private copy being edited, and whether it differs from the card in the emulator.
    val working = File(folder, "card.img")
    var changed = false
        private set

    fun state(): State {
        val reply = link.request("sd-state", timeoutSeconds = 30)
        if (!reply.ok) return State.UNKNOWN
        return when (reply.message) {
            "inserted" -> State.INSERTED
            "ejected" -> State.EJECTED
            "none" -> State.NONE
            else -> State.UNKNOWN
        }
    }

    private fun run(command: String, argument: String = "") {
        val reply = link.request(command, argument, timeoutSeconds = 120)
        if (!reply.ok) throw Failed(reply.message)
    }

    fun create(mib: Int) = run("sd-create", mib.toString())
    fun eject() = run("sd-eject")
    fun reinsert() = run("sd-reinsert")

    // Inserts a picked image as the card. The core checks its size again.
    fun insertImage(uri: Uri) {
        folder.mkdirs()
        val copy = File(folder, "picked.img")
        try {
            if (!Firmware.copy(context, uri, copy)) throw Failed("Couldn't read that file.")
            val size = copy.length()
            if (size < FatImage.MIN_BYTES || size > FatImage.MAX_BYTES || size % 512 != 0L)
                throw Failed("Use a 32-128 MiB raw SD card image.")
            run("sd-insert", copy.path)
        } finally {
            copy.delete()
        }
    }

    // Writes the ejected card's image to a picked document.
    fun exportImage(to: Uri) {
        folder.mkdirs()
        val image = File(folder, "export.img")
        try {
            run("sd-export", image.path)
            val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
            output.use { o -> image.inputStream().use { it.copyTo(o, 1 shl 20) } }
        } finally {
            image.delete()
        }
    }

    // Starts editing: ejects the card if needed, exports it and checks it's a FAT16 card.
    fun beginEdit() {
        discard()
        folder.mkdirs()
        if (state() == State.INSERTED) eject()
        run("sd-export", working.path)
        FatImage.open(working, false).use { }
        changed = false
    }

    fun <T> read(block: (FatImage) -> T): T = FatImage.open(working, false).use(block)

    // Edits a copy of the working image and keeps it only when the edit checks out.
    private fun edit(block: (FatImage) -> Unit, verify: (FatImage) -> Unit) {
        val candidate = File(folder, "candidate.img")
        try {
            working.inputStream().channel.use { from ->
                candidate.outputStream().channel.use { to -> from.transferTo(0, from.size(), to) }
            }
            FatImage.open(candidate, true).use(block)
            FatImage.open(candidate, false).use(verify)
            if (!candidate.renameTo(working)) throw Failed("Couldn't save the edited card.")
            changed = true
        } finally {
            candidate.delete()
        }
    }

    // Copies a picked file into a card folder under its own name and checks the bytes on the card.
    fun addFile(path: String, uri: Uri): String {
        folder.mkdirs()
        val name = displayName(uri) ?: throw Failed("That file has no name.")
        FatImage.checkName(name)
        val source = File(folder, "incoming")
        try {
            if (!Firmware.copy(context, uri, source)) throw Failed("Couldn't read that file.")
            val expected = sha256(source)
            edit({ fat -> source.inputStream().buffered(1 shl 16).use { fat.add(path, name, it, source.length()) } }) { fat ->
                val entry = fat.find(joined(path, name)) ?: throw Failed("The file didn't land on the card.")
                val digest = MessageDigest.getInstance("SHA-256")
                fat.read(entry, object : java.io.OutputStream() {
                    override fun write(b: Int) = digest.update(b.toByte())
                    override fun write(b: ByteArray, off: Int, len: Int) = digest.update(b, off, len)
                })
                if (!digest.digest().contentEquals(expected)) throw Failed("The copy on the card didn't match the file.")
            }
            return name
        } finally {
            source.delete()
        }
    }

    fun makeFolder(path: String, name: String) {
        edit({ it.makeFolder(path, name.trim()) }) { fat ->
            if (fat.find(joined(path, name.trim()))?.directory != true) throw Failed("The folder didn't land on the card.")
        }
    }

    fun exportFile(entry: FatImage.Entry, to: Uri) {
        val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
        output.use { o -> read { it.read(entry, o) } }
    }

    // Puts the edited image in the emulator, or the untouched card back when nothing changed.
    fun finishEdit() {
        if (changed) run("sd-insert", working.path) else reinsert()
        discard()
    }

    // Drops the private copy; the card stays ejected.
    fun discard() {
        changed = false
        folder.listFiles().orEmpty().forEach { it.delete() }
    }

    private fun displayName(uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    } ?: uri.lastPathSegment?.substringAfterLast('/')

    companion object {
        fun joined(path: String, name: String) = if (path.endsWith("/")) path + name else "$path/$name"

        private fun sha256(file: File): ByteArray {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buffer = ByteArray(1 shl 16)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    digest.update(buffer, 0, n)
                }
            }
            return digest.digest()
        }

        // Removes a private copy left by a process that stopped mid-edit.
        fun clean(context: Context) {
            File(context.filesDir, "sd-exchange").listFiles().orEmpty().forEach { it.delete() }
        }

        fun message(e: Exception): String = when (e) {
            is Failed, is FatImage.FatException -> e.message ?: "Something went wrong."
            is IOException -> "Couldn't read or write the card image."
            else -> e.message ?: "Something went wrong."
        }
    }
}
