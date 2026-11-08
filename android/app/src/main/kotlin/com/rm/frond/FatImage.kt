package com.rm.frond

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.Calendar
import java.util.TreeMap
import java.util.zip.CRC32

// Reads and edits a FAT16 SD card image in place: the card the core makes (one primary FAT16
// partition) or a FAT16 superfloppy, 32 MiB to 2 GiB. It does what the desktop's SD exchange does with
// mtools: list folders, copy files in and out, make folders, and copy, move, rename and delete inside the card.
// Data is never overwritten in place: new data goes to free clusters.
// Each change is crash safe: new file data goes into free clusters, then the changed FAT and folder
// sectors go through a journal next to the card (<card>.journal) before they're written in place.
// open() replays a complete journal and drops a damaged one. Not thread safe; one editor per image file.
class FatImage private constructor(private val image: File, private val file: RandomAccessFile) : AutoCloseable {
    class FatException(message: String) : IOException(message)
    // A copy stopped by its caller; the change is rolled back.
    class Cancelled : IOException("Cancelled")

    data class Entry(
        val name: String,
        val shortName: String,
        val directory: Boolean,
        val size: Long,
        val firstCluster: Int,
        // Last change, in milliseconds since 1970; 0 when the card has none.
        val modified: Long = 0L,
    )

    private var offset = 0L
    private var clusterSectors = 0
    private var fatStart = 0L
    private var fatCount = 0
    private var fatSectors = 0
    private var rootStart = 0L
    private var rootEntries = 0
    private var dataStart = 0L
    private var clusterCount = 0
    private lateinit var fat: IntArray
    // The FAT as it is on the card, to find the changed sectors and to roll back a failed change.
    private lateinit var fatOnDisk: IntArray
    // Metadata sectors changed by the current change, by byte offset in the image, until commit.
    private val staged = TreeMap<Long, ByteArray>()

    // Bytes written to the image and its journal, and whether any change was committed.
    var bytesWritten = 0L
        private set
    var committed = false
        private set
    // Tests stop a change here: "data", "journal", "partly" and "applied".
    internal var crashHook: ((String) -> Unit)? = null
    // For a card that is being built and is thrown away if it fails: changes go straight into the
    // image, with no journal or syncs. Call sync() when done.
    var bulk = false

    val clusterBytes get() = clusterSectors * SECTOR

    val freeBytes: Long get() = (2 until clusterCount + 2).count { fat[it] == 0 }.toLong() * clusterBytes
    val totalBytes: Long get() = clusterCount.toLong() * clusterBytes

    private fun open() {
        val size = file.length()
        if (size < MIN_BYTES || size > MAX_BYTES || size % SECTOR != 0L) throw FatException("Use a 32 MB to 2 GB FAT16 card image.")
        var boot = read(0, SECTOR)
        if (String(boot, 54, 8, Charsets.US_ASCII) != "FAT16   ") {
            // A single primary partition, as the core makes. Never guess offsets.
            val type = boot[450].toInt() and 0xff
            val begin = u32(boot, 454)
            val sectors = u32(boot, 458)
            if (u16(boot, 510) != 0xaa55 || type !in listOf(4, 6, 14) || begin == 0L || sectors == 0L ||
                (begin + sectors) * SECTOR > size
            ) throw FatException("The card has no FAT16 partition.")
            offset = begin * SECTOR
            boot = read(offset, SECTOR)
        }
        if (String(boot, 54, 8, Charsets.US_ASCII) != "FAT16   " || u16(boot, 11) != SECTOR || u16(boot, 510) != 0xaa55)
            throw FatException("The card doesn't hold a FAT16 file system.")
        clusterSectors = boot[13].toInt() and 0xff
        val reserved = u16(boot, 14)
        fatCount = boot[16].toInt() and 0xff
        rootEntries = u16(boot, 17)
        val total = u16(boot, 19).toLong().takeIf { it != 0L } ?: u32(boot, 32)
        fatSectors = u16(boot, 22)
        if (clusterSectors == 0 || clusterSectors and (clusterSectors - 1) != 0 || reserved == 0 || fatCount == 0 ||
            fatSectors == 0 || rootEntries == 0
        ) throw FatException("The card's FAT16 header is damaged.")
        fatStart = offset + reserved.toLong() * SECTOR
        rootStart = fatStart + fatCount.toLong() * fatSectors * SECTOR
        val rootSectors = (rootEntries * 32 + SECTOR - 1) / SECTOR
        dataStart = rootStart + rootSectors.toLong() * SECTOR
        val dataSectors = total - (reserved + fatCount.toLong() * fatSectors + rootSectors)
        clusterCount = (dataSectors / clusterSectors).toInt()
        if (clusterCount < 4085 || clusterCount >= 65525 || offset + total * SECTOR > size ||
            (clusterCount + 2) * 2 > fatSectors * SECTOR
        ) throw FatException("The card's FAT16 header is damaged.")
        val raw = read(fatStart, (clusterCount + 2) * 2)
        fat = IntArray(clusterCount + 2) { u16(raw, it * 2) }
        fatOnDisk = fat.copyOf()
    }

