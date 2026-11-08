package com.rm.frond

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.Calendar
import java.util.zip.CRC32

// The FAT12 or FAT16 file system on a card (the one partition the core and cameras make, or a
// superfloppy, up to 2 GiB), through the storage library's FatImage (src/common/storage/FatImage.h)
// in libfrondcard.so: list folders, copy files in and out, make folders, and copy, move, rename and
// delete inside the card. New data only goes into free clusters, and each change's FAT and folder
// sectors are committed together: on a card file (over()) crash safe through CardFile, on a raw
// image (open()) straight in. A change that fails rolls back. Not thread safe; one editor per card.
class FatImage private constructor(private val card: CardFile, private val owned: Boolean) : AutoCloseable {
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

    private var handle = 0L

    private fun live(): Long = if (handle == 0L) throw IOException("The card is closed.") else handle
    private fun numbers() = CardStorage.fatNumbers(live())

    val clusterBytes: Int get() = numbers()[0].toInt()
    val freeBytes: Long get() = numbers()[1]
    val totalBytes: Long get() = numbers()[2]
    // Bytes written to the card, and whether any change was committed.
    val bytesWritten: Long get() = numbers()[3]
    val committed: Boolean get() = numbers()[4] != 0L
    val fatBits: Int get() = numbers()[5].toInt()

    // Lists a folder, "/" or "/PALM/Launcher". Folders first, then files, by name.
    fun list(path: String): List<Entry> = entries(CardStorage.fatList(live(), path))

    fun find(path: String): Entry? = entries(CardStorage.fatFind(live(), path)).firstOrNull()

    fun read(entry: Entry, out: OutputStream) =
        CardStorage.fatRead(live(), entry.name, entry.directory, entry.size, entry.firstCluster, out)

    // Copies size bytes from input into a new file in the folder at path, dated `modified`
    // (milliseconds since 1970) or now.
    fun add(path: String, name: String, input: InputStream, size: Long, modified: Long = 0L) =
        CardStorage.fatAdd(live(), path, name, input, size, modified)

    fun makeFolder(path: String, name: String, modified: Long = 0L) = CardStorage.fatMakeFolder(live(), path, name, modified)

    // Runs `work`'s adds and folders as one change with one commit; a failure or cancel rolls all
    // of it back.
    fun batch(work: () -> Unit) = CardStorage.fatBatch(live(), Runnable { work() })

    // A file, or a folder with everything in it.
    fun remove(path: String) = CardStorage.fatRemove(live(), path)

    // Into toFolder, under newName when given (a rename when the folder is the same).
    fun move(from: String, toFolder: String, newName: String? = null) = CardStorage.fatMove(live(), from, toFolder, newName)

    // Copies a file or folder into toFolder under newName (or its own name). `bytes` gets the
    // amount copied so far; `cancelled` stops it. A folder can't be copied into itself.
    fun copy(from: String, toFolder: String, newName: String? = null, cancelled: () -> Boolean = { false },
             bytes: (Long) -> Unit = {}) {
        val stop = cancelled
        val told = bytes
        CardStorage.fatCopy(live(), from, toFolder, newName, object : CopySteps {
            override fun cancelled() = stop()
            override fun copied(done: Long) = told(done)
        })
    }

    // Runs `work` (an add, copy or move that makes an entry of the victim's name) and removes
    // `victim` in the same commit. The victim's clusters stay used until then, so a cancel, a
    // failure or a crash leaves the old one; the new data needs room beside it.
    fun replace(victim: String, work: () -> Unit) = CardStorage.fatReplace(live(), victim, Runnable { work() })

    // The bytes in a file, or in every file under a folder.
    fun sizeOf(path: String): Long = CardStorage.fatSizeOf(live(), path)

    // Whether every FAT copy on the card matches the first.
    internal fun fatCopiesMatch(): Boolean = CardStorage.fatCopiesMatch(live())

    // A FAT check, as fsck would do it: FAT copies match, every chain is sound and used once, file
    // sizes fit their chains and no cluster is allocated without an owner. The problems found.
    internal fun check(): List<String> = CardStorage.fatCheck(live()).toList()

    fun sync() = card.sync()

    override fun close() {
        if (handle != 0L) CardStorage.fatClose(handle)
        handle = 0L
        if (owned) card.close()
    }

    companion object {
        // Opens a raw card image.
        fun open(image: File, writable: Boolean): FatImage {
            val raw = CardFile.raw(image, writable)
            return on(raw, true)
        }

        // The file system on an open card file. Closing the FatImage leaves the card file open.
        fun over(card: CardFile): FatImage = on(card, false)

        private fun on(card: CardFile, owned: Boolean): FatImage {
            val fat = FatImage(card, owned)
            try {
                fat.handle = CardStorage.fatOpen(card.handle)
            } catch (e: Exception) {
                if (owned) card.close()
                throw e
            }
            return fat
        }

        // Throws FatException when FAT can't hold the name.
        fun checkName(name: String) = CardStorage.checkName(name)

        fun parts(path: String): List<String> = path.split('/').filter { it.isNotEmpty() }

        // Whether `inner` is `outer` or below it.
        fun within(outer: String, inner: String): Boolean {
            val o = parts(outer).map { it.lowercase() }
            val i = parts(inner).map { it.lowercase() }
            return i.size >= o.size && i.take(o.size) == o
        }

        private fun entries(raw: Array<Any>): List<Entry> {
            val n = raw[0] as LongArray
            return (0 until (raw.size - 1) / 2).map { i ->
                val at = 5 * i
                Entry(raw[1 + 2 * i] as String, raw[2 + 2 * i] as String, n[at] != 0L, n[at + 1], n[at + 2].toInt(),
                    dosMillis(n[at + 3].toInt(), n[at + 4].toInt()))
            }
        }

        private fun dosMillis(date: Int, time: Int): Long {
            if (date == 0) return 0L
            val c = Calendar.getInstance()
            c.clear()
            c.set(1980 + (date shr 9), ((date shr 5) and 15) - 1, date and 31, time shr 11, (time shr 5) and 63, (time and 31) * 2)
            return c.timeInMillis
        }

        // ---- The old SD exchange's raw image journal, for its leftovers ----

        private val JOURNAL_MAGIC = "FRONDFJ1".toByteArray(Charsets.US_ASCII)
        private val JOURNAL_END = "FJEND\u0000\u0000\u0000".toByteArray(Charsets.US_ASCII)
        private const val SECTOR = 512
        private const val JOURNAL_HEADER = 20
        private const val JOURNAL_RECORD = 8 + SECTOR
        private const val JOURNAL_TRAILER = 12
        private const val JOURNAL_MAX_BYTES = 16L shl 20

        fun journalOf(image: File) = File(image.path + ".journal")

        // Applies a complete journal to its image and deletes it; deletes an incomplete or damaged one.
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
            val dir = journal.absoluteFile.parentFile ?: return
            try { FileChannel.open(dir.toPath(), StandardOpenOption.READ).use { it.force(true) } } catch (_: Exception) { }
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
    }
}
