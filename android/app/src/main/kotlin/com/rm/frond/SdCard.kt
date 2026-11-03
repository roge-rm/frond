package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.security.MessageDigest

// Card images live in the core/session. Only SD images use the private FAT exchange copy.
// Every call blocks; run it off the main thread.
class SdCard(private val context: Context, private val link: ControlLink, private val session: String? = null,
             private val device: Device = Device.TX) {
    private val memoryStick = device.storageSlot == "memory-stick"
    private fun command(operation: String) = (if (memoryStick) "ms-" else "sd-") + operation
    enum class State { NONE, INSERTED, EJECTED, UNKNOWN }

    class Failed(message: String, val code: String? = null) : Exception(message)
    @Volatile private var jobCancelled = false
    @Volatile private var copyingInput: java.io.InputStream? = null
    private val exportGate = Any()
    private var publishingExport = false
    data class Cancellation(val input: java.io.InputStream?)
    fun beginJob() = synchronized(exportGate) { jobCancelled = false; publishingExport = false }
    fun cancelJob(): Cancellation? {
        val input = synchronized(exportGate) {
            if (publishingExport) return null
            jobCancelled = true
            link.cancelFiles()
            copyingInput
        }
        return Cancellation(input)
    }

    fun finishMutation() = synchronized(exportGate) {
        // Once the native write commits, cancellation cannot undo the folder refresh.
        publishingExport = true
        jobCancelled = false
    }

    private val folder = File(context.filesDir, if (memoryStick) "ms-images" else "sd-exchange")
    // The private copy being edited, and whether it differs from the card in the emulator.
    val working = File(folder, "card.img")
    var changed = false
        private set
    // Whether the card was in the Palm when the file view opened.
    private var wasInserted = true

    fun state(): State {
        val reply = link.request(command("state"), timeoutSeconds = 30)
        if (!reply.ok) return State.UNKNOWN
        return when (reply.message) {
            "inserted" -> State.INSERTED
            "ejected" -> State.EJECTED
            "none" -> State.NONE
            else -> State.UNKNOWN
        }
    }

    private fun run(command: String, argument: String = "") {
        if (jobCancelled) throw Failed("File transfer cancelled.", "cancelled")
        val reply = link.request(command, argument, timeoutSeconds = if (command == "ms-get" || command == "ms-put") 240 else 120,
            cancelRequested = { jobCancelled })
        if (!reply.ok) throw Failed(if (reply.code == "not-ready") "Open Date Book on the Palm, then try again." else reply.message, reply.code)
    }

    fun create(mib: Int) {
        if (memoryStick && mib !in device.memoryStickSizes) throw Failed("Use a Memory Stick size from the list.")
        run(command("create"), mib.toString())
    }
    fun eject() = run(command("eject"))
    fun reinsert() = run(command("reinsert"))
    // Whether the stick in the slot is a Memory Stick Pro ("pro N" from ms-type).
    fun memoryStickPro(): Boolean {
        if (!memoryStick) return false
        val reply = link.request("ms-type", timeoutSeconds = 30)
        return reply.ok && reply.message.startsWith("pro ")
    }
    fun protection(): Boolean? {
        if (!memoryStick || state() == State.NONE) return null
        val reply = link.request("ms-protection", timeoutSeconds = 30)
        return if (!reply.ok) null else when (reply.message) {
            "read-only" -> true
            "writable" -> false
            else -> null
        }
    }
    fun protect(on: Boolean) {
        if (!memoryStick) throw Failed("Write protection is not available for this card.")
        run("ms-read-only", if (on) "on" else "off")
    }

    // Inserts a picked image as the card. The core checks its size again.
    fun insertImage(uri: Uri) {
        folder.mkdirs()
        val copy = File(folder, "picked.img")
        try {
            // Frond classic stick files, or raw Memory Stick Pro images.
            val stickFiles = device.msSizes.map { MEMORY_STICK_HEADER_BYTES + Device.memoryStickPhysicalBytes(it) } +
                Device.MEMORY_STICK_PRO_SIZES.filter { it <= device.msProMax }.map { it.toLong() shl 20 }
            val limit = if (memoryStick) stickFiles.maxOrNull() ?: 0L else device.sdImageMax
            val problem = if (memoryStick && device.msProMax > 0) "Use a Frond classic Memory Stick image or a raw Memory Stick Pro image."
                else if (memoryStick) "Use a Frond classic Memory Stick image from 4 to 128 MB."
                else "Use a 32 MB to ${Device.sizeLabel(device.sdImageMax)} raw SD card image."
            if (!copyIn(uri, copy, limit)) throw Failed(problem)
            val size = copy.length()
            if (memoryStick && size !in stickFiles ||
                !memoryStick && (size < FatImage.MIN_BYTES || size > limit || size % 512 != 0L)) throw Failed(problem)
            run(command("insert"), copy.path)
        } finally {
            copy.delete()
        }
    }

    // Writes the ejected card's image to a picked document.
    fun exportImage(to: Uri) {
        folder.mkdirs()
        val image = File(folder, "export.img")
        try {
            run(command("export"), image.path)
            val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
            output.use { o -> image.inputStream().use { it.copyTo(o, 1 shl 20) } }
        } finally {
            image.delete()
        }
    }

    // Starts editing: ejects the card if needed, saves a recovery session like the desktop, exports the
    // card and checks it's a FAT16 card. Refuses to start when there isn't room for the recovery copy.
    fun beginEdit() {
        if (memoryStick) return
        folder.mkdirs()
        if (pendingFor(context, session) != null && !restorePending(context, link, session)) {
            throw Failed("The last edit of this card wasn't put back in the Palm. Close the emulator and open it again.")
        }
        discard()
        Sessions.adoptRecovery(context)
        val owner = Sessions.forPath(context, session)
        val device = owner?.device ?: Device.TX
        // The recovery copy, plus the card's working copy and the copy each edit makes.
        val needs = exchangeNeeds(owner?.takeIf { it.saved }?.size, device, session?.let { File(it).parentFile })
        val free = context.filesDir.usableSpace
        if (free < needs) {
            throw Failed("There isn't room to save a recovery session before the exchange. Free up about ${(needs - free + (1L shl 20) - 1) shr 20} MB and try again.")
        }
        val inserted = state() == State.INSERTED
        wasInserted = inserted
        try {
            if (inserted) eject()
            Sessions.saveRecovery(context, link, owner?.name ?: device.label, session)?.let {
                throw Failed("Couldn't save a recovery session, so the exchange didn't start. $it")
            }
        } catch (e: Exception) {
            if (inserted) try { reinsert() } catch (_: Exception) { }
            throw e
        }
        try {
            run("sd-export", working.path)
            FatImage.open(working, false).use { }
        } catch (e: Exception) {
            // Leave the card as it was found and keep the recovery session.
            File(folder, "card.img").delete()
            if (inserted) try { reinsert() } catch (_: Exception) { }
            throw Failed(message(e) + kept(Sessions.adoptRecovery(context)))
        }
        changed = false
    }

    fun list(path: String): Pair<List<FatImage.Entry>, Long> {
        if (!memoryStick) return read { it.list(path) to it.freeBytes }
        if (jobCancelled) throw Failed("File transfer cancelled.", "cancelled")
        val reply = link.request("ms-files", path, timeoutSeconds = 120, cancelRequested = { jobCancelled })
        if (!reply.ok) throw Failed(if (reply.code == "not-ready") "Open Date Book on the Palm, then try again." else reply.message, reply.code)
        val lines = reply.message.lines().filter { it.isNotEmpty() }
        val space = lines.firstOrNull()?.split('\t') ?: throw Failed("The Palm returned no card space.")
        if (space.size != 3 || space[0] != "space") throw Failed("The Palm returned invalid card space.")
        val used = space[1].toLongOrNull() ?: throw Failed("The Palm returned invalid card space.")
        val total = space[2].toLongOrNull() ?: throw Failed("The Palm returned invalid card space.")
        if (used < 0 || total < used || total > (2048L shl 20)) throw Failed("The Palm returned invalid card space.")
        val entries = lines.drop(1).map { line ->
            val fields = line.split('\t')
            if (fields.size != 3 || fields[0] !in listOf("d", "f")) throw Failed("The Palm returned an invalid folder entry.")
            val size = fields[1].toLongOrNull() ?: throw Failed("The Palm returned an invalid file size.")
            if (size < 0 || size > total) throw Failed("The Palm returned an invalid file size.")
            val name = decodeName(fields[2])
            FatImage.Entry(name, name, fields[0] == "d", size, 0)
        }
        if (entries.size > 512) throw Failed("This folder has more than 512 entries.")
        if (entries.map { it.name.lowercase(java.util.Locale.ROOT) }.toSet().size != entries.size)
            throw Failed("The Palm returned duplicate filenames.")
        return entries to (total - used)
    }

    private fun guestPair(host: String, card: String) = hex(host) + "\n" + hex(card)

    fun <T> read(block: (FatImage) -> T): T {
        if (memoryStick) throw Failed("File exchange is not available for Memory Stick images.")
        return FatImage.open(working, false).use(block)
    }

    // Edits a copy of the working image and keeps it only when the edit checks out.
    private fun edit(block: (FatImage) -> Unit, verify: (FatImage) -> Unit) {
        if (memoryStick) throw Failed("File exchange is not available for Memory Stick images.")
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
        if (memoryStick) checkMemoryStickName(name) else FatImage.checkName(name)
        val source = File(folder, "incoming")
        try {
            if (memoryStick) {
                if (!copyIn(uri, source, MEMORY_STICK_FILE_BYTES)) throw Failed("File exchange supports files up to 8 MB.")
                run("ms-put", guestPair(source.path, joined(path, name)))
                return name
            }
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
        if (memoryStick) { checkMemoryStickName(name); run("ms-mkdir", joined(path, name)); return }
        edit({ it.makeFolder(path, name.trim()) }) { fat ->
            if (fat.find(joined(path, name.trim()))?.directory != true) throw Failed("The folder didn't land on the card.")
        }
    }

    fun renameEntry(path: String, oldName: String, newName: String) {
        if (!memoryStick) throw Failed("Renaming is not available in this file view.")
        checkMemoryStickName(oldName); checkMemoryStickName(newName)
        run("ms-rename", hex(joined(path, oldName)) + "\n" + hex(newName))
    }

    fun deleteEntry(path: String, name: String) {
        if (!memoryStick) throw Failed("Deleting is not available in this file view.")
        checkMemoryStickName(name)
        run("ms-delete", joined(path, name))
    }

    fun exportFile(entry: FatImage.Entry, to: Uri, path: String = "/", publishing: () -> Unit = {}) {
        if (memoryStick) {
            folder.mkdirs()
            val exported = File(folder, "file-export")
            try {
                run("ms-get", guestPair(exported.path, joined(path, entry.name)))
                synchronized(exportGate) {
                    if (jobCancelled) throw Failed("File transfer cancelled.", "cancelled")
                    // A document provider cannot promise atomic replacement once publication starts.
                    publishingExport = true
                }
                publishing()
                val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
                output.use { out -> exported.inputStream().use { it.copyTo(out, 1 shl 16) } }
            } finally { exported.delete() }
            return
        }
        val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
        output.use { o -> read { it.read(entry, o) } }
    }

    // Puts the edited image in the emulator, or the untouched card back when nothing changed.
    // The recovery session goes once the card is back; a failed insert keeps it as a session.
    fun finishEdit() {
        if (memoryStick) return
        try {
            if (changed) run("sd-insert", working.path) else if (wasInserted) reinsert()
        } catch (e: Failed) {
            throw Failed(message(e) + kept(Sessions.adoptRecovery(context)))
        }
        discard()
        Sessions.dropRecovery(context)
    }

    // Leaves the exchange without the changes; the card stays ejected and the recovery session goes.
    fun cancelEdit() {
        if (memoryStick) return
        discard()
        Sessions.dropRecovery(context)
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
            copyingInput = input
            return input.use { BoundedCardImage.copy(it, to, limit) { jobCancelled } }
        } catch (e: IOException) {
            to.delete()
            throw Failed(if (jobCancelled) "File transfer cancelled." else "Couldn't read that file.", if (jobCancelled) "cancelled" else null)
        } finally { copyingInput = null }
    }

    private fun displayName(uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    } ?: uri.lastPathSegment?.substringAfterLast('/')

    companion object {
        // Versioned 64-byte header and classic physical page records; the core checks CRC and boot blocks.
        private const val MEMORY_STICK_HEADER_BYTES = 64L
        private const val MEMORY_STICK_FILE_BYTES = 8L shl 20
        // The core's candidate card file (uarm/SdCandidate.h), live while its machine runs.
        const val LIVE_CANDIDATE = ".frondsd.tmp-live-"
        private fun hex(text: String) = text.toByteArray(Charsets.UTF_8).joinToString("") { "%02x".format(it) }
        private fun checkMemoryStickName(name: String) {
            if (name.isEmpty() || name.length > 255 || name in listOf(".", "..") ||
                name.first() == ' ' || name.last() in listOf(' ', '.') ||
                name.any { it.code !in 32..126 || it in "/\\:*?\"<>|" })
                throw Failed("Use a printable ASCII filename without reserved characters.")
        }
        private fun decodeName(value: String): String {
            if (value.length % 2 != 0 || value.length > 510 || value.any { it !in "0123456789abcdef" })
                throw Failed("The Palm returned an invalid filename.")
            val name = ByteArray(value.length / 2) { value.substring(it * 2, it * 2 + 2).toInt(16).toByte() }.toString(Charsets.US_ASCII)
            checkMemoryStickName(name)
            return name
        }

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
            // A recovery session left by an exchange that never finished.
            Sessions.adoptRecovery(context)
            File(context.filesDir, "ms-images").deleteRecursively()
            val folder = File(context.filesDir, "sd-exchange")
            val marker = File(folder, PENDING)
            val owner = if (marker.isFile) marker.readText() else null
            val keep = owner != null && File(owner).isFile
            folder.listFiles().orEmpty().forEach {
                if (!(keep && (it.name == "card.img" || it.name == PENDING))) it.delete()
            }
        }

        // Says where the recovery session went, after a failed exchange.
        // The running card's size from its machine folder: a live candidate (cards above 128 MB before
        // their first save) or sd.frondsd, else the largest card kept in memory.
        fun cardBytes(folder: File?): Pair<Long, Boolean> {
            val files = folder?.listFiles().orEmpty()
            val live = files.filter { it.isFile && it.name.startsWith(LIVE_CANDIDATE) }.maxOfOrNull { it.length() }
            val saved = files.firstOrNull { it.isFile && it.name == "sd.frondsd" }?.length()
            return maxOf(live ?: 0L, saved ?: 0L, FatImage.SMALL_MAX_BYTES) to (live != null)
        }

        // Room an exchange needs: the recovery save (with the card again when it isn't in the saved session
        // yet), the exported working copy and the copy each edit makes.
        fun exchangeNeeds(savedSize: Long?, device: Device, folder: File?): Long {
            val (card, live) = cardBytes(folder)
            val saved = savedSize?.takeIf { it > 0 }
            return Sessions.recoveryNeeds(saved, device) + (if (live || saved == null) card else 0L) + 2 * card
        }

        fun kept(name: String?) = if (name == null) "" else "\n\nThe session from before the exchange is on the home screen as \"$name\"."

        fun message(e: Exception): String = when (e) {
            is Failed, is FatImage.FatException -> e.message ?: "Something went wrong."
            is IOException -> "Couldn't read or write the file."
            else -> e.message ?: "Something went wrong."
        }
    }
}
