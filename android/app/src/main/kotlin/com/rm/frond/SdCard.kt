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
class SdCard(private val context: Context, private val link: ControlLink, private val session: String? = null) {
    enum class State { NONE, INSERTED, EJECTED, UNKNOWN }

    class Failed(message: String) : Exception(message)

    private val folder = File(context.filesDir, "sd-exchange")
    // The private copy being edited, and whether it differs from the card in the emulator.
    val working = File(folder, "card.img")
    var changed = false
        private set
    // Whether the card was in the Palm when the file view opened.
    private var wasInserted = true

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
            if (!copyIn(uri, copy, FatImage.MAX_BYTES)) throw Failed("Use a 32-128 MiB raw SD card image.")
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
        folder.mkdirs()
        if (pendingFor(context, session) != null && !restorePending(context, link, session)) {
            throw Failed("The last edit of this card wasn't put back in the Palm. Close the emulator and open it again.")
        }
        discard()
        val inserted = state() == State.INSERTED
        wasInserted = inserted
        try {
            if (inserted) eject()
            run("sd-export", working.path)
            FatImage.open(working, false).use { }
        } catch (e: Exception) {
            // Leave the card as it was found.
            File(folder, "card.img").delete()
            if (inserted) try { reinsert() } catch (_: Exception) { }
            throw e
        }
        changed = false
    }

    fun <T> read(block: (FatImage) -> T): T = FatImage.open(working, false).use(block)

    // Edits a copy of the working image and keeps it only when the edit checks out.
    private fun edit(block: (FatImage) -> Unit, verify: (FatImage) -> Unit) {
        val candidate = File(folder, "candidate.img")
        try {
            working.inputStream().channel.use { from ->
                candidate.outputStream().channel.use { to ->
                    var at = 0L
                    val size = from.size()
                    while (at < size) {
                        val n = from.transferTo(at, size - at, to)
                        if (n <= 0) throw Failed("Couldn't copy the card image.")
                        at += n
                    }
                }
            }
            FatImage.open(candidate, true).use(block)
            FatImage.open(candidate, false).use(verify)
            if (!candidate.renameTo(working)) throw Failed("Couldn't save the edited card.")
            changed = true
            // If the app is killed from here, the next start of this session puts the card back.
            session?.let { File(folder, PENDING).writeText(it) }
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
            val room = read { it.freeBytes }
            if ((sizeOf(uri) ?: 0L) > room || !copyIn(uri, source, room)) throw Failed("There isn't room on the card for that file.")
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
        if (changed) run("sd-insert", working.path) else if (wasInserted) reinsert()
        discard()
    }

    // Drops the private copy; the card stays ejected.
    fun discard() {
        changed = false
        folder.listFiles().orEmpty().forEach { it.delete() }
    }

    private fun sizeOf(uri: Uri): Long? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else null
        }
    } catch (e: Exception) {
        null
    }

    // Copies a picked file, stopping when it's bigger than limit. False when it's too big; throws Failed when it can't be read.
    private fun copyIn(uri: Uri, to: File, limit: Long): Boolean {
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw Failed("Couldn't read that file.")
            input.use { i ->
                to.outputStream().use { o ->
                    val buffer = ByteArray(1 shl 16)
                    var total = 0L
                    while (true) {
                        val n = i.read(buffer)
                        if (n < 0) break
                        total += n
                        if (total > limit) {
                            to.delete()
                            return false
                        }
                        o.write(buffer, 0, n)
                    }
                }
            }
            return true
        } catch (e: IOException) {
            to.delete()
            throw Failed("Couldn't read that file.")
        }
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

        private const val PENDING = "pending"

        // The edited card kept for a session that was killed in the file view, or null.
        fun pendingFor(context: Context, session: String?): File? {
            val folder = File(context.filesDir, "sd-exchange")
            val card = File(folder, "card.img")
            val marker = File(folder, PENDING)
            return if (session != null && card.isFile && marker.isFile && marker.readText() == session) card else null
        }

        // Inserts the edited card kept after a kill, once the session is running again.
        // Keeps it when the insert fails.
        fun restorePending(context: Context, link: ControlLink, session: String?): Boolean {
            val card = pendingFor(context, session) ?: return true
            val reply = link.request("sd-insert", card.path, timeoutSeconds = 120)
            if (!reply.ok) return false
            File(card.parentFile, PENDING).delete()
            card.delete()
            return true
        }

        // Removes private copies left by a process that stopped mid-edit. The edited card of a session
        // that still exists stays until it has been inserted.
        fun clean(context: Context) {
            val folder = File(context.filesDir, "sd-exchange")
            val marker = File(folder, PENDING)
            val owner = if (marker.isFile) marker.readText() else null
            val keep = owner != null && File(owner).isFile
            folder.listFiles().orEmpty().forEach {
                if (!(keep && (it.name == "card.img" || it.name == PENDING))) it.delete()
            }
        }

        fun message(e: Exception): String = when (e) {
            is Failed, is FatImage.FatException -> e.message ?: "Something went wrong."
            is IOException -> "Couldn't read or write the card image."
            else -> e.message ?: "Something went wrong."
        }
    }
}
