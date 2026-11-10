package com.rm.frond

import java.io.InputStream
import java.io.OutputStream

// Told how a copy on the card goes, from native code: asked whether to stop, told the bytes so far.
interface CopySteps {
    fun cancelled(): Boolean
    fun copied(done: Long)
}

// The storage library (src/common/storage) in libfrondcard.so, for CardFile and FatImage. Handles
// are native objects: a card is from openCard or openRaw and goes with close, a FAT is from fatOpen
// and goes with fatClose before its card. Errors throw FatImage.FatException with the library's
// message, a cancel FatImage.Cancelled, and an exception a stream or callback throws comes back as
// it is. Unit tests load a host build of the same code.
internal object CardStorage {
    init {
        System.loadLibrary("frondcard")
    }

    // Card files and raw images.
    @JvmStatic external fun openCard(path: String, writable: Boolean, owner: CardFile): Long
    @JvmStatic external fun openRaw(path: String, writable: Boolean): Long
    @JvmStatic external fun close(card: Long)
    @JvmStatic external fun hooks(card: Long, firstWrite: Boolean, crash: Boolean)
    @JvmStatic external fun kill(card: Long)
    // Length, generation, card bytes written, journal bytes written.
    @JvmStatic external fun numbers(card: Long): LongArray
    // File offset and length of each write.
    @JvmStatic external fun writes(card: Long): LongArray
    @JvmStatic external fun sync(card: Long)
    @JvmStatic external fun format(card: Long, label: String?)

    @JvmStatic external fun inUse(path: String): Boolean
    @JvmStatic external fun unsettled(path: String): Boolean
    // "ok" or "error CODE MESSAGE".
    @JvmStatic external fun settle(path: String): String
    @JvmStatic external fun readiness(path: String): String
    @JvmStatic external fun discard(path: String): String
    // The generation, or -1.
    @JvmStatic external fun verify(path: String): Long
    @JvmStatic external fun header(path: String): ByteArray?
    // Version, payload bytes, side bytes, generation and kind (0 SD or Pro, 1 classic stick, 2 NAND)
    // of a container, or null. With `checked`, null unless its size and every CRC check out.
    @JvmStatic external fun cardInfo(path: String, checked: Boolean): LongArray?
    // The card with a save's journal applied, written to `out`; neither file changes.
    @JvmStatic external fun settledCopy(card: String, journal: String, out: OutputStream)
    @JvmStatic external fun encodeV2(payload: ByteArray, generation: Long): ByteArray?

    // The FAT on an open card.
    @JvmStatic external fun fatOpen(card: Long): Long
    @JvmStatic external fun fatClose(fat: Long)
    // Cluster bytes, free bytes, total bytes, bytes written, committed (0 or 1), FAT bits.
    @JvmStatic external fun fatNumbers(fat: Long): LongArray
    // [numbers, name, short name, name, short name...], numbers being directory, size, first
    // cluster, DOS date and DOS time for each entry.
    @JvmStatic external fun fatList(fat: Long, path: String): Array<Any>
    @JvmStatic external fun fatFind(fat: Long, path: String): Array<Any>
    @JvmStatic external fun fatRead(fat: Long, name: String, directory: Boolean, size: Long, firstCluster: Int, out: OutputStream)
    // `modified` in milliseconds since 1970, 0 for now.
    @JvmStatic external fun fatAdd(fat: Long, path: String, name: String, input: InputStream, size: Long, modified: Long)
    @JvmStatic external fun fatMakeFolder(fat: Long, path: String, name: String, modified: Long)
    @JvmStatic external fun fatBatch(fat: Long, work: Runnable)
    @JvmStatic external fun fatRemove(fat: Long, path: String)
    @JvmStatic external fun fatMove(fat: Long, from: String, toFolder: String, newName: String?)
    @JvmStatic external fun fatCopy(fat: Long, from: String, toFolder: String, newName: String?, steps: CopySteps)
    @JvmStatic external fun fatReplace(fat: Long, victim: String, work: Runnable)
    @JvmStatic external fun fatSizeOf(fat: Long, path: String): Long
    @JvmStatic external fun fatCopiesMatch(fat: Long): Boolean
    @JvmStatic external fun fatCheck(fat: Long): Array<String>
    @JvmStatic external fun checkName(name: String)

    // The message of an "error CODE MESSAGE" reply, or null for "ok".
    fun failure(reply: String): Pair<String, String>? {
        if (reply == "ok") return null
        val words = reply.split(' ', limit = 3)
        return words.getOrElse(1) { "error" } to words.getOrElse(2) { reply }
    }
}
