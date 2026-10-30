package com.rm.frond

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.util.Calendar

// Reads and edits a FAT16 SD card image in place: the card the core makes (one primary FAT16
// partition) or a FAT16 superfloppy, 32-128 MiB. It does what the desktop's SD exchange does with
// mtools: list folders, copy files in and out, make folders. Nothing is ever overwritten or deleted.
// Not thread safe; one editor per image file.
class FatImage private constructor(private val file: RandomAccessFile) : AutoCloseable {
    class FatException(message: String) : IOException(message)

    data class Entry(
        val name: String,
        val shortName: String,
        val directory: Boolean,
        val size: Long,
        val firstCluster: Int,
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
    private var fatDirty = false

    val clusterBytes get() = clusterSectors * SECTOR

    val freeBytes: Long get() = (2 until clusterCount + 2).count { fat[it] == 0 }.toLong() * clusterBytes
    val totalBytes: Long get() = clusterCount.toLong() * clusterBytes

    private fun open() {
        val size = file.length()
        if (size < MIN_BYTES || size > MAX_BYTES || size % SECTOR != 0L) throw FatException("Use a 32-128 MiB FAT16 card image.")
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
                list.forEachIndexed { i, c -> file.seek(clusterAt(c)); file.readFully(out, i * clusterBytes, clusterBytes) }
                out
            }

        fun writeSlots(bytes: ByteArray, first: Int, count: Int) {
            val list = if (root) emptyList() else clusters()
            for (slot in first until first + count) {
                val at = slot * 32
                val position = if (root) rootStart + at else clusterAt(list[at / clusterBytes]) + at % clusterBytes
                write(position, bytes, at, 32)
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
            val entry = Entry(long ?: short, short, attr and 0x10 != 0, u32(bytes, at + 28), u16(bytes, at + 26))
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
    fun add(path: String, name: String, input: InputStream, size: Long) {
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
        flush()
    }

    fun makeFolder(path: String, name: String) {
        checkName(name)
        val parent = folder(path)
        val existing = slots(parent)
        refuseTaken(existing, name)
        val cluster = allocate(1).first()
        val block = ByteArray(clusterBytes)
        val (date, time) = now()
        fun dot(at: Int, label: String, target: Int) {
            label.toByteArray(Charsets.US_ASCII).copyInto(block, at)
            for (i in label.length until 11) block[at + i] = ' '.code.toByte()
            block[at + 11] = 0x10
            stamp(block, at, date, time)
            put16(block, at + 26, target)
        }
        dot(0, ".", cluster)
        dot(32, "..", parent.cluster)
        write(clusterAt(cluster), block, 0, clusterBytes)
        addEntry(parent, existing, name, 0x10, cluster, 0)
        flush()
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
        fatDirty = true
        return free
    }

    private fun addEntry(folder: Folder, existing: List<Slot>, name: String, attr: Int, cluster: Int, size: Long) {
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

    private fun flush() {
        if (!fatDirty) return
        val raw = ByteArray(fat.size * 2)
        fat.forEachIndexed { i, v -> put16(raw, i * 2, v) }
        for (copy in 0 until fatCount) write(fatStart + copy.toLong() * fatSectors * SECTOR, raw, 0, raw.size)
        fatDirty = false
    }

    private fun read(at: Long, length: Int): ByteArray {
        val b = ByteArray(length)
        file.seek(at)
        file.readFully(b)
        return b
    }

    private fun write(at: Long, b: ByteArray, from: Int, length: Int) {
        file.seek(at)
        file.write(b, from, length)
    }

    override fun close() {
        flush()
        file.close()
    }

    companion object {
        const val SECTOR = 512
        const val MIN_BYTES = 32L shl 20
        const val MAX_BYTES = 128L shl 20
        private val LONG_OFFSETS = intArrayOf(1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
        // Characters FAT refuses in long names, as the desktop's exchange does.
        private val FORBIDDEN = "\"*/:<>?\\|"

        fun open(image: File, writable: Boolean): FatImage {
            val raf = RandomAccessFile(image, if (writable) "rw" else "r")
            try {
                return FatImage(raf).also { it.open() }
            } catch (e: Exception) {
                raf.close()
                throw e
            }
        }

        fun parts(path: String): List<String> = path.split('/').filter { it.isNotEmpty() }

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