    // A folder: the fixed root area, or a cluster chain.
    private inner class Folder(val cluster: Int) {
        val root get() = cluster == 0

        fun clusters(): List<Int> = chain(cluster)

        fun bytes(): ByteArray =
            if (root) read(rootStart, rootEntries * 32)
            else {
                val list = clusters()
                val out = ByteArray(list.size * clusterBytes)
                list.forEachIndexed { i, c -> read(clusterAt(c), clusterBytes).copyInto(out, i * clusterBytes) }
                out
            }

        fun writeSlots(bytes: ByteArray, first: Int, count: Int) {
            val list = if (root) emptyList() else clusters()
            for (slot in first until first + count) {
                val at = slot * 32
                val position = if (root) rootStart + at else clusterAt(list[at / clusterBytes]) + at % clusterBytes
                stage(position, bytes, at, 32)
            }
        }
    }

    private fun clusterAt(cluster: Int) = dataStart + (cluster - 2).toLong() * clusterBytes

    private fun chain(start: Int): List<Int> {
        val list = mutableListOf<Int>()
        var c = start
        while (c in 2 until clusterCount + 2) {
            if (list.size > clusterCount) throw FatException("The card's FAT has a loop.")
            list += c
            c = fat[c]
        }
        if (c in 1 until 0xfff8 && c != 0) throw FatException("The card's FAT is damaged.")
        return list
    }

    private class Slot(val entry: Entry, val first: Int, val count: Int)

    private fun slots(folder: Folder): List<Slot> {
        val bytes = folder.bytes()
        val result = mutableListOf<Slot>()
        val longParts = arrayOfNulls<String>(20)
        var longFirst = -1
        var longSum = -1
        var longCount = 0
        for (i in 0 until bytes.size / 32) {
            val at = i * 32
            val first = bytes[at].toInt() and 0xff
            if (first == 0) break
            val attr = bytes[at + 11].toInt() and 0xff
            if (first == 0xe5) { longFirst = -1; continue }
            if (attr and 0x3f == 0x0f) {
                val order = first and 0x3f
                if (first and 0x40 != 0) {
                    longParts.fill(null)
                    longFirst = i
                    longCount = order
                    longSum = bytes[at + 13].toInt() and 0xff
                }
                if (longFirst >= 0 && order in 1..20 && (bytes[at + 13].toInt() and 0xff) == longSum) {
                    longParts[order - 1] = longPart(bytes, at)
                } else longFirst = -1
                continue
            }
            val short = shortDisplay(bytes, at)
            val sum = checksum(bytes, at)
            val long = if (longFirst >= 0 && sum == longSum && (0 until longCount).all { longParts[it] != null })
                (0 until longCount).joinToString("") { longParts[it]!! } else null
            val start = if (long != null) longFirst else i
            longFirst = -1
            if (attr and 0x08 != 0) continue
            if (short == "." || short == "..") continue
            val entry = Entry(long ?: short, short, attr and 0x10 != 0, u32(bytes, at + 28), u16(bytes, at + 26),
                dosMillis(u16(bytes, at + 24), u16(bytes, at + 22)))
            result += Slot(entry, start, i - start + 1)
        }
        return result
    }

    private fun folder(path: String): Folder {
        var folder = Folder(0)
        for (part in parts(path)) {
            val entry = slots(folder).firstOrNull { it.entry.name.equals(part, ignoreCase = true) }?.entry
                ?: throw FatException("There's no folder $part on the card.")
            if (!entry.directory) throw FatException("$part is a file.")
            folder = Folder(entry.firstCluster)
        }
        return folder
    }

