package com.rm.frond

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

// Firmware images in app storage, one folder per device: files/firmware/<device id>/.
object Firmware {
    fun folder(context: Context, device: Device) = File(File(context.filesDir, "firmware"), device.id)
    fun nor(context: Context, device: Device) = File(folder(context, device), "nor.bin")
    fun nand(context: Context, device: Device) = File(folder(context, device), "nand.bin")

    fun ready(context: Context, device: Device): Boolean {
        val nor = nor(context, device)
        val nand = nand(context, device)
        return nor.isFile && (nand.isFile || !device.nandRequired) && check(device, nor, nand.takeIf { it.isFile }) == null
    }

    // What the NOR's Palm card header says, found the way the core looks for it: every 32 KiB.
    enum class Header { NONE, E2, E2_WRONG_PLACE, OTHER }

    private const val E2_HEADER_OFFSET = 0x20000L

    fun header(nor: File): Header {
        val palm = 0x50616c6dL
        val hspr = 0x68737072L
        val signature = 0xfeedbeefL
        RandomAccessFile(nor, "r").use { file ->
            fun read32(at: Long): Long? {
                if (at + 4 > file.length()) return null
                file.seek(at)
                val b = ByteArray(4)
                file.readFully(b)
                return (b[0].toLong() and 0xff) or ((b[1].toLong() and 0xff) shl 8) or
                    ((b[2].toLong() and 0xff) shl 16) or ((b[3].toLong() and 0xff) shl 24)
            }
            var at = 0L
            while (at + 12 <= file.length()) {
                if (read32(at + 8) == signature) {
                    val company = read32(at + 0x80) ?: return Header.NONE
                    val hal = read32(at + 0x84) ?: return Header.NONE
                    if (company == palm && hal == hspr) return if (at == E2_HEADER_OFFSET) Header.E2 else Header.E2_WRONG_PLACE
                    return Header.OTHER
                }
                at += 32 * 1024
            }
        }
        return Header.NONE
    }

    // Picks the device from the NOR like desktop/FirmwareInput.h detectDevice: an E2 card header,
    // else the Palm TX size. Null when it can't tell.
    fun detect(nor: File): Device? {
        val header = try { header(nor) } catch (e: IOException) { return null }
        if (header == Header.E2 || header == Header.E2_WRONG_PLACE) return Device.E2
        if (nor.length() == Device.TX.norSize) return Device.TX
        return null
    }

    const val NOT_DETECTED = "This isn't a Palm TX or Tungsten E2 NOR image."

    // A picked NOR is copied here first, so the device can be found before the import.
    fun picked(context: Context) = File(File(context.filesDir, "firmware"), "picked-nor.bin")

    // Bigger than any NOR the app takes; anything past it is refused while copying.
    private const val PICK_LIMIT = 32L shl 20

    // Copies a picked NOR into picked(). Returns an error message or null. Runs off the main thread.
    fun pick(context: Context, uri: Uri): String? {
        val to = picked(context)
        to.parentFile?.mkdirs()
        try {
            val input = context.contentResolver.openInputStream(uri) ?: throw IOException("no stream")
            input.use { i ->
                to.outputStream().use { o ->
                    val buffer = ByteArray(1 shl 20)
                    var total = 0L
                    while (true) {
                        val n = i.read(buffer)
                        if (n < 0) break
                        total += n
                        if (total > PICK_LIMIT) {
                            to.delete()
                            return NOT_DETECTED
                        }
                        o.write(buffer, 0, n)
                    }
                }
            }
            return null
        } catch (e: Exception) {
            to.delete()
            return "Couldn't read the NOR image."
        }
    }

    private fun mib(bytes: Long) = if (bytes % (1L shl 20) == 0L) "${bytes shr 20} MiB" else "%,d bytes".format(bytes)

    // The same checks as desktop/FirmwareInput.h: the header names this device and the sizes fit.
    // Returns a message to show, or null when the files are fine. nand is null when none was chosen.
    fun check(device: Device, nor: File, nand: File?): String? {
        if (!nor.canRead()) return "Couldn't read the NOR image."
        val header = try { header(nor) } catch (e: IOException) { return "Couldn't read the NOR image." }
        val norSize = nor.length()
        val nandSize = nand?.length()
        when (device) {
            Device.TX -> {
                if (header == Header.E2 || header == Header.E2_WRONG_PLACE)
                    return "This is a Tungsten E2 NOR image. Choose a Palm TX one."
                if (nand == null) return "Choose a NAND image for the Palm TX."
                if (norSize == device.nandSize && nandSize == device.norSize)
                    return "The NOR and NAND images are the wrong way round."
                if (norSize != device.norSize) return "The Palm TX NOR image must be ${mib(device.norSize)}. This one is ${mib(norSize)}."
                if (nandSize != device.nandSize) return "The Palm TX NAND image must be a raw ${mib(device.nandSize)} file. This one is ${mib(nandSize!!)}."
            }
            Device.E2 -> {
                when (header) {
                    Header.E2 -> {}
                    Header.E2_WRONG_PLACE -> return "This E2 NOR image doesn't have its header at 0x20000."
                    Header.OTHER -> return "This NOR image isn't for a Tungsten E2."
                    Header.NONE -> return if (norSize == Device.TX.norSize) "This looks like a Palm TX NOR image. Choose a Tungsten E2 one."
                        else "This NOR image has no Tungsten E2 header."
                }
                if (norSize != device.norSize) return "The Tungsten E2 NOR image must be ${mib(device.norSize)}. This one is ${mib(norSize)}."
                if (nandSize != null && nandSize != device.nandSize)
                    return "The Tungsten E2 NAND image must be a raw ${mib(device.nandSize)} file. This one is ${mib(nandSize)}."
            }
        }
        return null
    }

