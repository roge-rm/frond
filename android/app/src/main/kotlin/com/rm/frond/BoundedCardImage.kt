package com.rm.frond

import java.io.File
import java.io.InputStream

// Document providers need not report a size. Keep at most limit bytes and remove incomplete copies.
internal object BoundedCardImage {
    fun copy(input: InputStream, destination: File, limit: Long, cancelled: () -> Boolean = { false }): Boolean {
        require(limit >= 0)
        var complete = false
        try {
            destination.outputStream().use { output ->
                val buffer = ByteArray(1 shl 16)
                var total = 0L
                while (true) {
                    if (cancelled()) throw java.io.InterruptedIOException("Card copy cancelled")
                    val remaining = limit - total
                    val wanted = if (remaining < buffer.size) (remaining + 1).toInt() else buffer.size
                    var count = input.read(buffer, 0, wanted)
                    if (count == 0) {
                        val byte = input.read()
                        if (byte < 0) count = -1 else { buffer[0] = byte.toByte(); count = 1 }
                    }
                    if (cancelled()) throw java.io.InterruptedIOException("Card copy cancelled")
                    if (count < 0) break
                    if (count.toLong() > remaining) return false
                    output.write(buffer, 0, count)
                    total += count
                }
            }
            complete = true
            return true
        } finally {
            if (!complete) destination.delete()
        }
    }
}