    // Lists a folder, "/" or "/PALM/Launcher". Folders first, then files, by name.
    fun list(path: String): List<Entry> =
        slots(folder(path)).map { it.entry }.sortedWith(compareBy({ !it.directory }, { it.name.lowercase() }))

    fun find(path: String): Entry? {
        val p = parts(path)
        if (p.isEmpty()) return null
        return slots(folder(p.dropLast(1).joinToString("/", "/"))).firstOrNull { it.entry.name.equals(p.last(), ignoreCase = true) }?.entry
    }

    fun read(entry: Entry, out: OutputStream) {
        if (entry.directory) throw FatException("${entry.name} is a folder.")
        var left = entry.size
        val buffer = ByteArray(clusterBytes)
        for (c in chain(entry.firstCluster)) {
            if (left <= 0) break
            val n = minOf(left, clusterBytes.toLong()).toInt()
            file.seek(clusterAt(c))
            file.readFully(buffer, 0, n)
            out.write(buffer, 0, n)
            left -= n
        }
        if (left > 0) throw FatException("${entry.name} is shorter on the card than its size says.")
    }

    // Copies size bytes from input into a new file in the folder at path.
    fun add(path: String, name: String, input: InputStream, size: Long) = change {
        checkName(name)
        val folder = folder(path)
        val existing = slots(folder)
        refuseTaken(existing, name)
        val needed = ((size + clusterBytes - 1) / clusterBytes).toInt()
        val clusters = allocate(needed)
        val buffer = ByteArray(clusterBytes)
        var left = size
        for (c in clusters) {
            val n = minOf(left, clusterBytes.toLong()).toInt()
            var got = 0
            while (got < n) {
                val r = input.read(buffer, got, n - got)
                if (r < 0) throw FatException("The file ended early.")
                got += r
            }
            buffer.fill(0, n, clusterBytes)
            write(clusterAt(c), buffer, 0, clusterBytes)
            left -= n
        }
        addEntry(folder, existing, name, 0x20, clusters.firstOrNull() ?: 0, size)
    }

    fun makeFolder(path: String, name: String) = change {
        checkName(name)
        val parent = folder(path)
        val existing = slots(parent)
        refuseTaken(existing, name)
        val cluster = allocate(1).first()
        val (date, time) = now()
        write(clusterAt(cluster), folderBlock(cluster, parent.cluster, date, time), 0, clusterBytes)
        addEntry(parent, existing, name, 0x10, cluster, 0)
    }

    // A new folder's first cluster: the "." and ".." entries, then empty.
    private fun folderBlock(cluster: Int, parent: Int, date: Int, time: Int): ByteArray {
        val block = ByteArray(clusterBytes)
        fun dot(at: Int, label: String, target: Int) {
            label.toByteArray(Charsets.US_ASCII).copyInto(block, at)
            for (i in label.length until 11) block[at + i] = ' '.code.toByte()
            block[at + 11] = 0x10
            stamp(block, at, date, time)
            put16(block, at + 26, target)
        }
        dot(0, ".", cluster)
        dot(32, "..", parent)
        return block
    }

    private fun locate(path: String): Pair<Folder, Slot> {
        val p = parts(path)
        if (p.isEmpty()) throw FatException("That is the top folder.")
        val parent = folder(p.dropLast(1).joinToString("/", "/"))
        val slot = slots(parent).firstOrNull { it.entry.name.equals(p.last(), ignoreCase = true) }
            ?: throw FatException("There's no ${p.last()} on the card.")
        return parent to slot
    }

    private fun rawOf(parent: Folder, slot: Slot): ByteArray =
        parent.bytes().copyOfRange((slot.first + slot.count - 1) * 32, (slot.first + slot.count) * 32)

    private fun eraseSlots(parent: Folder, slot: Slot) {
        val bytes = parent.bytes()
        for (i in slot.first until slot.first + slot.count) bytes[i * 32] = 0xe5.toByte()
        parent.writeSlots(bytes, slot.first, slot.count)
    }

    private fun freeTree(entry: Entry, depth: Int) {
        if (depth > MAX_DEPTH) throw FatException("Folders are nested too deeply.")
        if (entry.directory) for (s in slots(Folder(entry.firstCluster))) freeTree(s.entry, depth + 1)
        for (c in chain(entry.firstCluster)) fat[c] = 0
    }

