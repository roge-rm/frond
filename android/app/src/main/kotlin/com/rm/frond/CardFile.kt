package com.rm.frond

import java.io.File
import java.io.IOException

// A Frond card file edited in place for one Files… visit: sd.frondsd for SD, ms.frondmsp for a
// Memory Stick Pro. The work is the storage library's (src/common/storage/CardFile.h), the same
// code the desktop and the core use, in libfrondcard.so.
//
// Crash safety, per change: before data goes into a 1 MiB chunk, its number goes into
// <card>.journal-N.pending, and data only goes into clusters the card's FAT calls free; then the
// FAT and folder sectors and the new chunk table go into a standalone journal <card>.journal-N,
// are written in place with the header, and both files go. settle() finishes a cut-off change.
// Either way the card loads at N, the generation at open plus 1, so a machine with the card sees
// it changed once per visit.
//
// The card file holds an open file description lock while open, so other Frond processes and
// threads, and the core in this process, leave it alone. Not thread safe.
class CardFile private constructor(val file: File, val writable: Boolean) : AutoCloseable {
    internal var handle = 0L
        private set
    private var closed = false

    // Called once, before the first byte this visit writes, e.g. to mark the machine's slot swapped.
    // An exception from it stops the change.
    var beforeFirstWrite: (() -> Unit)? = null
        set(value) { field = value; hooks() }
    // Tests: called at "writing" (after the first data of a change), "data", "journal", "partly",
    // "applied".
    internal var crashHook: ((String) -> Unit)? = null
        set(value) { field = value; hooks() }
    // Tests: after a simulated crash nothing more reaches the disk, as if the process had died.
    internal var dead = false
        set(value) { field = value; if (value) CardStorage.kill(live()) }

    private fun live(): Long = if (closed) throw IOException("The card file is closed.") else handle
    private fun hooks() = CardStorage.hooks(live(), beforeFirstWrite != null, crashHook != null)
    private fun numbers() = CardStorage.numbers(live())

    // Called from native code.
    fun firstWrite() { beforeFirstWrite?.invoke() }
    fun crashAt(step: String) { crashHook?.invoke(step) }

    val payloadBytes: Long get() = numbers()[0]
    val generation: Long get() = numbers()[1]
    // Bytes written into the card file, the write calls (file offset, length), and journal bytes.
    val cardBytesWritten: Long get() = numbers()[2]
    val journalBytesWritten: Long get() = numbers()[3]
    internal val writes: List<Pair<Long, Int>>
        get() = CardStorage.writes(live()).let { w -> (w.indices step 2).map { w[it] to w[it + 1].toInt() } }

    // A fresh FAT file system over the whole card, as the library formats one for its size.
    fun format() = CardStorage.format(live(), null)

    internal fun sync() = CardStorage.sync(live())

    // Fixes the CRCs of data a failed change left, then unlocks the card file.
    override fun close() {
        if (closed) return
        closed = true
        CardStorage.close(handle)
    }

    companion object {
        const val IN_USE = "The card is open somewhere else. Close it there first."

        // Opens a card file for a visit, settling a cut-off change first. Throws IN_USE when another
        // CardFile (this process or another) has it.
        fun open(card: File, writable: Boolean): CardFile {
            val c = CardFile(card, writable)
            c.handle = CardStorage.openCard(card.path, writable, c)
            return c
        }

        // Whether a card file is open in a CardFile now (this process or another).
        fun inUse(card: File): Boolean = CardStorage.inUse(card.path)

        // Whether a cut-off change is waiting beside the card file.
        fun unsettled(card: File): Boolean = CardStorage.unsettled(card.path)

        // Finishes a cut-off change, if there is one. False when the card is open in a CardFile now,
        // or can't be settled; true when there was nothing to do or it's done.
        fun settle(card: File): Boolean = CardStorage.failure(CardStorage.settle(card.path)) == null

        // Null when nobody has the card open in Files and anything a crash cut off is finished;
        // otherwise the message to show.
        fun notReady(card: File): String? {
            val (code, message) = CardStorage.failure(CardStorage.readiness(card.path)) ?: return null
            return if (code == "busy") Cards.IN_FILES else message
        }

        // What a card load checks: the header and every CRC. Reads the whole card. The generation
        // (0 for v1), or null.
        fun verify(card: File): Long? = CardStorage.verify(card.path).takeIf { it >= 0 }

        // The first 64 bytes of a card file, or null.
        fun header(card: File): ByteArray? = CardStorage.header(card.path)

        // A v2 card file of `payload` (whole sectors) at `generation`, as the card tool makes one.
        internal fun encodeV2(payload: ByteArray, generation: Long): ByteArray =
            CardStorage.encodeV2(payload, generation) ?: throw IOException("Not a card's size.")

        // A raw card image for FatImage.open: writes go straight in.
        internal fun raw(image: File, writable: Boolean): CardFile {
            val c = CardFile(image, writable)
            c.handle = CardStorage.openRaw(image.path, writable)
            return c
        }
    }
}
