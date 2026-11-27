package com.rm.frond

import android.content.ContentResolver
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.IOException

// Builds the Zaurus internal CF from a Sharp HDD1 archive with the core's builder
// (common/storage/ZaurusCf.h), as the desktop's "Create a CF image" does. In libfrondcard.so.
object ZaurusCf {
    init {
        System.loadLibrary("frondcard")
    }

    const val DEFAULT_GB = 4
    val sizes: List<Int> = (4..32).toList()

    // Bytes on the device once the image is full. 4 is the stock Microdrive, larger sizes are decimal GB.
    fun imageBytes(sizeGb: Int): Long = when (sizeGb) {
        4 -> 7_999_488L * 512
        in sizes -> sizeGb * 1_000_000_000L / 512 * 512
        else -> 0
    }

    fun sizeName(sizeGb: Int) = "$sizeGb GB"

    // Room the build needs up front: the archive's contents with headroom, and the NAND copy.
    // The rest of the image is holes until Qtopia writes to it.
    fun neededNow(archiveBytes: Long?): Long = (archiveBytes ?: 0).coerceAtLeast(0) * 3 + (64L shl 20)

    fun shortOfSpace(free: Long, archiveBytes: Long?): String? =
        if (free >= neededNow(archiveBytes)) null
        else "Not enough free space to create the CF image. It needs about ${neededNow(archiveBytes) / 1_000_000} MB."

    // The warning under the size choice when the card can't fill up on this storage, or null.
    fun fillWarning(free: Long, sizeGb: Int): String? =
        if (free >= imageBytes(sizeGb)) null
        else "Only ${"%.1f".format(java.util.Locale.ROOT, free / 1e9)} GB free here, so the card can't fill up."

    interface Listener {
        fun progress(done: Long, total: Long)
        fun cancelled(): Boolean
    }

    @JvmStatic private external fun build(hdd1Fd: Int, hdd1Size: Long, fontFd: Int, requireFonts: Boolean, sizeGb: Int,
                                          outFd: Int, listener: Listener): String?

    // Builds into `outFd`, an empty file, on this thread. Throws IOException with the core's message,
    // or CancellationException when `cancelled` stops it; the caller deletes the output either way.
    internal fun buildFds(hdd1Fd: Int, hdd1Size: Long, fontFd: Int, sizeGb: Int, outFd: Int,
                          cancelled: () -> Boolean, progress: (Long, Long) -> Unit, requireFonts: Boolean = true) {
        val reply = build(hdd1Fd, hdd1Size, fontFd, requireFonts, sizeGb, outFd, object : Listener {
            override fun progress(done: Long, total: Long) = progress(done, total)
            override fun cancelled() = cancelled()
        })
        when {
            reply == "ok" -> Unit
            reply == "cancelled" -> throw kotlinx.coroutines.CancellationException()
            reply != null && reply.startsWith("error ") -> throw IOException(reply.removePrefix("error "))
            else -> throw IOException("Couldn't create the CF image.")
        }
    }

    // Reads the archive through its document's file descriptor and writes `target` sparse. `font` is a
    // CS0 dump already copied into the machine folder, or null. Every LC Font must end up filled, as on
    // the desktop.
    fun create(resolver: ContentResolver, hdd1: Uri, font: File?, sizeGb: Int, target: File,
               cancelled: () -> Boolean, progress: (Long, Long) -> Unit) {
        if (imageBytes(sizeGb) == 0L) throw IOException("Choose a size from 4 to 32 GB.")
        val archiveBytes = HostFiles.size(resolver, hdd1)?.takeIf { it >= 0 }
        shortOfSpace(requireNotNull(target.parentFile).usableSpace, archiveBytes)?.let { throw IOException(it) }
        if (target.exists() && !target.delete()) throw IOException("Couldn't create the CF image.")
        val input = try {
            resolver.openFileDescriptor(hdd1, "r")
        } catch (e: Exception) {
            null
        } ?: throw IOException("Couldn't read the selected HDD1 archive.")
        input.use {
            val size = input.statSize.takeIf { it > 0 } ?: archiveBytes ?: -1
            val fontFd = font?.let { ParcelFileDescriptor.open(it, ParcelFileDescriptor.MODE_READ_ONLY) }
            fontFd.use {
                ParcelFileDescriptor.open(target, ParcelFileDescriptor.MODE_READ_WRITE or ParcelFileDescriptor.MODE_CREATE or
                    ParcelFileDescriptor.MODE_TRUNCATE).use { out ->
                    buildFds(input.fd, size, fontFd?.fd ?: -1, sizeGb, out.fd, cancelled, progress)
                }
            }
        }
    }
}
