package com.rm.frond

import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException

// The file locks this process holds: machines' .run.lock and card copies' .new-*.lock. They are
// POSIX record locks (FileChannel locks), which belong to the process and all go when it closes any
// descriptor of the file. So every such lock is taken here, and nothing opens a file held here: a
// second try in this process gets null without touching the file. Card files open in Files hold the
// storage library's own lock (CardFile), which other descriptors don't drop.
internal object FileLocks {
    class Held internal constructor(private val key: String, val raf: RandomAccessFile, private val lock: FileLock) : Closeable {
        private var closed = false

        override fun close() {
            synchronized(FileLocks) {
                if (closed) return
                closed = true
                try { lock.release() } catch (_: IOException) {}
                try { raf.close() } catch (_: IOException) {}
                held.remove(key)
            }
        }
    }

    private val held = HashMap<String, Held>()

    private fun keyOf(file: File) = try { file.canonicalPath } catch (e: IOException) { file.absolutePath }

    fun isHeld(file: File): Boolean = synchronized(this) { keyOf(file) in held }

    // Opens the file (made if missing) and locks it. Null when this process or another holds it.
    // Throws IOException when it can't be opened.
    fun tryLock(file: File): Held? = synchronized(this) {
        val key = keyOf(file)
        if (key in held) return null
        val raf = RandomAccessFile(file, "rw")
        val lock = try {
            raf.channel.tryLock()
        } catch (e: OverlappingFileLockException) {
            null
        } catch (e: IOException) {
            raf.close()
            throw e
        }
        if (lock == null) {
            // Nothing here held it, so closing this descriptor drops no lock of ours.
            raf.close()
            return null
        }
        Held(key, raf, lock).also { held[key] = it }
    }
}