    // Deletes a file, or a folder with everything in it. The space is free again.
    fun remove(path: String) = change {
        val (parent, slot) = locate(path)
        freeTree(slot.entry, 0)
        eraseSlots(parent, slot)
    }

    // Moves a file or folder into toFolder, under newName when given (a rename when the folder is the
    // same). A folder can't go into itself.
    fun move(from: String, toFolder: String, newName: String? = null) = change {
        val (parent, slot) = locate(from)
        val name = newName ?: slot.entry.name
        checkName(name)
        val dest = folder(toFolder)
        if (slot.entry.directory && within(from, toFolder)) throw FatException("${slot.entry.name} can't go into itself.")
        val raw = rawOf(parent, slot)
        eraseSlots(parent, slot)
        val existing = slots(dest)
        refuseTaken(existing, name)
        addEntry(dest, existing, name, raw[11].toInt() and 0xff, slot.entry.firstCluster, slot.entry.size, template = raw)
        if (slot.entry.directory && dest.cluster != parent.cluster) {
            val inner = Folder(slot.entry.firstCluster)
            val bytes = inner.bytes()
            put16(bytes, 32 + 26, dest.cluster)
            inner.writeSlots(bytes, 1, 1)
        }
    }

    // Copies a file or folder into toFolder under newName (or its own name). `bytes` gets the
    // amount copied so far; `cancelled` stops it. A folder can't be copied into itself.
    fun copy(from: String, toFolder: String, newName: String? = null, cancelled: () -> Boolean = { false },
             bytes: (Long) -> Unit = {}) = change {
        val (parent, slot) = locate(from)
        val name = newName ?: slot.entry.name
        checkName(name)
        val dest = folder(toFolder)
        if (slot.entry.directory && within(from, toFolder)) throw FatException("${slot.entry.name} can't go into itself.")
        refuseTaken(slots(dest), name)
        var done = 0L
        copyOne(rawOf(parent, slot), slot.entry, dest, name, 0, cancelled) { done += it; bytes(done) }
    }

    private fun copyOne(raw: ByteArray, entry: Entry, dest: Folder, name: String, depth: Int, cancelled: () -> Boolean, bytes: (Long) -> Unit) {
        if (depth > MAX_DEPTH) throw FatException("Folders are nested too deeply.")
        if (cancelled()) throw Cancelled()
        val existing = slots(dest)
        if (entry.directory) {
            val cluster = allocate(1).first()
            write(clusterAt(cluster), folderBlock(cluster, dest.cluster, u16(raw, 24), u16(raw, 22)), 0, clusterBytes)
            addEntry(dest, existing, name, 0x10, cluster, 0, template = raw)
            val inner = Folder(entry.firstCluster)
            val innerBytes = inner.bytes()
            val target = Folder(cluster)
            for (child in slots(inner)) {
                val childRaw = innerBytes.copyOfRange((child.first + child.count - 1) * 32, (child.first + child.count) * 32)
                copyOne(childRaw, child.entry, target, child.entry.name, depth + 1, cancelled, bytes)
            }
        } else {
            val source = chain(entry.firstCluster)
            val clusters = allocate(source.size)
            val buffer = ByteArray(clusterBytes)
            var left = entry.size
            for (i in clusters.indices) {
                if (cancelled()) throw Cancelled()
                val n = minOf(left, clusterBytes.toLong()).toInt()
                file.seek(clusterAt(source[i]))
                file.readFully(buffer, 0, n)
                buffer.fill(0, n, clusterBytes)
                write(clusterAt(clusters[i]), buffer, 0, clusterBytes)
                left -= n
                bytes(n.toLong())
            }
            addEntry(dest, existing, name, raw[11].toInt() and 0xff, clusters.firstOrNull() ?: 0, entry.size, template = raw)
        }
    }

    // The bytes in a file, or in every file under a folder.
    fun sizeOf(path: String): Long {
        val (parent, slot) = locate(path)
        fun total(e: Entry, depth: Int): Long {
            if (depth > MAX_DEPTH) throw FatException("Folders are nested too deeply.")
            return if (e.directory) slots(Folder(e.firstCluster)).sumOf { total(it.entry, depth + 1) } else e.size
        }
        return total(slot.entry, 0)
    }

    private fun refuseTaken(existing: List<Slot>, name: String) {
        if (existing.any { it.entry.name.equals(name, ignoreCase = true) || it.entry.shortName.equals(name, ignoreCase = true) })
            throw FatException("$name is already on the card. Choose another name.")
    }

