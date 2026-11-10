package com.rm.frond

import org.json.JSONObject
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.security.MessageDigest
import java.security.SecureRandom
import java.time.Instant
import java.time.temporal.ChronoUnit
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import java.util.zip.Inflater

// The card library, as on the desktop (desktop/CardStore.h): files/cards/<id>/card.json for every
// card, the card file there while it is in the drawer, and <machine>/slot.json for the card a
// machine's slot holds or is linked to. A card in a machine is that folder's sd.frondsd,
// ms.frondms or ms.frondmsp, so moving one is a rename. One machine runs at a time, in the
// :emulator process, which holds <machine>/.run.lock for the whole run.
object Cards {
    val cardFiles = listOf("sd.frondsd", "ms.frondms", "ms.frondmsp")
    const val RUN_LOCK = ".run.lock"
    const val SPARE_BYTES = 16L shl 20 // frondSdCandidate::spareBytes

    fun kindOfFile(file: String) = when (file) { "sd.frondsd" -> "sd"; "ms.frondms" -> "ms"; else -> "ms-pro" }
    fun fileOfKind(kind: String) = when (kind) { "sd" -> "sd.frondsd"; "ms" -> "ms.frondms"; else -> "ms.frondmsp" }
    // Descriptor kinds in a .frond: 1 SD, 2 classic stick, 3 Pro.
    fun descriptorKindOf(file: String) = when (file) { "sd.frondsd" -> 1L; "ms.frondms" -> 2L; else -> 3L }
    // The card tool's word for a kind.
    fun toolKind(kind: String) = if (kind == "ms-pro") "mspro" else kind
    fun kindName(kind: String) = when (kind) { "sd" -> "SD card"; "ms" -> "Memory Stick"; else -> "Memory Stick Pro" }

    // files/cards beside files/sessions.
    fun libraryFor(sessions: File) = File(sessions.absoluteFile.parentFile, "cards")
    fun entryDir(library: File, id: String) = File(library, id)

    // Free space where `dir` is, and the card tool. Tests replace them.
    @Volatile var freeSpace: (File) -> Long = { it.usableSpace }
    @Volatile var tool: (Array<String>, CardToolProgress?, AtomicBoolean?) -> String = { args, listener, cancel ->
        // "export-fd CARD FD" writes the raw image into an open file.
        if (args.size == 3 && args[0] == "export-fd") CardTool.exportFd(args[1], args[2].toInt(), listener, cancel)
        else CardTool.run(args, listener, cancel)
    }

    // ---- card.json and slot.json ----

    data class Card(
        val id: String,
        val name: String,
        val kind: String,
        val created: String,
        val machine: String?, // the holder's folder name, a hint
        val bytes: Long, // logical size
        val identity: ByteArray, // 32 bytes, the card's serial
        val writeProtected: Boolean,
    ) {
        override fun equals(other: Any?) = other is Card && id == other.id && name == other.name && kind == other.kind &&
            created == other.created && machine == other.machine && bytes == other.bytes &&
            identity.contentEquals(other.identity) && writeProtected == other.writeProtected
        override fun hashCode() = id.hashCode()
    }

    private fun JSONObject.text(key: String): String? = if (!has(key) || isNull(key)) null else optString(key).ifEmpty { null }
    private fun nullable(s: String?): Any = s ?: JSONObject.NULL

    fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it) }
    fun unhex(text: String): ByteArray? {
        if (text.length % 2 != 0) return null
        return try { ByteArray(text.length / 2) { text.substring(2 * it, 2 * it + 2).toInt(16).toByte() } } catch (e: NumberFormatException) { null }
    }
    private fun fit32(bytes: ByteArray) = bytes.copyOf(32)

    fun readCard(dir: File): Card? = try {
        val o = JSONObject(File(dir, "card.json").readText())
        val kind = o.optString("kind")
        if (o.optInt("version") != 1 || o.optString("id") != dir.name || kind !in listOf("sd", "ms", "ms-pro")) null
        else Card(o.getString("id"), o.optString("name"), kind, o.optString("created"), o.text("machine"),
            o.optLong("bytes"), fit32(unhex(o.optString("identity")) ?: ByteArray(0)), o.optBoolean("writeProtected"))
    } catch (e: Exception) {
        null
    }

    fun writeCard(dir: File, card: Card): Boolean = writeJson(File(dir, "card.json"), JSONObject()
        .put("version", 1).put("id", card.id).put("name", card.name).put("kind", card.kind).put("bytes", card.bytes)
        .put("created", card.created).put("identity", hex(fit32(card.identity))).put("writeProtected", card.writeProtected)
        .put("machine", nullable(card.machine)))

    data class Slot(
        val present: Boolean = false,
        val card: String? = null, // the card it holds
        val link: String? = null, // the linked card
        val name: String? = null, // the card's name, so an export carries it
        val swap: Boolean = false, // the slot changed since the last save
    )

    fun readSlot(machineDir: File): Slot = try {
        val o = JSONObject(File(machineDir, "slot.json").readText())
        if (o.optInt("version") != 1) Slot()
        else Slot(true, o.text("card"), o.text("link"), o.text("name"), o.optBoolean("swap"))
    } catch (e: Exception) {
        Slot()
    }

    fun writeSlot(machineDir: File, slot: Slot): Boolean = writeJson(File(machineDir, "slot.json"), JSONObject()
        .put("version", 1).put("card", nullable(slot.card)).put("link", nullable(slot.link))
        .put("name", nullable(slot.name)).put("swap", slot.swap))

    private fun writeJson(file: File, json: JSONObject): Boolean {
        val temporary = File(file.path + ".tmp")
        return try {
            temporary.outputStream().use { out ->
                out.write(json.toString().toByteArray(Charsets.UTF_8))
                out.fd.sync()
            }
            temporary.renameTo(file)
        } catch (e: IOException) {
            false
        } finally {
            temporary.delete()
        }
    }

    fun newId() = UUID.randomUUID().toString()
    fun now(): String = Instant.now().truncatedTo(ChronoUnit.SECONDS).toString()
    private val random = SecureRandom()
    // A fresh serial: 32 hex characters, as the core makes them.
    fun newIdentity(): ByteArray = hex(ByteArray(16).also { random.nextBytes(it) }).toByteArray(Charsets.US_ASCII)

    fun cardFileIn(dir: File): String? = cardFiles.firstOrNull { File(dir, it).isFile }

    // ---- What the .frond and the card files say ----

    class Descriptor(val kind: Long, val inserted: Boolean, val writeProtected: Boolean, val identity: ByteArray,
                     val logical: Long, val physical: Long, val generation: Long)

    // The .frond's version (0 if unreadable) and its media descriptors (v7, v8 and v9).
    fun descriptorsOf(frond: File?): Pair<Long, List<Descriptor>> = try {
        if (frond == null || !frond.isFile) 0L to emptyList()
        else RandomAccessFile(frond, "r").use { f ->
            val head = ByteArray(92)
            val n = f.read(head)
            if (n < 8 || le32(head, 0) != 0x19800819L) return 0L to emptyList()
            val version = le32(head, 4)
            if ((version != 7L && version != 8L && version != 9L) || n != 92) return version to emptyList()
            val size = if (version == 7L) 92 else 100
            val list = ArrayList<Descriptor>()
            for (i in 0 until minOf(le32(head, 88), 4L).toInt()) {
                val d = ByteArray(size)
                if (f.read(d) != size) return version to emptyList()
                list += Descriptor(le32(d, 0), le32(d, 36) != 0L, le32(d, 40) != 0L, d.copyOfRange(44, 76),
                    le64(d, 76), le64(d, 84), if (size == 100) le64(d, 92) else 0L)
            }
            version to list
        }
    } catch (e: IOException) {
        0L to emptyList()
    }

    fun describedCard(cards: List<Descriptor>, file: String) = cards.firstOrNull { it.kind == descriptorKindOf(file) }

    // The SD, Pro or classic v2 generation, or a v1 classic stick's change counter, and the logical
    // size, from a card file's header.
    // A v1 classic stick isn't a container, so only its header is read here.
    fun cardHeader(file: File): Pair<Long, Long>? = try {
        val info = CardFile.info(file)
        when {
            info?.kind == 0 -> info.generation to info.payload
            // Each MB is 2048 pages of 512 bytes plus 9.
            info?.classic == true -> info.generation to classicV2Physical(info.payload, info.side) / 521 * 512
            info != null -> null
            else -> CardFile.header(file)?.takeIf { String(it, 0, 8, Charsets.US_ASCII) == "FRONDMS\u0000" && le32(it, 8) == 1L }
                ?.let { h -> le64(h, 32) to le32(h, 12) * le32(h, 16) * 512 }
        }
    } catch (e: IOException) {
        null
    }

    fun hasJournals(dir: File, file: String) = dir.list().orEmpty().any { it.startsWith("$file.journal-") }

    // The card file is complete on its own: no journals beside it, and the counter the .frond's last
    // save recorded (a card put in since then, swap set, has nothing to match).
    fun settled(dir: File, file: String?, swap: Boolean): Boolean {
        if (file == null || !File(dir, file).isFile || hasJournals(dir, file)) return false
        if (swap) return true
        val d = describedCard(descriptorsOf(sessionFile(dir)).second, file) ?: return true
        return cardHeader(File(dir, file))?.first == d.generation
    }

    // ---- Locks ----

    const val BUSY = "The card library is busy. Try again."
    private val libraryThreads = ReentrantLock()
    private var libraryChannel: FileChannel? = null
    private var libraryFileLock: FileLock? = null

    // cards/.lock: one thread of this process at a time (it nests), and one process through the
    // file lock, which the kernel frees if the process dies.
    class LibraryLock(library: File, timeoutMs: Long = 10_000) : Closeable {
        val ok: Boolean

        init {
            library.mkdirs()
            val deadline = System.currentTimeMillis() + timeoutMs
            var got = libraryThreads.tryLock(timeoutMs, TimeUnit.MILLISECONDS)
            if (got && libraryThreads.holdCount == 1) {
                got = false
                try {
                    val channel = RandomAccessFile(File(library, ".lock"), "rw").channel
                    while (true) {
                        val lock = try { channel.tryLock() } catch (e: OverlappingFileLockException) { null }
                        if (lock != null) {
                            libraryChannel = channel
                            libraryFileLock = lock
                            got = true
                            break
                        }
                        if (System.currentTimeMillis() > deadline) break
                        Thread.sleep(50)
                    }
                    if (!got) channel.close()
                } catch (e: IOException) {
                    got = false
                }
                if (!got) libraryThreads.unlock()
            }
            ok = got
        }

        override fun close() {
            if (!ok) return
            if (libraryThreads.holdCount == 1) {
                try { libraryFileLock?.release() } catch (_: IOException) {}
                try { libraryChannel?.close() } catch (_: IOException) {}
                libraryFileLock = null
                libraryChannel = null
            }
            libraryThreads.unlock()
        }
    }

    // A machine's <machine>/.run.lock, held by the :emulator process while the machine runs. Others
    // take it for the length of an operation. Null when someone holds it.
    class RunLock internal constructor(private val held: FileLocks.Held) : Closeable {
        override fun close() = held.close()
    }

    // Null when this process or another holds it (through FileLocks, so a check never drops a lock
    // this process holds).
    fun tryRunLock(dir: File): RunLock? = try {
        if (!dir.isDirectory) null else FileLocks.tryLock(File(dir, RUN_LOCK))?.let { RunLock(it) }
    } catch (e: IOException) {
        null
    }

    // ---- Entries ----

    class Entry(val card: Card, val dir: File, val inDrawer: Boolean)

    fun entries(library: File): List<Entry> = library.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty()
        .mapNotNull { dir -> readCard(dir)?.let { Entry(it, dir, File(dir, fileOfKind(it.kind)).isFile) } }

    // "Games", then "Games 2" and so on, among the library's names.
    fun freeCardName(library: File, base: String, except: String? = null): String {
        val names = entries(library).filter { it.card.id != except }.map { it.card.name }.toSet()
        if (base !in names) return base
        var n = 2
        while ("$base $n" in names) n++
        return "$base $n"
    }

    // Whether a library card other than `except` already has this serial.
    fun identityUsed(library: File, identity: ByteArray, except: String? = null): Boolean {
        val wanted = fit32(identity)
        return entries(library).any { it.card.id != except && it.card.identity.contentEquals(wanted) }
    }

    // The name a machine shows: <id>.json's name, else its folder name.
    fun machineName(dir: File): String = try {
        JSONObject(File(dir, "${dir.name}.json").readText()).optString("name").ifEmpty { dir.name }
    } catch (e: Exception) {
        dir.name
    }

    fun machineDevice(dir: File): Device? = try {
        Device.byId(JSONObject(File(dir, "${dir.name}.json").readText()).optString("device"))
    } catch (e: Exception) {
        null
    }

    // A new entry for the card file in `dir`. Kind and size from the file, serial and write
    // protection from the descriptor when it describes this card. With `freshIdentity` (a copy of a
    // card another entry has), or when another card has that serial, the card gets a new serial and
    // the slot swap, so the core loads it as a different card. Library lock held.
    fun moveIn(library: File, dir: File, file: String, slot: Slot, name: String?, freshIdentity: Boolean = false): Card? {
        val d = describedCard(descriptorsOf(sessionFile(dir)).second, file)
        val logical = cardHeader(File(dir, file))?.second ?: 0L
        var fresh = freshIdentity
        if (!fresh && d != null && !slot.swap && identityUsed(library, d.identity)) fresh = true
        val swap = slot.swap || fresh
        val id = newId()
        val card = Card(id, freeCardName(library, name?.takeIf { it.isNotBlank() } ?: "${machineName(dir)} card"),
            kindOfFile(file), now(), dir.name, d?.logical ?: logical,
            if (d != null && !swap) fit32(d.identity) else newIdentity(), d?.writeProtected ?: false)
        val to = entryDir(library, id)
        if (!to.mkdirs() || !writeCard(to, card)) {
            to.deleteRecursively()
            return null
        }
        if (!writeSlot(dir, slot.copy(present = true, card = id, name = card.name, swap = swap))) {
            to.deleteRecursively()
            return null
        }
        return card
    }

    const val SAME_DISK = "Cards and sessions have to be on the same disk."
    const val IN_FILES = "The card is open in Files. Close it first."
    const val MISMATCH = "The card has an unfinished change that doesn't match it."

    // A card file nobody has open in Files…, with any change a crash cut off finished.
    fun ready(card: File) = !CardFile.inUse(card) && CardFile.settle(card)

    // Card file from a machine folder into the drawer. The slot gets swap first, so a crash at any
    // point loads with --card-swap; a cut-off move is fixed by reconcile rule 4. Library lock and
    // machine held.
    fun toDrawer(library: File, dir: File, id: String): String? {
        val file = cardFileIn(dir)
        val card = readCard(entryDir(library, id))
        if (file == null || card == null) return "Couldn't move the card."
        CardFile.notReady(File(dir, file))?.let { return it }
        val slot = readSlot(dir).copy(present = true, swap = true, card = id)
        if (!writeSlot(dir, slot)) return "Couldn't move the card."
        if (!File(dir, file).renameTo(File(entryDir(library, id), file))) return "Couldn't move the card into Cards. $SAME_DISK"
        writeSlot(dir, slot.copy(card = null))
        writeCard(entryDir(library, id), card.copy(machine = null))
        return null
    }

    // A drawer card into an empty slot. Library lock and machine held.
    fun toMachine(library: File, id: String, dir: File): String? {
        val card = readCard(entryDir(library, id)) ?: return "Couldn't find that card."
        val file = fileOfKind(card.kind)
        if (cardFileIn(dir) != null) return "Take the machine's card out first."
        if (!File(entryDir(library, id), file).isFile) return "That card is in another machine."
        CardFile.notReady(File(entryDir(library, id), file))?.let { return it }
        val slot = readSlot(dir).copy(present = true, swap = true, card = id, name = card.name)
        if (!writeSlot(dir, slot)) return "Couldn't move the card."
        if (!File(entryDir(library, id), file).renameTo(File(dir, file))) {
            writeSlot(dir, slot.copy(card = null))
            return "Couldn't move the card into the machine. $SAME_DISK"
        }
        writeCard(entryDir(library, id), card.copy(machine = dir.name))
        return null
    }

    // ---- Copies: cards/.new-<id>, locked by cards/.new-<id>.lock while a copy fills it ----

    class NewCopy(library: File) : Closeable {
        val id = newId()
        val dir = File(library, ".new-$id")
        private val lockFile = File(library, ".new-$id.lock")
        private var lock: FileLocks.Held? = null
        private var kept = false
        val ok: Boolean

        init {
            ok = try {
                library.mkdirs()
                lock = FileLocks.tryLock(lockFile)
                lock != null && dir.mkdirs()
            } catch (e: Exception) {
                false
            }
        }

        fun taken() { kept = true }

        override fun close() {
            if (!kept) dir.deleteRecursively()
            lock?.close()
            lockFile.delete()
        }
    }

    // Copy folders left by a process that died: unlocked and untouched for ten minutes.
    fun removeStaleNew(library: File, now: Long = System.currentTimeMillis()) {
        library.listFiles { f -> f.isDirectory && f.name.startsWith(".new-") }.orEmpty().forEach { dir ->
            val lockFile = File(library, "${dir.name}.lock")
            val lock = try { FileLocks.tryLock(lockFile) } catch (e: IOException) { null } ?: return@forEach
            lock.use {
                val newest = (listOf(dir) + dir.listFiles().orEmpty()).maxOf { f -> f.lastModified() }
                if (now - newest > 10 * 60 * 1000) dir.deleteRecursively()
            }
            if (!dir.exists()) lockFile.delete()
        }
    }

    // Copies `from` to `to` in 1 MiB chunks, reporting progress; false when cancelled.
    private fun copyChunked(from: File, to: File, cancelled: () -> Boolean, progress: ((Long, Long) -> Unit)?): Boolean {
        val total = from.length()
        var done = 0L
        val buffer = ByteArray(1 shl 20)
        from.inputStream().use { input ->
            to.outputStream().use { out ->
                while (true) {
                    if (cancelled()) return false
                    val n = input.read(buffer)
                    if (n < 0) break
                    out.write(buffer, 0, n)
                    done += n
                    progress?.invoke(done, total)
                }
                out.fd.sync()
            }
        }
        return true
    }

    fun spaceMessage(bytes: Long) = "Not enough free space for the card. It needs ${(bytes + SPARE_BYTES) / 1_000_000 + 1} MB."

    // A settled copy of the card file `file` in machine folder `dir` (or a drawer folder) at `to`. An
    // unfinished save is settled as the copy is made. Checks free space first. Null on success,
    // "unsettled" when the journal can't be applied, "cancelled", or a message.
    fun settledCopy(dir: File, file: String, to: File, cancelled: () -> Boolean = { false },
                    progress: ((Long, Long) -> Unit)? = null): String? {
        var source = File(dir, file)
        var journal: File? = null
        if (hasJournals(dir, file) || !settled(dir, file, false)) {
            val settle = sdSettleOf(dir)
            when {
                settle.sibling == file && settle.full != null -> source = settle.full
                settle.sibling == file && settle.journal != null -> journal = settle.journal
                hasJournals(dir, file) -> return "unsettled"
            }
        }
        val total = source.length()
        val free = freeSpace(to.absoluteFile.parentFile)
        if (free >= 0 && free < total + SPARE_BYTES) return spaceMessage(total)
        return try {
            if (journal != null) {
                to.outputStream().use { out -> writeSettledSd(source, journal, out); out.fd.sync() }
                progress?.invoke(total, total)
                null
            } else if (!copyChunked(source, to, cancelled, progress)) {
                to.delete()
                "cancelled"
            } else null
        } catch (e: IOException) {
            to.delete()
            if (journal != null) "unsettled" else "Couldn't copy the card."
        }
    }

    // ---- Reconcile ----

    // A machine folder taken for reconcile: its run lock, or null for the caller's own machine.
    private class Held(val dir: File, val lock: RunLock?)

    // Folders an import or a duplicate is still filling.
    private fun beingMade(dir: File) = dir.name in activeImports || File(dir, ".import.part").exists() || File(dir, COPY_PART).exists()

    // Makes card.json, slot.json and the machine hints match where the card files are. `held` is a
    // machine folder the caller already holds. Machines running elsewhere are skipped. Library lock held.
    fun reconcileLocked(sessions: File, held: File? = null) {
        val library = libraryFor(sessions)
        val heldCanon = held?.canonicalFile
        val machines = ArrayList<Held>()
        val skippedClaims = HashSet<String>() // ids named by machines we can't touch
        val skippedNames = HashSet<String>()
        try {
            for (dir in sessions.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty().sortedBy { it.name }) {
                if (beingMade(dir)) {
                    // An import or a duplicate still filling it: its card entry is already made.
                    val slot = readSlot(dir)
                    listOfNotNull(slot.card, slot.link).forEach { skippedClaims += it }
                    skippedNames += dir.name
                    continue
                }
                if (sessionFile(dir) == null) continue // not saved yet
                if (heldCanon != null && dir.canonicalFile == heldCanon) {
                    machines += Held(dir, null)
                    continue
                }
                val lock = tryRunLock(dir)
                if (lock == null) {
                    val slot = readSlot(dir)
                    listOfNotNull(slot.card, slot.link).forEach { skippedClaims += it }
                    skippedNames += dir.name
                    continue
                }
                machines += Held(dir, lock)
            }
            settleEdits(library, machines.map { it.dir })
            reconcileMachines(sessions, library, machines.map { it.dir }, skippedClaims, skippedNames)
        } finally {
            machines.forEach { it.lock?.close() }
        }
    }

    // Finishes Files… changes a crash cut off, in the machines held and the drawer. Cards open in
    // Files… now are left alone.
    private fun settleEdits(library: File, dirs: List<File>) {
        for (dir in dirs) cardFileIn(dir)?.let { CardFile.settle(File(dir, it)) }
        for (e in entries(library)) CardFile.settle(File(e.dir, fileOfKind(e.card.kind)))
    }

    private fun reconcileMachines(sessions: File, library: File, dirs: List<File>, skippedClaims: Set<String>, skippedNames: Set<String>) {
        var known = entries(library)
        fun find(id: String?) = known.firstOrNull { it.card.id == id }
        // Rules 1 to 4 per machine. Machines the hint names go first, so they keep their card (rule 3).
        val ordered = dirs.sortedBy { dir -> if (find(readSlot(dir).card)?.card?.machine == dir.name) 0 else 1 }
        val claimed = HashSet<String>()
        for (dir in ordered) {
            if (descriptorsOf(sessionFile(dir)).first < 7L) continue // a v6 card is inside the .frond until its first run
            val slot = readSlot(dir)
            val file = cardFileIn(dir)
            val e = find(slot.card)
            if (file != null) {
                if (e != null && e.card.kind == kindOfFile(file) && !e.inDrawer && e.card.id !in claimed) {
                    claimed += e.card.id // rule 1
                    if (e.card.machine != dir.name) writeCard(e.dir, e.card.copy(machine = dir.name))
                    continue
                }
                // Rule 2, or rule 3 for a second holder: a new entry, named after the first one's, with
                // its own serial. The card it holds isn't its link any more.
                moveIn(library, dir, file, slot.copy(present = true, link = null), e?.card?.name ?: slot.name, e != null)?.let {
                    claimed += it.id
                    known = entries(library)
                }
                continue
            }
            // No card file. Rule 4 (a cut-off move: the drawer wins) or a card that is elsewhere now.
            if (slot.card != null) writeSlot(dir, slot.copy(card = null, swap = true))
        }
        // Rule 5: an entry with no file anywhere goes, with links to it. Not while a machine we
        // skipped may hold it.
        for (e in known) {
            if (e.inDrawer || e.card.id in claimed || e.card.id in skippedClaims) continue
            if (e.card.machine != null && e.card.machine in skippedNames) continue
            // A machine that isn't saved yet, holding the card it was given while running.
            val hinted = e.card.machine?.let { File(sessions, it) }
            if (hinted != null && sessionFile(hinted) == null && File(hinted, fileOfKind(e.card.kind)).isFile &&
                readSlot(hinted).card == e.card.id) continue
            e.dir.deleteRecursively()
            for (dir in dirs) {
                val slot = readSlot(dir)
                if (slot.link == e.card.id) writeSlot(dir, slot.copy(link = null))
            }
        }
        // Rule 6: a linked card left in a stopped machine by a crash goes back to the drawer once settled.
        for (dir in dirs) {
            val slot = readSlot(dir)
            val file = cardFileIn(dir) ?: continue
            if (slot.link == null || slot.card != slot.link) continue
            if (settled(dir, file, slot.swap)) toDrawer(library, dir, slot.card)
        }
        // Hints of drawer cards.
        for (e in entries(library)) if (e.inDrawer && e.card.machine != null) writeCard(e.dir, e.card.copy(machine = null))
    }

    fun reconcile(sessions: File, held: File? = null): String? = LibraryLock(libraryFor(sessions)).use { lock ->
        if (!lock.ok) return BUSY
        reconcileLocked(sessions, held)
        null
    }

    // At app start: stale copies go, then reconcile, which moves existing machines' cards in.
    fun startUp(sessions: File) {
        if (!sessions.isDirectory) return
        removeStaleNew(libraryFor(sessions))
        reconcile(sessions)
    }

    // ---- Machine start and close (the :emulator process, run lock held) ----

    // Card flags for the core, from slot.json and card.json. Only for ARM machines.
    fun launchArguments(sessions: File, dir: File): List<String> {
        val slot = readSlot(dir)
        if (!slot.swap) return emptyList()
        val args = mutableListOf("--card-swap")
        val card = slot.card?.let { readCard(entryDir(libraryFor(sessions), it)) }
        if (cardFileIn(dir) != null && card != null) {
            args += listOf("--card-identity", hex(fit32(card.identity)))
            if (card.writeProtected) args += "--card-protected"
        }
        return args
    }

    // The flags for the core, and the name of the machine holding a linked card when it's elsewhere.
    class Start(val arguments: List<String>, val heldBy: String?)

    // Machine start: reconcile, then a linked machine with an empty slot takes its card if it is in
    // the drawer.
    fun takeAtStart(sessions: File, dir: File): Start {
        val library = libraryFor(sessions)
        var heldBy: String? = null
        // The run lock is held, so this machine's card can be settled even if the library is busy.
        cardFileIn(dir)?.let { CardFile.settle(File(dir, it)) }
        LibraryLock(library).use { lock ->
            if (lock.ok) {
                reconcileLocked(sessions, dir)
                val slot = readSlot(dir)
                val link = slot.link
                if (link != null && cardFileIn(dir) == null) {
                    val card = readCard(entryDir(library, link))
                    if (card != null && File(entryDir(library, link), fileOfKind(card.kind)).isFile) toMachine(library, link, dir)
                    else if (card != null) heldBy = card.machine?.let { machineName(File(sessions, it)) }
                }
            }
        }
        return Start(launchArguments(sessions, dir), heldBy)
    }

    // After the final save of a linked holder: the card goes to the drawer.
    fun releaseAtClose(sessions: File, dir: File): String? {
        val slot = readSlot(dir)
        val file = cardFileIn(dir)
        if (slot.link == null || slot.card != slot.link || file == null) return null
        if (!settled(dir, file, false)) return "The card stays in the machine: its last save isn't finished."
        val library = libraryFor(sessions)
        return LibraryLock(library).use { lock -> if (!lock.ok) BUSY else toDrawer(library, dir, slot.link) }
    }

    // ---- The running machine's slot (the :emulator process, run lock held) ----

    // After the core's take-out: the settled card file goes to the drawer with the serial and
    // protection the core reported (`identity` 64 hex digits, or null). A card not in Cards yet
    // gets its entry first; a serial another card has stays the new one the entry got.
    fun tookOut(sessions: File, dir: File, identity: String?, protected: Boolean): String? {
        val library = libraryFor(sessions)
        return LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            val slot = readSlot(dir)
            val file = cardFileIn(dir) ?: return "Couldn't find the card's file."
            var card = slot.card?.let { readCard(entryDir(library, it)) }
                ?: moveIn(library, dir, file, slot.copy(present = true), null) ?: return "Couldn't move the card into Cards."
            val reported = identity?.let { unhex(it) }
            if (reported != null && reported.size == 32 && !identityUsed(library, reported, card.id)) card = card.copy(identity = reported)
            card = card.copy(writeProtected = protected)
            writeCard(entryDir(library, card.id), card)
            toDrawer(library, dir, card.id)
        }
    }

    // Moves drawer card `id` into the running machine's empty slot for a put-in. Returns the card,
    // or null and the error.
    fun toRunning(sessions: File, dir: File, id: String): Pair<Card?, String?> {
        val library = libraryFor(sessions)
        return LibraryLock(library).use { lock ->
            if (!lock.ok) return null to BUSY
            toMachine(library, id, dir)?.let { return null to it }
            val card = readCard(entryDir(library, id)) ?: return null to "Couldn't find that card."
            card to null
        }
    }

    // A put-in the core refused: the card goes back to the drawer.
    fun putInFailed(sessions: File, dir: File, id: String) {
        val library = libraryFor(sessions)
        LibraryLock(library).use { lock -> if (lock.ok) toDrawer(library, dir, id) }
    }

    // After a save of the machine that succeeded.
    fun clearSwap(dir: File) {
        val slot = readSlot(dir)
        if (slot.present && slot.swap) writeSlot(dir, slot.copy(swap = false))
    }

    // ---- A machine's card when the machine goes ----

    // Moves the card of machine `dir` to the drawer before the machine is deleted; an unsettled one
    // as a settled copy. Machine held. Null on success, then the folder can go.
    fun keepCardOf(sessions: File, dir: File, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null,
                   cancelledMessage: String = "Delete cancelled."): String? {
        val library = libraryFor(sessions)
        if (descriptorsOf(sessionFile(dir)).first == 6L) return keepV6CardOf(sessions, dir, cancelled, progress)
        LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            // A save that got as far as its full card copy: settled the way a load does, so the card
            // is moved in rather than lost.
            if (cardFileIn(dir) == null) {
                val settle = sdSettleOf(dir)
                if (settle.full != null && !settle.full.renameTo(File(dir, settle.sibling))) return "Couldn't move the card into Cards."
            }
            reconcileLocked(sessions, dir)
            val slot = readSlot(dir)
            val file = cardFileIn(dir) ?: return null
            val id = slot.card ?: return "Couldn't move the card into Cards."
            if (settled(dir, file, slot.swap)) return toDrawer(library, dir, id)
        }
        // Copied outside the lock, then renamed in under it.
        val slot = readSlot(dir)
        val file = cardFileIn(dir) ?: return null
        val id = slot.card ?: return "Couldn't move the card into Cards."
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't move the card into Cards."
            when (val error = settledCopy(dir, file, File(copy.dir, file), cancelled, progress)) {
                null -> Unit
                "unsettled" -> return "Open ${machineName(dir)} once to finish its last save."
                "cancelled" -> return cancelledMessage
                else -> return error
            }
            LibraryLock(library).use { lock ->
                if (!lock.ok) return BUSY
                val card = readCard(entryDir(library, id)) ?: return "Couldn't move the card into Cards."
                if (!File(copy.dir, file).renameTo(File(entryDir(library, id), file))) return "Couldn't move the card into Cards."
                File(dir, file).delete()
                dir.listFiles { f -> f.name.startsWith("$file.journal-") }.orEmpty().forEach { it.delete() }
                writeCard(entryDir(library, id), card.copy(machine = null))
            }
        }
        return null
    }

    // The SD card inside a v6 .frond (the stream after the header and its 32-byte table), written
    // raw to `to`, with its serial as the core gives it at extraction (MD5 of the payload, hex). Null
    // serial and no error for a file with no card.
    class V6Card(val error: String?, val identity: ByteArray?)

    fun extractV6Card(frond: File, to: File, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null): V6Card {
        val unreadable = "Couldn't read the card inside this session."
        try {
            frond.inputStream().buffered(1 shl 20).use { input ->
                val head = ByteArray(72)
                var got = 0
                while (got < 72) { val n = input.read(head, got, 72 - got); if (n < 0) break; got += n }
                if (got != 72 || le32(head, 0) != 0x19800819L || le32(head, 4) != 6L) return V6Card(null, null)
                var skip = 0L
                for (i in 2 until 7) skip += le32(head, 40 + 4 * i)
                val sd = le32(head, 40 + 28)
                if (sd == 0L) return V6Card(null, null)
                if (sd % 512 != 0L || sd > (128L shl 20)) return V6Card(unreadable, null)
                val inflater = Inflater()
                val md5 = MessageDigest.getInstance("MD5")
                val inBuf = ByteArray(1 shl 20)
                val outBuf = ByteArray(1 shl 20)
                var skipped = 0L
                var written = 0L
                try {
                    to.outputStream().use { out ->
                        while (written < sd) {
                            if (cancelled()) { to.delete(); return V6Card("cancelled", null) }
                            if (inflater.needsInput()) {
                                val n = input.read(inBuf)
                                if (n < 0) { to.delete(); return V6Card(unreadable, null) }
                                inflater.setInput(inBuf, 0, n)
                            }
                            val want = if (skipped < skip) minOf(skip - skipped, outBuf.size.toLong()) else minOf(sd - written, outBuf.size.toLong())
                            val n = inflater.inflate(outBuf, 0, want.toInt())
                            if (n == 0 && (inflater.finished() || inflater.needsDictionary())) { to.delete(); return V6Card(unreadable, null) }
                            if (skipped < skip) skipped += n
                            else if (n > 0) {
                                md5.update(outBuf, 0, n)
                                out.write(outBuf, 0, n)
                                written += n
                                progress?.invoke(written, sd)
                            }
                        }
                        out.fd.sync()
                    }
                } finally {
                    inflater.end()
                }
                return V6Card(null, hex(md5.digest()).toByteArray(Charsets.US_ASCII))
            }
        } catch (e: java.util.zip.DataFormatException) {
            to.delete()
            return V6Card(unreadable, null)
        } catch (e: IOException) {
            to.delete()
            return V6Card("Couldn't read the session.", null)
        }
    }

    // A v6 machine's card is inside its .frond: it goes into the drawer as a new card, extracted
    // raw and made into a settled card file by the card tool, before the machine can be deleted.
    private fun keepV6CardOf(sessions: File, dir: File, cancelled: () -> Boolean, progress: ((Long, Long) -> Unit)?): String? {
        val library = libraryFor(sessions)
        val frond = sessionFile(dir) ?: return null
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't move the card into Cards."
            val raw = File(copy.dir, "card.img")
            // Raw copy first, then the card file: twice the card's size at most.
            val sdBytes = try {
                RandomAccessFile(frond, "r").use { f -> ByteArray(72).also { f.readFully(it) }.let { le32(it, 68) } }
            } catch (e: IOException) { 0L }
            val free = freeSpace(library)
            if (sdBytes > 0 && free >= 0 && free < 2 * sdBytes + SPARE_BYTES) return spaceMessage(2 * sdBytes)
            fun half(base: Long, all: Long) = progress?.let { p -> { done: Long, _: Long -> p(base + done / 2, all) } }
            val extracted = extractV6Card(frond, raw, cancelled, half(0, sdBytes))
            if (extracted.error == "cancelled") return "Delete cancelled."
            if (extracted.error != null) return "${extracted.error} It wasn't deleted."
            var identity = extracted.identity ?: return null
            val rawBytes = raw.length()
            val result = runTool(arrayOf("import", raw.path, "sd", File(copy.dir, "sd.frondsd").path), cancelled, half(rawBytes / 2, rawBytes))
            raw.delete()
            if (result.code == "cancelled") return "Delete cancelled."
            val logical = cardHeader(File(copy.dir, "sd.frondsd"))?.second
            if (!result.ok || logical != rawBytes)
                return "Couldn't move the card into Cards: ${if (result.ok) "the copy didn't check out." else result.message} It wasn't deleted."
            LibraryLock(library).use { lock ->
                if (!lock.ok) return BUSY
                // Two v6 machines duplicated from each other hold the same card bytes, so the same serial.
                if (identityUsed(library, identity)) identity = newIdentity()
                val card = Card(copy.id, freeCardName(library, "${machineName(dir)} card"), "sd", now(), null, logical, identity, false)
                if (!writeCard(copy.dir, card) || !copy.dir.renameTo(entryDir(library, copy.id))) return "Couldn't move the card into Cards."
                copy.taken()
            }
        }
        return null
    }

    // ---- The card tool ----

    class ToolResult(val ok: Boolean, val code: String, val message: String)

    // Runs the card tool on this thread; `cancelled` is checked at each progress report.
    fun runTool(args: Array<String>, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null): ToolResult {
        val cancel = AtomicBoolean(false)
        val reply = try {
            tool(args, CardToolProgress { done, total ->
                if (cancelled()) cancel.set(true)
                progress?.invoke(done, total)
            }, cancel)
        } catch (e: Throwable) {
            "error load ${e.message ?: "The card tool isn't available."}"
        }
        if (reply.startsWith("ok")) return ToolResult(true, "ok", reply)
        val parts = reply.split(' ', limit = 3)
        return ToolResult(false, parts.getOrElse(1) { "error" }, parts.getOrElse(2) { "Couldn't run the card tool." })
    }

    fun toolError(r: ToolResult) = if (r.code == "cancelled") COPY_CANCELLED else r.message

    // ---- Card operations. Each takes the library lock for its changes; copies run outside it. ----

    // A copy filled in `copy` becomes the card `card` (its id is the copy's).
    private fun adoptNew(library: File, copy: NewCopy, card: Card): String? = LibraryLock(library).use { lock ->
        if (!lock.ok) return BUSY
        if (!writeCard(copy.dir, card.copy(id = copy.id)) || !copy.dir.renameTo(entryDir(library, copy.id))) return "Couldn't add the card."
        copy.taken()
        null
    }

    // A new formatted card in the drawer: `size` in MiB for SD and Pro, MB for a classic stick.
    // Returns an error or null; `made` gets the new id.
    fun makeCard(sessions: File, name: String, kind: String, size: Int, cancelled: () -> Boolean = { false },
                 progress: ((Long, Long) -> Unit)? = null, made: ((String) -> Unit)? = null): String? {
        val library = libraryFor(sessions)
        val wanted = name.trim()
        if (wanted.isEmpty()) return "Give the card a name."
        if (freeCardName(library, wanted) != wanted) return "There's already a card called $wanted."
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't add the card."
            val file = File(copy.dir, fileOfKind(kind))
            val r = runTool(arrayOf("make", toolKind(kind), size.toString(), file.path), cancelled, progress)
            if (!r.ok) return toolError(r)
            val logical = cardHeader(file)?.second ?: return "Couldn't add the card."
            adoptNew(library, copy, Card(copy.id, wanted, kind, now(), null, logical, newIdentity(), false))?.let { return it }
            made?.invoke(copy.id)
        }
        return null
    }

    // An image into the drawer as a new card named `base` (numbered if taken).
    fun importCard(sessions: File, image: File, kind: String, base: String, cancelled: () -> Boolean = { false },
                   progress: ((Long, Long) -> Unit)? = null, made: ((String) -> Unit)? = null): String? {
        val library = libraryFor(sessions)
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't add the card."
            val file = File(copy.dir, fileOfKind(kind))
            val r = runTool(arrayOf("import", image.path, toolKind(kind), file.path), cancelled, progress)
            if (!r.ok) return toolError(r)
            val logical = cardHeader(file)?.second ?: return "Couldn't add the card."
            val name = LibraryLock(library).use { lock ->
                if (!lock.ok) return BUSY
                freeCardName(library, base.trim().ifEmpty { kindName(kind) })
            }
            adoptNew(library, copy, Card(copy.id, name, kind, now(), null, logical, newIdentity(), false))?.let { return it }
            made?.invoke(copy.id)
        }
        return null
    }

    // A folder copied once onto a new card named `name` (SD or Memory Stick Pro, `size` in MiB): the
    // card tool makes a new card file, then the tree goes onto it through the storage library in
    // one commit, as on the desktop. All inside cards/.new-<id>, so a refusal, an error or Cancel
    // leaves nothing. Progress runs 0 to 1000: the card file to 200, the files the rest.
    fun importFolderCard(sessions: File, root: FolderCard.Node, name: String, kind: String, size: Int,
                         cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null,
                         made: ((String) -> Unit)? = null): String? {
        val library = libraryFor(sessions)
        val wanted = name.trim()
        if (wanted.isEmpty()) return "Give the card a name."
        if (freeCardName(library, wanted) != wanted) return "There's already a card called $wanted."
        if (kind !in FolderCard.kinds() || size !in CardChoices.sizesFor(kind)) return "Choose an SD card or Memory Stick Pro size."
        val plan = FolderCard.measure(root, cancelled)
        plan.problem?.let { return it }
        if (!FolderCard.fits(plan, size)) return "The folder doesn't fit on a ${Device.sizeLabel(size)} card."
        val cardBytes = size.toLong() shl 20
        val free = freeSpace(library)
        if (free >= 0 && free < cardBytes + SPARE_BYTES) return spaceMessage(cardBytes)
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't add the card."
            val file = File(copy.dir, fileOfKind(kind))
            val r = runTool(arrayOf("make", toolKind(kind), size.toString(), file.path), cancelled) { done, total ->
                progress?.invoke(done * 200 / maxOf(total, 1), 1000)
            }
            if (!r.ok) return toolError(r)
            if (cancelled()) return COPY_CANCELLED
            try {
                CardFile.open(file, true).use { card ->
                    FatImage.over(card).use { fat ->
                        FolderCard.fill(fat, root, plan, cancelled) { done, total -> progress?.invoke(200 + done * 800 / total, 1000) }
                            ?.let { return it }
                    }
                }
            } catch (e: IOException) {
                return "Couldn't copy the folder: ${e.message ?: "write error"}"
            }
            val logical = cardHeader(file)?.second ?: return "Couldn't add the card."
            adoptNew(library, copy, Card(copy.id, wanted, kind, now(), null, logical, newIdentity(), false))?.let { return it }
            made?.invoke(copy.id)
        }
        return null
    }

    // Where a card's file is now, for a copy or an export: the drawer, or a stopped machine (whose
    // run lock the result holds until closed). An unfinished save is used as a load would settle it.
    class Source(val path: File, val card: Card, val holder: File?, val lock: RunLock?) : Closeable {
        override fun close() { lock?.close() }
    }

    fun cardSource(sessions: File, id: String): Pair<Source?, String?> {
        val library = libraryFor(sessions)
        val card = readCard(entryDir(library, id)) ?: return null to "Couldn't find that card."
        val file = fileOfKind(card.kind)
        val inDrawer = File(entryDir(library, id), file)
        if (inDrawer.isFile) {
            CardFile.notReady(inDrawer)?.let { return null to it }
            return Source(inDrawer, card, null, null) to null
        }
        val holder = holderOf(sessions, id) ?: return null to "Couldn't find that card."
        val lock = tryRunLock(holder) ?: return null to "${machineName(holder)} is running. Stop it first."
        CardFile.notReady(File(holder, file))?.let { lock.close(); return null to it }
        val slot = readSlot(holder)
        if (settled(holder, file, slot.swap)) return Source(File(holder, file), card, holder, lock) to null
        val settle = sdSettleOf(holder)
        if (settle.sibling == file && settle.full != null) return Source(settle.full, card, holder, lock) to null
        lock.close()
        return null to "Open ${machineName(holder)} once to finish its last save."
    }

    // The machine folder holding card `id`, if any.
    fun holderOf(sessions: File, id: String): File? {
        val card = readCard(entryDir(libraryFor(sessions), id)) ?: return null
        return sessions.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty().firstOrNull { dir ->
            readSlot(dir).card == id && File(dir, fileOfKind(card.kind)).isFile
        }
    }

    // The card as an image: .img for SD and Pro, .frondms for a classic stick.
    fun exportCardImage(sessions: File, id: String, image: File, cancelled: () -> Boolean = { false },
                        progress: ((Long, Long) -> Unit)? = null): String? {
        val (source, error) = cardSource(sessions, id)
        if (source == null) return error
        source.use {
            val r = runTool(arrayOf("export", it.path.path, image.path), cancelled, progress)
            return if (r.ok) null else toolError(r)
        }
    }

    // The raw image of an SD or Memory Stick Pro card written into the open file `fd`, with no temp
    // copy. A classic stick has no raw image, so use exportCardImage for it.
    fun exportCardImageFd(sessions: File, id: String, fd: Int, cancelled: () -> Boolean = { false },
                          progress: ((Long, Long) -> Unit)? = null): String? {
        val (source, error) = cardSource(sessions, id)
        if (source == null) return error
        source.use {
            val r = runTool(arrayOf("export-fd", it.path.path, fd.toString()), cancelled, progress)
            return if (r.ok) null else toolError(r)
        }
    }

    // A settled copy in the drawer as "Name 2", with its own serial and no links.
    fun duplicateCard(sessions: File, id: String, cancelled: () -> Boolean = { false },
                      progress: ((Long, Long) -> Unit)? = null, made: ((String) -> Unit)? = null): String? {
        val library = libraryFor(sessions)
        val (source, sourceError) = cardSource(sessions, id)
        if (source == null) return sourceError
        NewCopy(library).use { copy ->
            if (!copy.ok) return "Couldn't copy the card."
            val file = fileOfKind(source.card.kind)
            source.use {
                if (it.holder == null || it.path.name.endsWith(".full")) {
                    val total = it.path.length()
                    val free = freeSpace(library)
                    if (free >= 0 && free < total + SPARE_BYTES) return spaceMessage(total)
                    val done = try { copyChunked(it.path, File(copy.dir, file), cancelled, progress) } catch (e: IOException) { return "Couldn't copy the card." }
                    if (!done) return COPY_CANCELLED
                } else {
                    when (val error = settledCopy(it.holder, file, File(copy.dir, file), cancelled, progress)) {
                        null -> Unit
                        "cancelled" -> return COPY_CANCELLED
                        "unsettled" -> return "Open ${machineName(it.holder)} once to finish its last save."
                        else -> return error
                    }
                }
            }
            val name = LibraryLock(library).use { lock ->
                if (!lock.ok) return BUSY
                freeCardName(library, source.card.name)
            }
            adoptNew(library, copy, source.card.copy(name = name, created = now(), machine = null, identity = newIdentity()))?.let { return it }
            made?.invoke(copy.id)
        }
        return null
    }

    // card.json only. Names are unique.
    fun renameCard(sessions: File, id: String, wanted: String): String? {
        val library = libraryFor(sessions)
        val name = wanted.trim()
        if (name.isEmpty()) return "Give the card a name."
        return LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            val card = readCard(entryDir(library, id)) ?: return "Couldn't find that card."
            if (freeCardName(library, name, id) != name) return "There's already a card called $name."
            if (writeCard(entryDir(library, id), card.copy(name = name))) null else "Couldn't rename the card."
        }
    }

    // Only a card in the drawer. Links to it go (from machines that aren't running).
    fun deleteCard(sessions: File, id: String): String? {
        val library = libraryFor(sessions)
        return LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            val card = readCard(entryDir(library, id)) ?: return "Couldn't find that card."
            if (!File(entryDir(library, id), fileOfKind(card.kind)).isFile) return "Take the card out of its machine first."
            CardFile.notReady(File(entryDir(library, id), fileOfKind(card.kind)))?.let { return it }
            if (!entryDir(library, id).deleteRecursively()) return "Couldn't delete the card."
            for (dir in sessions.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty()) {
                val slot = readSlot(dir)
                if (slot.link != id) continue
                tryRunLock(dir)?.use { writeSlot(dir, slot.copy(link = null)) }
            }
            null
        }
    }

    // Take out of a stopped machine: settled, a rename; unsettled, a settled copy, or refused when
    // the journal can't be applied. The slot gets card null and swap.
    fun takeOutStopped(sessions: File, id: String, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null): String? {
        val library = libraryFor(sessions)
        val card = readCard(entryDir(library, id)) ?: return "Couldn't find that card."
        if (File(entryDir(library, id), fileOfKind(card.kind)).isFile) return null
        val dir = holderOf(sessions, id) ?: return "Couldn't find that card."
        tryRunLock(dir)?.use {
            keepCardOf(sessions, dir, cancelled, progress, COPY_CANCELLED)?.let { return it }
            val slot = readSlot(dir)
            if (slot.card != null && cardFileIn(dir) == null) writeSlot(dir, slot.copy(card = null, swap = true))
            return null
        }
        return "${machineName(dir)} is running. Stop it first."
    }

    // Whether `device` takes a card of this kind and size (MiB for SD and Pro, MB for a classic stick).
    fun deviceTakes(device: Device, kind: String, size: Int): Boolean = when (kind) {
        "sd" -> device.sdSlot && (size.toLong() shl 20) in device.sdMin..device.sdMax
        "ms" -> device.memoryStick && size in device.msSizes
        else -> device.memoryStick && size >= 256 && size <= device.msProMax
    }

    // A card's size in the units above, from its logical bytes. A classic stick snaps to the nearest size.
    fun sizeOf(kind: String, bytes: Long): Int {
        val mib = ((bytes + (1L shl 19)) shr 20).toInt()
        if (kind != "ms") return mib
        return Device.MEMORY_STICK_SIZES.minByOrNull { kotlin.math.abs(it - mib) } ?: mib
    }

    // Links `id` to the machines in `linked` and unlinks it from the others. A machine linked to
    // another card moves its link (its name goes in `moved`). One holding a different card, one that
    // doesn't take the card, or one running, is left alone. `held` is the caller's own machine.
    fun setLinks(sessions: File, id: String, linked: List<File>, held: File? = null, moved: MutableList<String>? = null): String? {
        val library = libraryFor(sessions)
        LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            val card = readCard(entryDir(library, id)) ?: return "Couldn't find that card."
            val wanted = linked.map { it.canonicalFile }.toSet()
            var error: String? = null
            for (dir in sessions.listFiles { f -> f.isDirectory && !f.name.startsWith(".") }.orEmpty().sortedBy { it.name }) {
                if (sessionFile(dir) == null) continue
                val device = machineDevice(dir) ?: continue
                val slot = readSlot(dir)
                val want = dir.canonicalFile in wanted
                if (want == (slot.link == id)) continue
                val own = held != null && held.canonicalFile == dir.canonicalFile
                val machineLock = if (own) null else tryRunLock(dir)
                if (!own && machineLock == null) { error = "${machineName(dir)} is running. Stop it first."; continue }
                machineLock.use {
                    val next = if (want) {
                        if (!deviceTakes(device, card.kind, sizeOf(card.kind, card.bytes))) return@use
                        if (cardFileIn(dir) != null && slot.card != id) return@use
                        if (slot.link != null) moved?.add(machineName(dir))
                        slot.copy(present = true, link = id)
                    } else slot.copy(present = true, link = null)
                    if (!writeSlot(dir, next)) error = "Couldn't link ${machineName(dir)}."
                }
            }
            return error
        }
    }

    // Save a Copy: the copy carries the running machine's slot.json, as an export does. Its .frond
    // was just saved with the card it has, so it needs no swap.
    fun stageSlot(machineDir: File, copyDir: File): Boolean {
        val slot = readSlot(machineDir)
        if (!slot.present) return true
        return writeSlot(copyDir, slot.copy(swap = false, card = if (cardFileIn(copyDir) == null) null else slot.card))
    }

    // An imported machine's packed card becomes a new library card, named from slot.json or
    // `fallbackName`, with its own serial (the machine it came from may still have the card);
    // links aren't carried over.
    fun adoptImportedCard(sessions: File, dir: File, fallbackName: String): String? {
        val library = libraryFor(sessions)
        return LibraryLock(library).use { lock ->
            if (!lock.ok) return BUSY
            val slot = readSlot(dir)
            val file = cardFileIn(dir)
            if (file == null) {
                if (!slot.present) return null
                return if (writeSlot(dir, slot.copy(card = null, link = null))) null else "Couldn't save the session."
            }
            if (moveIn(library, dir, file, slot.copy(present = true, link = null), slot.name ?: fallbackName, freshIdentity = true) == null)
                "Couldn't save the session." else null
        }
    }
}
