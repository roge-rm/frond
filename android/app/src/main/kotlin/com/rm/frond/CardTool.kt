package com.rm.frond

import java.util.concurrent.atomic.AtomicBoolean

// Receives the card tool's progress in bytes, on the calling thread.
fun interface CardToolProgress {
    fun progress(done: Long, total: Long)
}

// Makes, imports and exports card files (the core's card tool). Each call runs on the caller's
// thread, so call it from a worker. Arguments as for `frond-core --card`: ["make", kind, size,
// output], ["import", image, kind, output] and ["export", cardFile, image], with kind sd, ms or
// mspro. Returns "ok KIND BYTES" or "error CODE MESSAGE"; setting `cancel` stops it and removes
// the partial output. In libfrondcard.so, which has no SDL, so any process can call it.
object CardTool {
    init {
        System.loadLibrary("frondcard")
    }

    @JvmStatic external fun run(args: Array<String>, listener: CardToolProgress?, cancel: AtomicBoolean?): String

    // The raw image of an SD or Memory Stick Pro card file, written into the open regular file `fd`
    // (a picked document) with no temp copy. Same replies as export; a classic stick is "error kind".
    @JvmStatic external fun exportFd(card: String, fd: Int, listener: CardToolProgress?, cancel: AtomicBoolean?): String

    // A raw SD or Memory Stick Pro image read from the open regular file `fd` into the new card file
    // `dest`, with no temp copy. Same replies as import; a classic stick is "error kind".
    @JvmStatic external fun importFd(fd: Int, kind: String, dest: String, listener: CardToolProgress?, cancel: AtomicBoolean?): String
}