    // Puts back a firmware file left as *.old by an import that was stopped between its renames.
    // All or nothing per device: when any file with an .old is missing, the import was stopped, so the
    // new files go and every .old comes back. Otherwise the import finished and the .old files go.
    fun recover(context: Context) = synchronized(lock) {
        for (device in Device.entries) recover(listOf(nor(context, device), nand(context, device)))
    }

    internal fun recover(targets: List<File>) {
        val olds = targets.map { File(it.path + ".old") }.filter { it.isFile }
        if (olds.isEmpty()) return
        if (targets.all { t -> !File(t.path + ".old").isFile || t.exists() }) {
            olds.forEach { it.delete() }
            return
        }
        // A target with no .old yet is still the old file (the stop came during the first renames).
        olds.forEach { File(it.path.removeSuffix(".old")).delete() }
        olds.forEach { it.renameTo(File(it.path.removeSuffix(".old"))) }
    }

    // Held by import and recover so one can't run in the middle of the other.
    private val lock = Any()

    // Puts the picked NOR (see pick) and NAND into app storage. Checks them first and keeps the old
    // ones on any error. Returns an error message, or null when the new firmware is in place. Runs off
    // the main thread.
    fun import(context: Context, device: Device, nandUri: Uri?): String? = synchronized(lock) {
        return importLocked(context, device, picked(context), nandUri)
    }

    private fun importLocked(context: Context, device: Device, pickedNor: File, nandUri: Uri?): String? {
        val folder = folder(context, device)
        folder.mkdirs()
        recover(listOf(nor(context, device), nand(context, device)))
        val newNor = File(folder, "nor.bin.new")
        val newNand = File(folder, "nand.bin.new")
        try {
            if (!pickedNor.isFile) return "Couldn't read the NOR image."
            // A copy, so the pick stays for another try after an error.
            if (!copyFile(pickedNor, newNor)) return "Couldn't read the NOR image."
            if (nandUri != null && !copy(context, nandUri, newNand)) return "Couldn't read the NAND image."
            check(device, newNor, if (nandUri != null) newNand else null)?.let { return it }
            // The old pair moves to *.old first, so a failed rename puts it back and never leaves
            // a NOR from one pair with the NAND from the other.
            val pairs = listOfNotNull(newNor to nor(context, device), if (nandUri != null) newNand to nand(context, device) else null)
            // A device whose NAND is optional loses the old one when the new firmware has none.
            val dropped = if (nandUri == null && !device.nandRequired) nand(context, device).takeIf { it.exists() } else null
            val moved = mutableListOf<File>()
            val placed = mutableListOf<File>()
            fun undo(): String {
                placed.forEach { it.delete() }
                moved.forEach { File(it.path + ".old").renameTo(it) }
                return "Couldn't save the firmware."
            }
            for (target in pairs.map { it.second } + listOfNotNull(dropped)) {
                val old = File(target.path + ".old")
                old.delete()
                if (target.exists()) {
                    if (!target.renameTo(old)) return undo()
                    moved += target
                }
            }
            for ((from, target) in pairs) {
                if (!from.renameTo(target)) return undo()
                placed += target
            }
            (pairs.map { it.second } + listOfNotNull(dropped)).forEach { File(it.path + ".old").delete() }
            pickedNor.delete()
            return null
        } finally {
            newNor.delete()
            newNand.delete()
        }
    }

    private fun copyFile(from: File, to: File): Boolean = try {
        from.copyTo(to, overwrite = true)
        true
    } catch (e: IOException) {
        to.delete()
        false
    }

    fun md5(file: File): String? = try {
        val digest = java.security.MessageDigest.getInstance("MD5")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 20)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                digest.update(buffer, 0, n)
            }
        }
        digest.digest().joinToString("") { "%02x".format(it) }
    } catch (e: IOException) {
        null
    }

    fun copy(context: Context, uri: Uri, to: File): Boolean = try {
        val input = context.contentResolver.openInputStream(uri) ?: throw IOException("no stream")
        input.use { i -> to.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
        true
    } catch (e: Exception) {
        to.delete()
        false
    }
}