    private fun allocate(count: Int): List<Int> {
        val free = mutableListOf<Int>()
        var c = 2
        while (free.size < count && c < clusterCount + 2) {
            if (fat[c] == 0) free += c
            c++
        }
        if (free.size < count) throw FatException("There isn't enough space on the card.")
        for (i in free.indices) fat[free[i]] = if (i + 1 < free.size) free[i + 1] else 0xffff
        return free
    }

    private fun addEntry(folder: Folder, existing: List<Slot>, name: String, attr: Int, cluster: Int, size: Long, template: ByteArray? = null) {
        val short = shortName(name, existing.map { it.entry.shortName.uppercase() }.toSet())
        val needsLong = displayOf(short) != name
        val longCount = if (needsLong) (name.length + 12) / 13 else 0
        val total = longCount + 1
        val (bytes, first) = freeSlots(folder, total)
        val sum = checksum(short, 0)
        for (k in 0 until longCount) {
            val order = longCount - k
            val at = (first + k) * 32
            bytes.fill(0, at, at + 32)
            bytes[at] = (order or if (k == 0) 0x40 else 0).toByte()
            bytes[at + 11] = 0x0f
            bytes[at + 13] = sum.toByte()
            val chars = IntArray(13) { j ->
                val index = (order - 1) * 13 + j
                when {
                    index < name.length -> name[index].code
                    index == name.length -> 0
                    else -> 0xffff
                }
            }
            LONG_OFFSETS.forEachIndexed { j, o -> put16(bytes, at + o, chars[j]) }
        }
        val at = (first + longCount) * 32
        bytes.fill(0, at, at + 32)
        short.copyInto(bytes, at)
        bytes[at + 11] = attr.toByte()
        val (date, time) = now()
        stamp(bytes, at, date, time)
        // A moved or copied entry keeps its dates.
        if (template != null) {
            for (i in 13..19) bytes[at + i] = template[i]
            for (i in 22..25) bytes[at + i] = template[i]
        }
        put16(bytes, at + 26, cluster)
        put32(bytes, at + 28, size)
        folder.writeSlots(bytes, first, total)
    }

    // Finds count free slots in a row, adding a cluster to a full subfolder. Returns the folder's
    // bytes (grown if needed) and the first slot.
    private fun freeSlots(folder: Folder, count: Int): Pair<ByteArray, Int> {
        var bytes = folder.bytes()
        fun search(): Int {
            var run = 0
            for (i in 0 until bytes.size / 32) {
                val first = bytes[i * 32].toInt() and 0xff
                if (first == 0 || first == 0xe5) {
                    run++
                    if (run == count) return i - count + 1
                } else run = 0
            }
            return -1
        }
        var found = search()
        while (found < 0) {
            if (folder.root) throw FatException("The card's top folder is full. Use a subfolder.")
            val last = folder.clusters().last()
            val added = allocate(1).first()
            fat[last] = added
            write(clusterAt(added), ByteArray(clusterBytes), 0, clusterBytes)
            bytes = folder.bytes()
            found = search()
        }
        return bytes to found
    }

    // Runs one change and commits it, or rolls the in-memory state back when it fails. Data
    // already written went into clusters the card's FAT still calls free.
    private fun change(block: () -> Unit) {
        try {
            block()
            commit()
        } catch (e: Throwable) {
            fat = fatOnDisk.copyOf()
            staged.clear()
            throw e
        }
    }

    // Stages the FAT sectors that differ from the card, in every FAT copy.
    private fun stageFat() {
        val sectors = sortedSetOf<Int>()
        for (i in fat.indices) if (fat[i] != fatOnDisk[i]) sectors += i * 2 / SECTOR
        for (s in sectors) {
            val from = s * SECTOR / 2
            val until = minOf(from + SECTOR / 2, fat.size)
            val raw = ByteArray((until - from) * 2)
            for (i in from until until) put16(raw, (i - from) * 2, fat[i])
            for (copy in 0 until fatCount) stage(fatStart + copy.toLong() * fatSectors * SECTOR + s.toLong() * SECTOR, raw, 0, raw.size)
        }
    }

