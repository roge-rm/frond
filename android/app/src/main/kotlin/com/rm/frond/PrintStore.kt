package com.rm.frond

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

// The held print job on disk and the core's job behind it. Poll, discard, raw save and drop all run under one
// lock, so a discard never lands between a status and its save, and bytes that arrive during a raw save
// wait and go into the next held job. Core calls go through request(argument), which sends it as the printer command, printer or irprinter.
class PrintStore(
    private val dir: File,
    private val request: (String) -> ControlLink.Reply,
) {
    private val lock = Mutex()
    private val heldFile = File(dir, "held.raw")
    private var counter = 0
    // Counts appends to the held file; a drop can be limited to the version that was printed.
    var version = 0
        private set

    init { dir.mkdirs() }

    // Removes the held file and any job file the core may have written after a timeout.
    fun sweep() {
        heldFile.delete()
        dir.listFiles()?.forEach { if (it.name.startsWith("job-") && it.name.endsWith(".raw")) it.delete() }
    }

    fun heldSize() = if (heldFile.isFile) heldFile.length() else 0L

    fun attach(on: Boolean): String? {
        val reply = request("attach " + if (on) "on" else "off")
        return if (reply.ok) null else reply.message
    }

    sealed class Poll {
        object Idle : Poll()
        class Bytes(val bytes: Long) : Poll()
        // A job was added; size is the held file's new size.
        class Added(val bytes: Long, val size: Long, val version: Int) : Poll()
        class Failed(val message: String) : Poll()
    }

    // onHeld runs inside the lock, so its updates keep the order of the file changes.
    suspend fun poll(onHeld: (Long, Int) -> Unit): Poll = lock.withLock {
        val status = PrintJob.parseStatus(request("status").message)
        if (!status.valid) return Poll.Idle
        // Bytes counts a job still coming in too, so leaving the printer asks first.
        if (!status.received) return Poll.Bytes(status.bytes + status.receiving)
        // A fresh name each time: a save that timed out may still be written by the core later.
        val job = File(dir, "job-${++counter}-${System.nanoTime()}.raw")
        // The core won't follow symlinks and /data/user/0 is one inside an app, so it gets the resolved path.
        val reply = request("save " + File(dir.canonicalFile, job.name).path)
        if (!reply.ok || !job.isFile) {
            return Poll.Failed(PrintJob.parseStatus(reply.message).error.ifEmpty { reply.message })
        }
        try {
            FileOutputStream(heldFile, true).use { out ->
                job.inputStream().use { it.copyTo(out) }
                out.fd.sync()
            }
            job.delete()
        } catch (e: IOException) {
            return Poll.Failed(e.message ?: "Couldn't store the print.")
        }
        val size = heldFile.length()
        version++
        onHeld(size, version)
        Poll.Added(status.bytes, size, version)
    }

    // Clears the core's job and the held file. A failed clear keeps the held file.
    suspend fun discard(onHeld: (Long, Int) -> Unit): String? = lock.withLock {
        val reply = request("clear")
        if (!reply.ok) return reply.message
        heldFile.delete()
        onHeld(0, version)
        null
    }

    // Drops the held file without asking the core, which has already thrown its job away.
    suspend fun dropLocal(onHeld: (Long, Int) -> Unit) = lock.withLock {
        heldFile.delete()
        onHeld(0, version)
    }

    // Runs copy with the held file; the job is dropped only when it returns normally.
    suspend fun saveRaw(onHeld: (Long, Int) -> Unit, copy: (File) -> Unit): String? = lock.withLock {
        if (!heldFile.isFile) return "Nothing to save."
        try {
            copy(heldFile)
        } catch (e: Exception) {
            return e.message ?: "Couldn't save the file."
        }
        heldFile.delete()
        onHeld(0, version)
        null
    }

    // Drops the held job after a print, unless more has arrived since the version that was printed.
    suspend fun dropIfVersion(printed: Int, onHeld: (Long, Int) -> Unit): Boolean = lock.withLock {
        if (version != printed || !heldFile.isFile) return false
        heldFile.delete()
        onHeld(0, version)
        true
    }

    // For onHeld callbacks, which already hold the lock.
    fun heldBytes(): ByteArray = if (heldFile.isFile) heldFile.readBytes() else ByteArray(0)
}
