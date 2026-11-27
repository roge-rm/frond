package com.rm.frond

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException
import java.security.MessageDigest

// Card images live in the core/session. The file view takes the ejected card out of the core into
// the machine folder's card file (sd.frondsd, ms.frondms or ms.frondmsp), edits that file in place
// (CardFile) and puts it back in. Every call blocks; run it off the main thread.
// frond-68k (the Dana, VZ Palms and T415) links card files by path: its put-in is attach with the
// file's path and key (Cards.attachArgument), its reinsert is insert, and its state replies go on
// with the card's key and flags.
class SdCard(private val context: Context, private val link: ControlLink, private val session: String? = null,
             private val device: Device = Device.TX) {
    // The slot the calls work on: 0, or 1 for the Zodiac's second SD slot (the core's sd2- commands,
    // its card in sd2.frondsd). Changed only while no card is out for the file view.
    @Volatile var slot = 0
        private set
    fun select(n: Int): Boolean {
        if (cardFile != null || identity != null) return n == slot
        slot = n
        return true
    }
    private val stickDevice = device.storageSlot == "memory-stick"
    private val memoryStick get() = stickDevice && slot == 0
    private val m68k = device.runs68k
    private val compactFlash get() = device.zaurus && slot == 1
    private fun command(operation: String, n: Int = slot): String {
        val op = if (!m68k) operation else when (operation) { "reinsert" -> "insert"; "put-in" -> "attach"; else -> operation }
        return when {
            n == 1 && device.zaurus -> "cf-$op"
            n != 0 -> "sd2-$op"
            stickDevice -> "ms-$op"
            else -> "sd-$op"
        }
    }
    enum class State { NONE, INSERTED, EJECTED, PALM_WIFI, UNKNOWN }

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

    val cancelled: Boolean get() = jobCancelled

    private fun <T> cancellable(block: () -> T): T = try {
        block()
    } catch (e: FatImage.Cancelled) {
        throw Failed("File transfer cancelled.", "cancelled")
    }

    // Whether the card has a folder at path.
    fun hasFolder(path: String): Boolean = try {
        read { it.find(path)?.directory == true }
    } catch (e: Exception) {
        false
    }

    fun finishMutation() = synchronized(exportGate) {
        // Once the native write commits, cancellation cannot undo the folder refresh.
        publishingExport = true
        jobCancelled = false
    }

    private val folder = File(context.filesDir, "sd-exchange")
    // The card file open for the file view, and whether a change was committed to it.
    @Volatile private var cardFile: CardFile? = null
    var changed = false
        private set
    // Whether the card was in the Palm when the file view opened, and what the take-out reported.
    private var wasInserted = true
    private var identity: String? = null
    private var protectedCard = false

    fun state(): State = stateOf(slot)

    fun stateOf(n: Int): State {
        val reply = link.request(command("state", n), timeoutSeconds = 30)
        if (!reply.ok) return State.UNKNOWN
        return parseState(reply.message, m68k)
    }

    private fun run(command: String, argument: String = "") {
        if (jobCancelled) throw Failed("File transfer cancelled.", "cancelled")
        val reply = link.request(command, argument, timeoutSeconds = 120, cancelRequested = { jobCancelled })
        if (!reply.ok) throw Failed(if (reply.code == "not-ready") "Open Date Book on the Palm, then try again." else reply.message, reply.code)
    }

    fun eject() = run(command("eject"))
    fun reinsert() = run(command("reinsert"))
    fun insertPalmWifi() = run("sd-wifi-insert")
    fun ejectPalmWifi() = run("sd-wifi-eject")

    // A library card the core left out at start (changed while the machine was off), slot `n`:
    // `argument` is Cards.secondPutIn's, or Cards.attachArgument's on frond-68k.
    fun putInAtStart(n: Int, argument: String) {
        val reply = link.request(command("put-in", n), argument, timeoutSeconds = 120)
        if (!reply.ok) throw Failed(message68k(reply.message), reply.code)
    }

    // Settles the ejected card in the machine folder's file and empties the slot. Returns the
    // card's serial (64 hex digits) and whether it's write protected, as the core reports them.
    // frond-68k only says taken-out, so its key and protection are read from the slot first.
    fun takeOut(): Pair<String?, Boolean> {
        val before = if (m68k) link.request(command("state"), timeoutSeconds = 30).takeIf { it.ok }?.message?.split(' ') else null
        val reply = link.request(command("take-out"), timeoutSeconds = 240)
        if (!reply.ok) throw Failed(message68k(reply.message), reply.code)
        if (before != null) return before.getOrNull(1) to (before.getOrNull(2) == "protected")
        val parts = reply.message.split(' ')
        return parts.getOrNull(0)?.takeIf { it.length == 64 } to (parts.getOrNull(1) == "protected")
    }

    // Opens the card file just moved into the machine folder and inserts it. frond-68k takes the
    // file by path, with the key and protection its Cards entry has.
    fun putIn(identity: String, protected: Boolean) {
        val argument = if (!m68k) identity + if (protected) " protected" else " writable"
            else attachArgument() ?: throw Failed("Couldn't find the card's file.")
        val reply = link.request(command("put-in"), argument, timeoutSeconds = 120)
        if (!reply.ok) throw Failed(message68k(reply.message), reply.code)
    }
    private fun attachArgument(): String? {
        val dir = session?.let { File(it).absoluteFile.parentFile } ?: return null
        return Cards.attachArgument(Sessions.folder(context), dir, slot)
    }
    // Plain text for frond-68k's card refusals; others pass through as they are.
    private fun message68k(core: String) = when {
        !m68k -> core
        "duplicate identity or serial" in core -> "That card is already in the other slot."
        "cannot lock card file" in core -> "That card is open somewhere else."
        else -> core
    }
    // Whether the stick in the slot is a Memory Stick Pro ("pro N" from ms-type).
    fun memoryStickPro(): Boolean {
        if (!memoryStick) return false
        val reply = link.request("ms-type", timeoutSeconds = 30)
        return reply.ok && reply.message.startsWith("pro ")
    }
    // While Files… has the stick taken out the slot is empty, so the take-out's answer stands.
    fun protection(n: Int, present: Boolean): Boolean? {
        if (!(stickDevice && n == 0) && !device.sdWriteProtection) return null
        if (cardFile != null && n == slot) return protectedCard
        if (!present) return null
        if (m68k) {
            val reply = link.request(command("state", n), timeoutSeconds = 30)
            val words = reply.message.split(' ')
            return if (reply.ok && words.size >= 3) words[2] == "protected" else null
        }
        val reply = link.request(command("protection", n), timeoutSeconds = 30)
        return if (!reply.ok) null else when (reply.message) {
            "read-only" -> true
            "writable" -> false
            else -> null
        }
    }
    fun protect(on: Boolean) {
        if (!memoryStick && !device.sdWriteProtection) throw Failed("Write protection is not available for this card.")
        run(command(if (compactFlash) "protect" else "read-only"), if (on) "on" else "off")
    }

    fun insertCompactFlash(uri: Uri) {
        if (!compactFlash) throw Failed("Choose the CompactFlash slot.")
        val state = state()
        if (state != State.NONE && state != State.EJECTED) throw Failed("Eject the CompactFlash card first.")
        folder.mkdirs()
        val image = File(folder, "incoming-cf.img")
        try {
            if (!copyIn(uri, image, ZaurusImages.MAX_EXTERNAL_CF_BYTES) || image.length() < 512 || image.length() % 512 != 0L)
                throw Failed("Choose a raw CompactFlash image up to 2 GiB with 512-byte sectors.")
            run("cf-insert", image.absolutePath)
        } finally {
            image.delete()
        }
    }

    fun queueZaurusPackage(uri: Uri) {
        if (!device.zaurus || state() != State.EJECTED || protection(slot, present = true) != false)
            throw Failed("Eject a writable card first.")
        val name = displayName(uri) ?: throw Failed("That file has no name.")
        if (!name.endsWith(".ipk", ignoreCase = true)) throw Failed("Choose an IPK package.")
        FatImage.checkName(name)
        folder.mkdirs()
        val dir = java.nio.file.Files.createTempDirectory(folder.toPath(), "ipk-").toFile()
        try {
            val file = File(dir, name)
            val limit = minOf(ZaurusImages.MAX_EXTERNAL_CF_BYTES, dir.usableSpace - JOURNAL_ROOM)
            if (limit <= 0 || !copyIn(uri, file, limit)) throw Failed("There isn't room to copy that package.")
            val medium = if (compactFlash) "cf" else "sd"
            run("zaurus-install", "$medium ${file.absolutePath}")
        } finally {
            dir.deleteRecursively()
        }
    }

    // Writes the ejected card's image to a picked document. frond-68k writes a card file
    // (.frondsd or .frondms), only to a new file.
    fun exportImage(to: Uri) {
        folder.mkdirs()
        val image = File(folder, if (!m68k) "export.img" else if (memoryStick) "export.frondms" else "export.frondsd")
        image.delete()
        try {
            run(command("export"), if (m68k) Cards.quotePath(image.absolutePath) else image.path)
            val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
            output.use { o -> image.inputStream().use { it.copyTo(o, 1 shl 20) } }
        } finally {
            image.delete()
        }
    }

    // Starts editing: ejects the card if needed, saves the session in its folder, marks the slot swapped
    // and takes the card out into the folder's card file, then opens that file. A kill from here on
    // loads the machine with --card-swap and the card as the file holds it.
    fun beginEdit() {
        folder.mkdirs()
        if (slot == 0 && pendingFor(context, session) != null && !restorePending(context, link, session)) {
            throw Failed("The last edit of this card wasn't put back in the Palm. Close the emulator and open it again.")
        }
        discard()
        val path = session ?: throw Failed("Save the machine before opening its card.")
        val dir = File(path).absoluteFile.parentFile ?: throw Failed("Save the machine before opening its card.")
        // A frond-68k card still linked from where an older Frond kept it opens once it's in Cards.
        if (m68k && Cards.cardFileIn(dir, slot) == null) throw Failed("Close the machine and open it again to move this card into Cards first.")
        val before = state()
        if (device.zaurus && before != State.EJECTED) throw Failed("Eject the card in Qtopia and in Frond before opening its files.")
        val inserted = before == State.INSERTED
        wasInserted = inserted
        try {
            if (inserted) eject()
            val saved = link.request("save", path, timeoutSeconds = 120)
            if (!saved.ok) throw Failed("Couldn't save the machine, so the card wasn't opened. ${saved.message}")
            if (compactFlash) {
                compactFlashIdentity(inserted).write(dir)
            } else {
                val saved2 = Cards.readSlot(dir, slot)
                if (!saved2.swap && !Cards.writeSlot(dir, saved2.copy(swap = true), slot)) throw Failed("Couldn't open the card.")
            }
            editing = dir
            editingSlot = slot
            val (id, prot) = takeOut()
            identity = id
            protectedCard = prot
        } catch (e: Exception) {
            editing = null
            if (inserted) try { reinsert() } catch (_: Exception) { }
            throw e
        }
        try {
            val file = if (compactFlash) File(dir, ZaurusImages.EXTERNAL_CF) else cardFileIn(dir, slot)
            cardFile = CardFile.open(file, !protectedCard)
            FatImage.over(cardFile!!).use { }
        } catch (e: Exception) {
            // The card goes back in as it was.
            cardFile?.close()
            cardFile = null
            putBack()
            throw Failed(message(e))
        }
        changed = false
    }

    // Puts the card file back in the slot, ejected again if it was out when the view opened.
    private fun putBack() {
        val id = identity ?: throw Failed("Couldn't put the card back in.")
        val holdEjected = device.zaurus && !wasInserted
        try {
            if (holdEjected) {
                val mode = if (protectedCard) "protected" else "writable"
                putInAtStart(slot, "$id $mode ejected")
            } else putIn(id, protectedCard)
        } catch (e: Failed) {
            throw Failed("Couldn't put the card back in. It's kept in the machine and goes in at the next start. ${e.message}", e.code)
        }
        identity = null
        editing = null
        if (!wasInserted && !holdEjected) try { eject() } catch (_: Exception) { }
        if (compactFlash) finishCompactFlashSwap()
    }

    private fun compactFlashIdentity(inserted: Boolean): ZaurusCfSlot {
        val reply = link.request("cf-identity", timeoutSeconds = 30)
        return reply.takeIf { it.ok }?.let { ZaurusCfSlot.parse(it.message, inserted) }
            ?: throw Failed("Couldn't read the removable CF card identity.")
    }

    // Replay a take-out that was interrupted before the card and session could be saved together.
    // Slot commands here use n=1 without changing the selected slot of an SD file visit.
    internal fun restoreCompactFlash(state: State, saved: ZaurusCfSlot): State {
        val path = session ?: throw Failed("The machine folder is missing.")
        val dir = File(path).absoluteFile.parentFile ?: throw Failed("The machine folder is missing.")
        if (state == State.NONE) {
            if (!CardFile.settle(File(dir, ZaurusImages.EXTERNAL_CF))) throw Failed("Couldn't settle the removable CF card.")
            putInAtStart(1, saved.argument)
        } else if (compactFlashIdentity(state == State.INSERTED).identity != saved.identity) {
            throw Failed("The removable CF card does not match the last file exchange.")
        }
        val now = stateOf(1)
        if (now == State.INSERTED && !saved.inserted) run(command("eject", 1))
        if (now == State.EJECTED && saved.inserted) run(command("reinsert", 1))
        finishCompactFlashSwap()
        return stateOf(1)
    }

    private fun finishCompactFlashSwap() {
        val path = session ?: throw Failed("The machine folder is missing.")
        val dir = File(path).absoluteFile.parentFile ?: throw Failed("The machine folder is missing.")
        val reply = link.request("save", path, timeoutSeconds = 120)
        if (!reply.ok) throw Failed("The card is back, but its changes couldn't be saved with the machine. ${reply.message}")
        val marker = File(dir, ZaurusImages.CF_SLOT)
        if (!marker.delete() && marker.exists()) throw Failed("Couldn't clear the CF file exchange.")
    }

    // A folder's entries, the free space on the card and the card's size.
    class Listing(val entries: List<FatImage.Entry>, val free: Long, val total: Long)

    fun list(path: String): Listing = read { Listing(it.list(path), it.freeBytes, it.totalBytes) }

    fun <T> read(block: (FatImage) -> T): T {
        val c = cardFile ?: throw Failed("The card isn't open.")
        return synchronized(c) { FatImage.over(c).use(block) }
    }

    // Edits the card file in place; CardFile commits each change so a crash leaves the old or the new
    // card. The check reads back only what changed.
    private fun edit(block: (FatImage) -> Unit, verify: (FatImage) -> Unit) {
        val c = cardFile ?: throw Failed("The card isn't open.")
        synchronized(c) {
            FatImage.over(c).use { fat ->
                try { block(fat) } finally { if (fat.committed) changed = true }
            }
            FatImage.over(c).use(verify)
        }
    }

    // Copies picked files into a card folder under their own names. Returns how many went in and how many were skipped.
    fun addFiles(path: String, uris: List<Uri>, resolver: CardFiles.Resolver, progress: (Float) -> Unit = {}): Pair<Int, Int> {
        var added = 0
        var skipped = 0
        uris.forEachIndexed { i, uri ->
            if (jobCancelled) throw Failed("File transfer cancelled.", "cancelled")
            if (addFile(path, uri, resolver) == null) skipped++ else added++
            progress((i + 1f) / uris.size)
        }
        return added to skipped
    }

    // Copies a picked file into a card folder under its own name and checks the bytes on the card.
    // Returns the name it went in under, or null when the clash rule skipped it.
    fun addFile(path: String, uri: Uri, resolver: CardFiles.Resolver): String? {
        folder.mkdirs()
        val name = displayName(uri) ?: throw Failed("That file has no name.")
        FatImage.checkName(name)
        val source = File(folder, "incoming")
        try {
            val room = read { it.freeBytes }
            val size = sizeOf(uri) ?: 0L
            if (size > room) throw Failed("There isn't room on the card for that file.")
            // The file is copied here first, then into the card.
            val free = context.filesDir.usableSpace - JOURNAL_ROOM
            if (size > free) throw Failed("There isn't room on this device for that file. Free up about ${(size - free + (1L shl 20) - 1) shr 20} MB and try again.")
            if (!copyIn(uri, source, room)) throw Failed("There isn't room on the card for that file.")
            val expected = sha256(source)
            var put: String? = null
            cancellable {
                edit({ fat -> put = CardFiles.addStream(fat, path, name, source.length(), { source.inputStream().buffered(1 shl 16) }, resolver) }) { fat ->
                    val landed = put ?: return@edit
                    val entry = fat.find(joined(path, landed)) ?: throw Failed("The file didn't land on the card.")
                    val digest = MessageDigest.getInstance("SHA-256")
                    fat.read(entry, object : java.io.OutputStream() {
                        override fun write(b: Int) = digest.update(b.toByte())
                        override fun write(b: ByteArray, off: Int, len: Int) = digest.update(b, off, len)
                    })
                    if (!digest.digest().contentEquals(expected)) throw Failed("The copy on the card didn't match the file.")
                }
            }
            return put
        } finally {
            source.delete()
        }
    }

    // Copies a picked folder and everything in it into a card folder.
    fun addTree(path: String, root: FolderCard.Node, resolver: CardFiles.Resolver, progress: (Float) -> Unit = {}): CardFiles.Report {
        val plan = FolderCard.measure(root) { jobCancelled }
        plan.problem?.let { throw Failed(if (it == COPY_CANCELLED) "File transfer cancelled." else it, if (it == COPY_CANCELLED) "cancelled" else null) }
        if (plan.bytes > read { it.freeBytes }) throw Failed("There isn't room on the card for that folder.")
        var report = CardFiles.Report(0, 0)
        val total = maxOf(plan.bytes, 1L)
        cancellable {
            edit({ fat -> report = CardFiles.importTree(fat, root, path, resolver, { jobCancelled }) { progress(it.toFloat() / total) } }) { }
        }
        return report
    }

    // Copies or moves card entries into a folder.
    fun paste(sources: List<String>, destFolder: String, move: Boolean, resolver: CardFiles.Resolver, progress: (Float) -> Unit = {}): CardFiles.Report {
        var report = CardFiles.Report(0, 0)
        cancellable {
            val total = maxOf(if (move) 1L else read { CardFiles.bytesIn(it, sources) }, 1L)
            edit({ fat -> report = CardFiles.paste(fat, sources, destFolder, move, resolver, { jobCancelled }) { progress(it.toFloat() / total) } }) { }
        }
        return report
    }

    // Deletes entries and what is in them.
    fun deleteEntries(path: String, names: List<String>) {
        cancellable { edit({ CardFiles.delete(it, names.map { n -> joined(path, n) }) { jobCancelled } }) { } }
    }

    fun makeFolder(path: String, name: String) {
        edit({ it.makeFolder(path, name.trim()) }) { fat ->
            if (fat.find(joined(path, name.trim()))?.directory != true) throw Failed("The folder didn't land on the card.")
        }
    }

    fun renameEntry(path: String, oldName: String, newName: String) {
        edit({ CardFiles.rename(it, joined(path, oldName), newName) }) { }
    }

    // Copies card entries into a picked folder.
    fun exportEntries(path: String, entries: List<FatImage.Entry>, to: CardFiles.HostFolder, progress: (Float) -> Unit = {}): Int {
        return cancellable {
            read { fat ->
                val paths = entries.map { joined(path, it.name) }
                val total = maxOf(CardFiles.bytesIn(fat, paths), 1L)
                CardFiles.exportTree(fat, paths, to, { jobCancelled }) { progress(it.toFloat() / total) }
            }
        }
    }

    fun exportFile(entry: FatImage.Entry, to: Uri) {
        val output = context.contentResolver.openOutputStream(to, "wt") ?: throw Failed("Couldn't write there.")
        output.use { o -> read { it.read(entry, o) } }
    }

    // Closes the card file and puts the card back in the Palm (ejected again if it was out). The
    // insert waits for the Palm to notice the card was out.
    // Also run as the machine stops, so it is synchronized against the view's own call.
    @Synchronized
    fun finishEdit() {
        val c = cardFile
        cardFile = null
        if (c != null) {
            try { synchronized(c) { c.close() } } catch (_: Exception) { }
            // A close that failed can leave the visit's pending list; the card goes in settled.
            CardFile.settle(c.file)
        }
        try {
            if (identity != null) putBack()
        } finally {
            changed = false
        }
    }

    // Drops what an earlier version's exchange left in the private folder.
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
        internal fun parseState(message: String, m68k: Boolean): State =
            when (if (m68k) message.substringBefore(' ') else message) {
                "inserted" -> State.INSERTED
                "ejected" -> State.EJECTED
                "none" -> State.NONE
                "inserted wifi", "inserted wifi ejected" -> State.PALM_WIFI
                else -> State.UNKNOWN
            }

        // The core's candidate card file (uarm/SdCandidate.h), live while its machine runs.
        const val LIVE_CANDIDATE = ".frondsd.tmp-live-"
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
            if (session == null || !card.isFile || !marker.isFile) return null
            val owner = marker.readText()
            return if (owner == session || sameMachine(context, owner, session)) card else null
        }

        // The machine folder whose SD exchange is open or pending, or null.
        fun pendingMachine(context: Context): File? {
            val marker = File(File(context.filesDir, "sd-exchange"), PENDING)
            if (!marker.isFile) return null
            val owner = try { marker.readText() } catch (_: IOException) { return null }
            return Sessions.forPath(context, owner)?.folder ?: File(owner).parentFile
        }

        private fun sameMachine(context: Context, a: String, b: String): Boolean {
            val first = Sessions.forPath(context, a) ?: return false
            return Sessions.forPath(context, b)?.id == first.id
        }

        // Inserts the edited card kept after a kill, once the session is running again.
        // Keeps it when the insert fails.
        fun restorePending(context: Context, link: ControlLink, session: String?): Boolean {
            val card = pendingFor(context, session) ?: return true
            try { FatImage.recover(card) } catch (_: IOException) { return false }
            val reply = link.request("sd-insert", card.path, timeoutSeconds = 120)
            if (!reply.ok) return false
            File(card.parentFile, PENDING).delete()
            card.delete()
            return true
        }

        // Removes private copies left by a process that stopped mid-edit. The edited card of a session
        // that still exists, and its journal until it's replayed, stay until it has been inserted.
        fun clean(context: Context) {
            // A recovery session left by an exchange that never finished.
            Sessions.adoptRecovery(context)
            File(context.filesDir, "ms-images").deleteRecursively()
            val folder = File(context.filesDir, "sd-exchange")
            val marker = File(folder, PENDING)
            val owner = if (marker.isFile) marker.readText() else null
            // A machine not saved yet has no session file but still owns its card.
            val keep = owner != null && (File(owner).isFile || Sessions.forPath(context, owner) != null)
            val card = File(folder, "card.img")
            if (keep && card.isFile) try { FatImage.recover(card) } catch (_: IOException) { }
            val journal = FatImage.journalOf(card).name
            folder.listFiles().orEmpty().forEach {
                if (keep && (it.name == card.name || it.name == PENDING || it.name == journal)) return@forEach
                if (it.isDirectory && it.name.startsWith("ipk-")) it.deleteRecursively() else it.delete()
            }
        }

        // Room for one change's journal: at most every FAT sector in both copies plus folder sectors.
        const val JOURNAL_ROOM = 1L shl 20

        // The card file a take-out left in a machine folder: the SD card, or the classic or Pro stick.
        fun cardFileIn(dir: File, n: Int = 0): File =
            if (n != 0) File(dir, "sd2.frondsd")
            else listOf("sd.frondsd", "ms.frondms", "ms.frondmsp").map { File(dir, it) }.firstOrNull { it.isFile } ?: File(dir, "sd.frondsd")

        // The machine folder whose card is taken out for the file view in this process, or null,
        // and its slot. That slot keeps swap until the card is back, whatever saves happen meanwhile.
        @Volatile var editing: File? = null
        @Volatile var editingSlot = 0

        fun message(e: Exception): String = when (e) {
            is Failed, is FatImage.FatException -> e.message ?: "Something went wrong."
            is IOException -> "Couldn't read or write the file."
            else -> e.message ?: "Something went wrong."
        }
    }
}