    // Syncs the data, writes and syncs the journal, applies it in place, syncs and drops it.
    private fun commit() {
        stageFat()
        if (staged.isEmpty()) return
        if (bulk) {
            for ((at, sector) in staged) write(at, sector, 0, SECTOR)
            fatOnDisk = fat.copyOf()
            staged.clear()
            committed = true
            return
        }
        file.fd.sync()
        crashHook?.invoke("data")
        val records = staged.entries.map { it.key to it.value }
        val bytes = encodeJournal(records, file.length())
        val journal = journalOf(image)
        FileOutputStream(journal).use { it.write(bytes); it.fd.sync() }
        syncDir(journal)
        bytesWritten += bytes.size
        crashHook?.invoke("journal")
        records.forEachIndexed { i, (at, sector) ->
            if (i == records.size / 2) crashHook?.invoke("partly")
            write(at, sector, 0, SECTOR)
        }
        file.fd.sync()
        crashHook?.invoke("applied")
        journal.delete()
        syncDir(journal)
        fatOnDisk = fat.copyOf()
        staged.clear()
        committed = true
    }

    private fun read(at: Long, length: Int): ByteArray {
        val b = ByteArray(length)
        file.seek(at)
        file.readFully(b)
        // Staged sectors that overlap, so a change sees its own edits.
        val first = at / SECTOR * SECTOR
        for ((sectorAt, sector) in staged.subMap(first, at + length)) {
            val from = maxOf(at, sectorAt)
            val until = minOf(at + length, sectorAt + SECTOR)
            sector.copyInto(b, (from - at).toInt(), (from - sectorAt).toInt(), (until - sectorAt).toInt())
        }
        return b
    }

    // Writes straight to the image: new data in free clusters, or a journal being applied.
    private fun write(at: Long, b: ByteArray, from: Int, length: Int) {
        file.seek(at)
        file.write(b, from, length)
        bytesWritten += length
    }

    // Keeps a metadata edit in memory, whole sectors, until commit.
    private fun stage(at: Long, b: ByteArray, from: Int, length: Int) {
        var done = 0
        while (done < length) {
            val position = at + done
            val sectorAt = position / SECTOR * SECTOR
            val sector = staged.getOrPut(sectorAt) { file.seek(sectorAt); ByteArray(SECTOR).also { file.readFully(it) } }
            val inSector = (position - sectorAt).toInt()
            val n = minOf(SECTOR - inSector, length - done)
            b.copyInto(sector, inSector, from + done, from + done + n)
            done += n
        }
    }

    // Whether every FAT copy on the card matches the first.
    internal fun fatCopiesMatch(): Boolean {
        val first = read(fatStart, fatSectors * SECTOR)
        return (1 until fatCount).all { read(fatStart + it.toLong() * fatSectors * SECTOR, fatSectors * SECTOR).contentEquals(first) }
    }

    fun sync() {
        file.fd.sync()
    }

    override fun close() {
        file.close()
    }

