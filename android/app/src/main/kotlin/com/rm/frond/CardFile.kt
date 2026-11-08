package com.rm.frond

import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.nio.file.StandardOpenOption
import java.util.TreeMap
import java.util.TreeSet
import java.util.zip.CRC32

// Where FatImage's bytes live. Offsets are card bytes from 0, whole sectors for metadata.
internal interface FatBlocks : AutoCloseable {
    val length: Long
    fun read(at: Long, b: ByteArray, from: Int, length: Int)
    // New data, only into clusters the FAT on the card calls free.
    fun write(at: Long, b: ByteArray, from: Int, length: Int)
    fun sync()
}

// A Frond card file edited in place: sd.frondsd for SD, ms.frondmsp for a Memory Stick Pro
// (uarm/FrondSdImage.h). A 64-byte header, the card's bytes from file offset 64, then for v2 one
// CRC-32 per 1 MiB chunk. v2 header, little endian: 8 version, 12 sector size, 16 card bytes, 24 header
// CRC (the header with this field zeroed, then the table), 28 generation, 36 table offset (64 + card
// bytes), 44 chunk bytes. v1 has the CRC of all the card's bytes at 24 and no table; its first change
// makes it v2.
//
// Crash safety, per change. 1: before data goes into a chunk, the chunk's number is added to the
// intent file <card>.files-intent and fsynced; the data only goes into clusters the card's FAT calls
// free. 2: the data is fsynced, then the FAT and folder sectors, the changed table entries and the new
// header go into <card>.files-commit (a temp, fsynced, renamed). 3: they're written in place and
// fsynced, then both files go. settle() finishes a cut-off change: a commit file is replayed (the change
// is done); an intent file alone gets its chunks' CRCs and the header rewritten (the change isn't done,
// only free clusters changed). Either way the card loads, at the visit's generation.
//
// One CardFile is one Files… visit: every change it writes, and a settle after a crash, moves the
// generation to the one at open plus 1, so a machine with the card sees it changed. The card file is
// locked (FileChannel lock) while open, so other Frond processes and threads leave it alone.
// Not thread safe.
class CardFile private constructor(val file: File, private val raf: RandomAccessFile, private val lock: FileLock,
                                   val writable: Boolean) : FatBlocks {
    private var version = 0
    var payloadBytes = 0L
        private set
    var generation = 0L
        private set
    private var target = 0L
    // Chunk CRCs as on the card; null for a v1 card until its first change.
    private var table: IntArray? = null
    // Chunks that got data since the last commit, all named in the intent file.
    private val dirty = TreeSet<Int>()
    private var intent: FileOutputStream? = null
    private var started = false

    // Called once, before the first byte this visit writes, e.g. to mark the machine's slot swapped.
    var beforeFirstWrite: (() -> Unit)? = null
    // Bytes written into the card file, the write calls (file offset, length), and journal bytes.
    var cardBytesWritten = 0L
        private set
    internal val writes = mutableListOf<Pair<Long, Int>>()
    var journalBytesWritten = 0L
        private set
    // Tests: called at "writing" (after the first data of a change), "data", "journal", "partly",
    // "applied".
    internal var crashHook: ((String) -> Unit)? = null
    // Tests: after a simulated crash nothing more reaches the disk, as if the process had died.
    internal var dead = false

    override val length get() = payloadBytes

    private fun load() {
        val head = ByteArray(HEADER)
        if (raf.length() < HEADER) throw FatImage.FatException(DAMAGED)
        raf.seek(0)
        raf.readFully(head)
        val h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
        if (!head.copyOf(8).contentEquals(MAGIC) || h.getInt(12) != SECTOR) throw FatImage.FatException(DAMAGED)
        version = h.getInt(8)
        payloadBytes = h.getLong(16)
        if (payloadBytes < SECTOR || payloadBytes % SECTOR != 0L || payloadBytes > MAX_PAYLOAD) throw FatImage.FatException(DAMAGED)
        when (version) {
            1 -> {
                if ((28 until HEADER).any { head[it] != 0.toByte() } || raf.length() != HEADER + payloadBytes) throw FatImage.FatException(DAMAGED)
                generation = 0
                table = null
            }
            2 -> {
                generation = h.getLong(28)
                if (h.getLong(36) != HEADER + payloadBytes || h.getInt(44) != CHUNK || (48 until HEADER).any { head[it] != 0.toByte() } ||
                    raf.length() != HEADER + payloadBytes + 4 * chunks(payloadBytes))
                    throw FatImage.FatException(DAMAGED)
                val t = readTable(raf, payloadBytes)
                if (headerCrc(head, t) != h.getInt(24)) throw FatImage.FatException(DAMAGED)
                table = t
            }
            else -> throw FatImage.FatException(DAMAGED)
        }
        target = generation + 1
    }

    override fun read(at: Long, b: ByteArray, from: Int, length: Int) {
        if (at < 0 || at + length > payloadBytes) throw FatImage.FatException("The card is shorter than its file system says.")
        raf.seek(HEADER + at)
        raf.readFully(b, from, length)
    }

    override fun write(at: Long, b: ByteArray, from: Int, length: Int) {
        if (!writable) throw FatImage.FatException("The card is write protected.")
        if (at < 0 || at + length > payloadBytes) throw FatImage.FatException("The card is shorter than its file system says.")
        if (dead) return
        start()
        val fresh = (chunkOf(at)..chunkOf(at + length - 1)).filter { it !in dirty }
        val first = dirty.isEmpty()
        if (fresh.isNotEmpty()) {
            note(fresh)
            dirty += fresh
        }
        put(HEADER + at, b, from, length)
        if (first && fresh.isNotEmpty()) crashHook?.invoke("writing")
    }

    override fun sync() {
        if (!dead) raf.fd.sync()
    }

    private fun start() {
        if (started) return
        beforeFirstWrite?.invoke()
        started = true
    }

    // Adds chunk numbers to the intent file, making it first with the card's size and the visit's
    // generation. Fsynced before the data is written.
    private fun note(chunks: List<Int>) {
        var out = intent
        val bytes = ByteBuffer.allocate(4 * chunks.size).order(ByteOrder.LITTLE_ENDIAN)
        chunks.forEach { bytes.putInt(it) }
        if (out == null) {
            out = FileOutputStream(intentOf(file))
            val head = ByteBuffer.allocate(INTENT_HEADER).order(ByteOrder.LITTLE_ENDIAN)
            head.put(INTENT_MAGIC).putLong(payloadBytes).putLong(target)
            out.write(head.array())
            out.fd.sync()
            syncDir(file)
            journalBytesWritten += INTENT_HEADER
            intent = out
        }
        out.write(bytes.array())
        out.fd.sync()
        journalBytesWritten += bytes.capacity()
    }

    private fun put(at: Long, b: ByteArray, from: Int, length: Int) {
        raf.seek(at)
        raf.write(b, from, length)
        cardBytesWritten += length
        writes += at to length
    }

    // Commits one change: `sectors` are whole card sectors by card offset (FAT and folders). Also fixes
    // the CRCs of chunks that got data without a commit (a failed change), when `sectors` is empty.
    internal fun commit(sectors: Map<Long, ByteArray>) {
        if (!writable) throw FatImage.FatException("The card is write protected.")
        if (dead) return
        if (sectors.isEmpty() && dirty.isEmpty()) return
        start()
        raf.fd.sync()
        crashHook?.invoke("data")
        if (dead) return
        val old = table
        val touched = TreeSet(dirty)
        for (at in sectors.keys) { touched += chunkOf(at); touched += chunkOf(at + SECTOR - 1) }
        val next = old?.copyOf() ?: IntArray(chunks(payloadBytes).toInt())
        val recompute: Iterable<Int> = if (old == null) next.indices else touched
        for (i in recompute) next[i] = chunkCrc(raf, payloadBytes, i, sectors)
        val records = ArrayList<Pair<Long, ByteArray>>()
        for ((at, sector) in sectors) records += HEADER + at to sector
        val tableAt = HEADER + payloadBytes
        if (old == null) records += tableAt to tableBytes(next, 0, next.size)
        else {
            // Runs of changed entries.
            var i = 0
            while (i < next.size) {
                if (next[i] == old[i]) { i++; continue }
                var j = i
                while (j < next.size && next[j] != old[j]) j++
                records += tableAt + 4L * i to tableBytes(next, i, j)
                i = j
            }
        }
        records += 0L to headerV2(payloadBytes, target, next)
        val finalLength = tableAt + 4 * chunks(payloadBytes)
        val encoded = encodeCommit(records, finalLength)
        val journal = commitOf(file)
        val temp = File(journal.path + ".tmp")
        FileOutputStream(temp).use { it.write(encoded); it.fd.sync() }
        if (!temp.renameTo(journal)) { temp.delete(); throw IOException("Couldn't write the card's journal.") }
        syncDir(file)
        journalBytesWritten += encoded.size
        crashHook?.invoke("journal")
        if (dead) return
        if (raf.length() < finalLength) raf.setLength(finalLength)
        records.forEachIndexed { k, (at, bytes) ->
            if (k == records.size / 2) { crashHook?.invoke("partly"); if (dead) return }
            put(at, bytes, 0, bytes.size)
        }
        raf.fd.sync()
        crashHook?.invoke("applied")
        if (dead) return
        journal.delete()
        intent?.close()
        intent = null
        intentOf(file).delete()
        syncDir(file)
        table = next
        version = 2
        generation = target
        dirty.clear()
    }

    // A change that failed after writing data: the chunk CRCs are fixed now so the card loads.
    internal fun settleData() {
        if (dirty.isNotEmpty() && !dead) commit(emptyMap())
    }

    override fun close() {
        try {
            if (!dead) settleData()
        } finally {
            try { intent?.close() } catch (_: IOException) {}
            try { lock.release() } catch (_: IOException) {}
            raf.close()
        }
    }

    companion object {
        const val HEADER = 64
        const val SECTOR = 512
        const val CHUNK = 1 shl 20
        const val MAX_PAYLOAD = 2048L shl 20
        const val IN_USE = "The card is open somewhere else. Close it there first."
        private const val DAMAGED = "The card file is damaged."
        private val MAGIC = "FRONDSD\u0000".toByteArray(Charsets.US_ASCII)
        private val INTENT_MAGIC = "FRONDFI1".toByteArray(Charsets.US_ASCII)
        private val COMMIT_MAGIC = "FRONDFC1".toByteArray(Charsets.US_ASCII)
        private val COMMIT_END = "FCEND\u0000\u0000\u0000".toByteArray(Charsets.US_ASCII)
        private const val INTENT_HEADER = 24
        private const val COMMIT_MAX_BYTES = 64L shl 20

        // The files a cut-off change leaves beside the card. Not core journal names (those start
        // <card>.journal-), so the core and the frontends' journal checks don't take them for a save.
        fun intentOf(card: File) = File(card.path + ".files-intent")
        fun commitOf(card: File) = File(card.path + ".files-commit")

        fun chunks(payload: Long) = (payload + CHUNK - 1) / CHUNK
        private fun chunkOf(at: Long) = (at / CHUNK).toInt()

        // Opens a card file for a visit, settling a cut-off change first. Throws IN_USE when another
        // CardFile (this process or another) has it.
        fun open(card: File, writable: Boolean): CardFile {
            val raf = RandomAccessFile(card, "rw")
            try {
                val lock = tryLock(raf) ?: throw IOException(IN_USE)
                try {
                    settleLocked(card, raf)
                    return CardFile(card, raf, lock, writable).also { it.load() }
                } catch (e: Exception) {
                    try { lock.release() } catch (_: IOException) {}
                    throw e
                }
            } catch (e: Exception) {
                raf.close()
                throw e
            }
        }

        private fun tryLock(raf: RandomAccessFile): FileLock? = try {
            raf.channel.tryLock()
        } catch (e: OverlappingFileLockException) {
            null
        }

        // Whether a card file is open in a CardFile now.
        fun inUse(card: File): Boolean {
            if (!card.isFile) return false
            return try {
                RandomAccessFile(card, "rw").use { raf -> tryLock(raf)?.also { it.release() } == null }
            } catch (e: IOException) {
                false
            }
        }

        // Whether a cut-off change is waiting beside the card file.
        fun unsettled(card: File) = intentOf(card).exists() || commitOf(card).exists()

        // Finishes a cut-off change, if there is one. False when the card is open in a CardFile now,
        // or can't be written; true when there was nothing to do or it's done.
        fun settle(card: File): Boolean {
            if (!card.isFile || !unsettled(card)) return true
            return try {
                RandomAccessFile(card, "rw").use { raf ->
                    val lock = tryLock(raf) ?: return false
                    try { settleLocked(card, raf) } finally { try { lock.release() } catch (_: IOException) {} }
                }
                true
            } catch (e: IOException) {
                false
            }
        }

        // Settling is safe to repeat: replaying a commit writes the same bytes, and the intent step
        // only recomputes CRCs from what is on the card.
        private fun settleLocked(card: File, raf: RandomAccessFile) {
            val commit = commitOf(card)
            File(commit.path + ".tmp").delete()
            if (commit.exists()) {
                val decoded = if (commit.length() <= COMMIT_MAX_BYTES) decodeCommit(commit.readBytes()) else null
                if (decoded != null) {
                    if (raf.length() < decoded.first) raf.setLength(decoded.first)
                    for ((at, bytes) in decoded.second) { raf.seek(at); raf.write(bytes) }
                    raf.fd.sync()
                }
                commit.delete()
                syncDir(card)
            }
            val intent = intentOf(card)
            if (intent.exists()) {
                val b = intent.readBytes()
                val h = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
                // A short header means nothing was written yet: it is fsynced before any data.
                if (b.size >= INTENT_HEADER && b.copyOf(8).contentEquals(INTENT_MAGIC)) {
                    val payload = h.getLong(8)
                    val target = h.getLong(16)
                    val listed = (0 until (b.size - INTENT_HEADER) / 4).map { h.getInt(INTENT_HEADER + 4 * it) }
                    if (payload in SECTOR..MAX_PAYLOAD && payload % SECTOR == 0L && raf.length() >= HEADER + payload) refresh(raf, payload, target, listed)
                }
                intent.delete()
                syncDir(card)
            }
        }

        // Rewrites the CRCs of the chunks named (all of them for a card that isn't v2 yet) and the
        // header at `target`.
        private fun refresh(raf: RandomAccessFile, payload: Long, target: Long, listed: List<Int>) {
            val head = ByteArray(HEADER).also { raf.seek(0); raf.readFully(it) }
            val h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
            val count = chunks(payload).toInt()
            val v2 = head.copyOf(8).contentEquals(MAGIC) && h.getInt(8) == 2 && h.getLong(16) == payload &&
                raf.length() == HEADER + payload + 4L * count
            val table = if (v2) readTable(raf, payload) else IntArray(count)
            val which = if (v2) listed.filter { it in 0 until count }.toSet() else (0 until count).toSet()
            for (i in which) table[i] = chunkCrc(raf, payload, i, emptyMap())
            raf.setLength(HEADER + payload + 4L * count)
            raf.seek(HEADER + payload)
            raf.write(tableBytes(table, 0, count))
            raf.fd.sync()
            raf.seek(0)
            raf.write(headerV2(payload, maxOf(target, if (v2) h.getLong(28) else 0L), table))
            raf.fd.sync()
        }

        private fun readTable(raf: RandomAccessFile, payload: Long): IntArray {
            val count = chunks(payload).toInt()
            val raw = ByteArray(4 * count)
            raf.seek(HEADER + payload)
            raf.readFully(raw)
            val b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
            return IntArray(count) { b.getInt(4 * it) }
        }

        // CRC-32 of chunk i as it will be, with `over` (card offset to whole sector) laid on top.
        private fun chunkCrc(raf: RandomAccessFile, payload: Long, i: Int, over: Map<Long, ByteArray>): Int {
            val start = i.toLong() * CHUNK
            val n = minOf(CHUNK.toLong(), payload - start).toInt()
            val buf = ByteArray(n)
            raf.seek(HEADER + start)
            raf.readFully(buf)
            val sorted = if (over is TreeMap<Long, ByteArray>) over.subMap(start - SECTOR + 1, start + n) else over.filterKeys { it > start - SECTOR && it < start + n }
            for ((at, sector) in sorted) {
                val from = maxOf(at, start)
                val until = minOf(at + SECTOR, start + n)
                sector.copyInto(buf, (from - start).toInt(), (from - at).toInt(), (until - at).toInt())
            }
            return CRC32().also { it.update(buf) }.value.toInt()
        }

        private fun tableBytes(table: IntArray, from: Int, until: Int): ByteArray {
            val b = ByteBuffer.allocate(4 * (until - from)).order(ByteOrder.LITTLE_ENDIAN)
            for (i in from until until) b.putInt(table[i])
            return b.array()
        }

        internal fun headerCrc(header: ByteArray, table: IntArray): Int {
            val copy = header.copyOf(HEADER)
            for (i in 24 until 28) copy[i] = 0
            return CRC32().also { it.update(copy); it.update(tableBytes(table, 0, table.size)) }.value.toInt()
        }

        internal fun headerV2(payload: Long, generation: Long, table: IntArray): ByteArray {
            val head = ByteArray(HEADER)
            val h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
            MAGIC.copyInto(head)
            h.putInt(8, 2).putInt(12, SECTOR).putLong(16, payload).putLong(28, generation).putLong(36, HEADER + payload).putInt(44, CHUNK)
            h.putInt(24, headerCrc(head, table))
            return head
        }

        // A v2 card file of `payload` (whole sectors) at `generation`, as the card tool makes one.
        internal fun encodeV2(payload: ByteArray, generation: Long): ByteArray {
            val count = chunks(payload.size.toLong()).toInt()
            val table = IntArray(count) { i ->
                val start = i * CHUNK
                CRC32().also { it.update(payload, start, minOf(CHUNK, payload.size - start)) }.value.toInt()
            }
            return headerV2(payload.size.toLong(), generation, table) + payload + tableBytes(table, 0, count)
        }

        // What the core's load checks (uarm/MachineFolder.h): a sound v2 header and every chunk's
        // CRC, or a v1 file's whole CRC. Reads the whole card. Returns the generation, or null.
        fun verify(card: File): Long? = RandomAccessFile(card, "r").use { raf ->
            val head = ByteArray(HEADER)
            if (raf.length() < HEADER) return null
            raf.readFully(head)
            val h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
            if (!head.copyOf(8).contentEquals(MAGIC) || h.getInt(12) != SECTOR) return null
            val payload = h.getLong(16)
            if (payload < SECTOR || payload % SECTOR != 0L || payload > MAX_PAYLOAD) return null
            when (h.getInt(8)) {
                1 -> {
                    if (raf.length() != HEADER + payload) return null
                    val crc = CRC32()
                    val buf = ByteArray(CHUNK)
                    var done = 0L
                    while (done < payload) {
                        val n = minOf(CHUNK.toLong(), payload - done).toInt()
                        raf.readFully(buf, 0, n)
                        crc.update(buf, 0, n)
                        done += n
                    }
                    if (crc.value.toInt() == h.getInt(24)) 0L else null
                }
                2 -> {
                    if (h.getLong(36) != HEADER + payload || h.getInt(44) != CHUNK || (48 until HEADER).any { head[it] != 0.toByte() } ||
                        raf.length() != HEADER + payload + 4 * chunks(payload)) return null
                    val table = readTable(raf, payload)
                    if (headerCrc(head, table) != h.getInt(24)) return null
                    if (table.indices.any { chunkCrc(raf, payload, it, emptyMap()) != table[it] }) return null
                    h.getLong(28)
                }
                else -> null
            }
        }

        // Header: magic, record count (u32), file length after (u64). Records: file offset (u64),
        // length (u32), bytes. Trailer: end magic and the CRC-32 of everything before it. Big endian.
        private fun encodeCommit(records: List<Pair<Long, ByteArray>>, finalLength: Long): ByteArray {
            val out = ByteArrayOutputStream()
            val data = DataOutputStream(out)
            data.write(COMMIT_MAGIC)
            data.writeInt(records.size)
            data.writeLong(finalLength)
            for ((at, bytes) in records) { data.writeLong(at); data.writeInt(bytes.size); data.write(bytes) }
            data.write(COMMIT_END)
            data.flush()
            val crc = CRC32().also { it.update(out.toByteArray()) }
            data.writeInt(crc.value.toInt())
            data.flush()
            return out.toByteArray()
        }

        private fun decodeCommit(b: ByteArray): Pair<Long, List<Pair<Long, ByteArray>>>? {
            if (b.size < 20 + 12 || !b.copyOf(8).contentEquals(COMMIT_MAGIC)) return null
            val end = b.size - 12
            if (!b.copyOfRange(end, end + 8).contentEquals(COMMIT_END)) return null
            val crc = CRC32().also { it.update(b, 0, end + 8) }
            val data = ByteBuffer.wrap(b)
            if (crc.value.toInt() != data.getInt(end + 8)) return null
            val count = data.getInt(8)
            val finalLength = data.getLong(12)
            var at = 20
            val records = ArrayList<Pair<Long, ByteArray>>()
            for (i in 0 until count) {
                if (at + 12 > end) return null
                val offset = data.getLong(at)
                val length = data.getInt(at + 8)
                if (offset < 0 || length < 0 || at + 12 + length > end || offset + length > finalLength) return null
                records += offset to b.copyOfRange(at + 12, at + 12 + length)
                at += 12 + length
            }
            if (at != end) return null
            return finalLength to records
        }

        // Makes a created or deleted file's name durable. Best effort: not every file system allows it.
        private fun syncDir(file: File) {
            val dir = file.absoluteFile.parentFile ?: return
            try { FileChannel.open(dir.toPath(), StandardOpenOption.READ).use { it.force(true) } } catch (_: Exception) { }
        }
    }
}