    companion object {
        const val SECTOR = 512
        const val MIN_BYTES = 32L shl 20
        const val MAX_BYTES = 2048L shl 20
        // Cards up to this size live in the core's memory until they're saved.
        const val SMALL_MAX_BYTES = 128L shl 20
        private val LONG_OFFSETS = intArrayOf(1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
        // Characters FAT refuses in long names, as the desktop's exchange does.
        private val FORBIDDEN = "\"*/:<>?\\|"

        private val JOURNAL_MAGIC = "FRONDFJ1".toByteArray(Charsets.US_ASCII)
        private val JOURNAL_END = "FJEND\u0000\u0000\u0000".toByteArray(Charsets.US_ASCII)
        private const val JOURNAL_HEADER = 20
        private const val JOURNAL_RECORD = 8 + SECTOR
        private const val JOURNAL_TRAILER = 12
        private const val JOURNAL_MAX_BYTES = 16L shl 20

        fun journalOf(image: File) = File(image.path + ".journal")

        // Opens a card, first replaying a journal left by a change that was cut off.
        fun open(image: File, writable: Boolean): FatImage {
            recover(image)
            val raf = RandomAccessFile(image, if (writable) "rw" else "r")
            try {
                return FatImage(image, raf).also { it.open() }
            } catch (e: Exception) {
                raf.close()
                throw e
            }
        }

        // Applies a complete journal to its card and deletes it; deletes an incomplete or damaged one.
        // Applying twice is harmless, so a crash while replaying just replays again.
        fun recover(image: File) {
            val journal = journalOf(image)
            if (!journal.exists()) return
            val records = if (journal.length() > JOURNAL_MAX_BYTES) null else decodeJournal(journal.readBytes(), image.length())
            if (records != null) {
                RandomAccessFile(image, "rw").use { raf ->
                    for ((at, sector) in records) { raf.seek(at); raf.write(sector) }
                    raf.fd.sync()
                }
            }
            if (!journal.delete() && journal.exists()) throw FatException("Couldn't finish the last change to the card.")
            syncDir(journal)
        }

        // Header: magic, record count (u32), image length (u64). Records: offset (u64), one sector.
        // Trailer: end magic and the CRC-32 of everything before it. Big endian.
        internal fun encodeJournal(records: List<Pair<Long, ByteArray>>, imageLength: Long): ByteArray {
            val out = java.io.ByteArrayOutputStream(JOURNAL_HEADER + records.size * JOURNAL_RECORD + JOURNAL_TRAILER)
            val data = java.io.DataOutputStream(out)
            data.write(JOURNAL_MAGIC)
            data.writeInt(records.size)
            data.writeLong(imageLength)
            for ((at, sector) in records) { data.writeLong(at); data.write(sector, 0, SECTOR) }
            data.write(JOURNAL_END)
            val crc = CRC32().also { it.update(out.toByteArray()) }
            data.writeInt(crc.value.toInt())
            data.flush()
            return out.toByteArray()
        }

        // The records of a complete, undamaged journal for an image of imageLength bytes, or null.
        internal fun decodeJournal(b: ByteArray, imageLength: Long): List<Pair<Long, ByteArray>>? {
            if (b.size < JOURNAL_HEADER + JOURNAL_TRAILER) return null
            val data = java.nio.ByteBuffer.wrap(b)
            if (!b.copyOfRange(0, 8).contentEquals(JOURNAL_MAGIC)) return null
            val count = data.getInt(8)
            if (count < 0 || b.size.toLong() != JOURNAL_HEADER + count.toLong() * JOURNAL_RECORD + JOURNAL_TRAILER) return null
            if (data.getLong(12) != imageLength) return null
            val end = b.size - JOURNAL_TRAILER
            if (!b.copyOfRange(end, end + 8).contentEquals(JOURNAL_END)) return null
            val crc = CRC32().also { it.update(b, 0, end + 8) }
            if (crc.value.toInt() != data.getInt(end + 8)) return null
            return (0 until count).map { i ->
                val at = JOURNAL_HEADER + i * JOURNAL_RECORD
                val offset = data.getLong(at)
                if (offset < 0 || offset % SECTOR != 0L || offset + SECTOR > imageLength) return null
                offset to b.copyOfRange(at + 8, at + JOURNAL_RECORD)
            }
        }

        // Makes a created or deleted journal's name durable. Best effort: not every file system allows it.
        private fun syncDir(file: File) {
            val dir = file.absoluteFile.parentFile ?: return
            try { FileChannel.open(dir.toPath(), StandardOpenOption.READ).use { it.force(true) } } catch (_: Exception) { }
        }

        fun parts(path: String): List<String> = path.split('/').filter { it.isNotEmpty() }

        private const val MAX_DEPTH = 64

        // Whether `inner` is `outer` or below it.
        fun within(outer: String, inner: String): Boolean {
            val o = parts(outer).map { it.lowercase() }
            val i = parts(inner).map { it.lowercase() }
            return i.size >= o.size && i.take(o.size) == o
        }

        private fun dosMillis(date: Int, time: Int): Long {
            if (date == 0) return 0L
            val c = Calendar.getInstance()
            c.clear()
            c.set(1980 + (date shr 9), ((date shr 5) and 15) - 1, date and 31, time shr 11, (time shr 5) and 63, (time and 31) * 2)
            return c.timeInMillis
        }

        fun checkName(name: String) {
            if (name.isEmpty() || name.length > 255 || name == "." || name == ".." || name.endsWith('.') ||
                name.endsWith(' ') || name.startsWith(' ') || name.any { it.code < 0x20 || it.code == 0x7f || it in FORBIDDEN }
            ) throw FatException("FAT can't use the name \"$name\".")
        }

        private fun u16(b: ByteArray, at: Int) = (b[at].toInt() and 0xff) or ((b[at + 1].toInt() and 0xff) shl 8)
        private fun u32(b: ByteArray, at: Int) = u16(b, at).toLong() or (u16(b, at + 2).toLong() shl 16)
        private fun put16(b: ByteArray, at: Int, v: Int) { b[at] = v.toByte(); b[at + 1] = (v shr 8).toByte() }
        private fun put32(b: ByteArray, at: Int, v: Long) { put16(b, at, (v and 0xffff).toInt()); put16(b, at + 2, (v shr 16).toInt()) }

        private fun longPart(b: ByteArray, at: Int): String {
            val s = StringBuilder()
            for (o in LONG_OFFSETS) {
                val c = u16(b, at + o)
                if (c == 0 || c == 0xffff) break
                s.append(c.toChar())
            }
            return s.toString()
        }

        private fun shortDisplay(b: ByteArray, at: Int): String {
            val case = b[at + 12].toInt()
            val raw = ByteArray(11) { b[at + it] }
            if (raw[0] == 0x05.toByte()) raw[0] = 0xe5.toByte()
            var base = String(raw, 0, 8, Charsets.ISO_8859_1).trimEnd()
            var ext = String(raw, 8, 3, Charsets.ISO_8859_1).trimEnd()
            if (case and 0x08 != 0) base = base.lowercase()
            if (case and 0x10 != 0) ext = ext.lowercase()
            return if (ext.isEmpty()) base else "$base.$ext"
        }

        private fun displayOf(short: ByteArray): String {
            val base = String(short, 0, 8, Charsets.ISO_8859_1).trimEnd()
            val ext = String(short, 8, 3, Charsets.ISO_8859_1).trimEnd()
            return if (ext.isEmpty()) base else "$base.$ext"
        }

        private fun checksum(b: ByteArray, at: Int): Int {
            var sum = 0
            for (i in 0 until 11) sum = (((sum and 1) shl 7) + (sum shr 1) + (b[at + i].toInt() and 0xff)) and 0xff
            return sum
        }

        // An 8.3 name for a long name, Windows style: upper case, invalid characters as _, and a
        // ~N tail when anything was lost or the name is taken.
        fun shortName(name: String, taken: Set<String>): ByteArray {
            val upper = name.uppercase()
            val dot = upper.lastIndexOf('.').takeIf { it > 0 } ?: -1
            var lossy = false
            fun clean(s: String): String {
                val out = StringBuilder()
                for (ch in s) {
                    when {
                        ch == ' ' || ch == '.' -> lossy = true
                        ch.code > 0x7e || ch in "+,;=[]" -> { out.append('_'); lossy = true }
                        else -> out.append(ch)
                    }
                }
                return out.toString()
            }
            var base = clean(if (dot >= 0) upper.substring(0, dot) else upper)
            var ext = if (dot >= 0) clean(upper.substring(dot + 1)) else ""
            if (base.length > 8) { lossy = true }
            if (ext.length > 3) { ext = ext.substring(0, 3); lossy = true }
            if (base.isEmpty()) { base = "_"; lossy = true }
            fun pack(b: String, e: String) = ByteArray(11) { i ->
                val c = if (i < 8) b.getOrNull(i) else e.getOrNull(i - 8)
                (c?.code ?: ' '.code).toByte()
            }
            fun key(b: String, e: String) = if (e.isEmpty()) b else "$b.$e"
            if (!lossy && key(base, ext) !in taken) return pack(base, ext)
            for (n in 1..999_999) {
                val tail = "~$n"
                val b = base.take(8 - tail.length) + tail
                if (key(b, ext) !in taken) return pack(b, ext)
            }
            throw FatException("Too many similar names in this folder.")
        }

        private fun now(): Pair<Int, Int> {
            val c = Calendar.getInstance()
            val date = ((c.get(Calendar.YEAR) - 1980).coerceIn(0, 127) shl 9) or ((c.get(Calendar.MONTH) + 1) shl 5) or c.get(Calendar.DAY_OF_MONTH)
            val time = (c.get(Calendar.HOUR_OF_DAY) shl 11) or (c.get(Calendar.MINUTE) shl 5) or (c.get(Calendar.SECOND) / 2)
            return date to time
        }

        private fun stamp(b: ByteArray, at: Int, date: Int, time: Int) {
            put16(b, at + 14, time)
            put16(b, at + 16, date)
            put16(b, at + 18, date)
            put16(b, at + 22, time)
            put16(b, at + 24, date)
        }
    }
}
