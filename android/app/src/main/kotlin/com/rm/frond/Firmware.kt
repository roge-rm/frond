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
    // A Zire 72 NOR has a small card at 0x0 and a big card at 0x20000 to the end, both PalmCard aZ72.
    enum class Header { NONE, E2, E2_WRONG_PLACE, ZIRE72, ZIRE72_BAD_LAYOUT, OTHER }

    data class NorInfo(val device: Device? = null, val hasHeader: Boolean = false, val validLayout: Boolean = true,
                       val variant: FirmwareVariant? = null)

    fun inspect(nor: File, devices: List<Device> = Device.entries): NorInfo = RandomAccessFile(nor, "r").use { file ->
        inspect(file.length(), devices) { at, count ->
            if (at < 0 || at > file.length() - count) null else {
                file.seek(at)
                ByteArray(count).also { file.readFully(it) }
            }
        }
    }

    internal fun inspect(size: Long, devices: List<Device> = Device.entries,
                         read: (Long, Int) -> ByteArray?): NorInfo {
        fun read32(at: Long): Long? {
            if (at < 0 || at > size - 4) return null
            val bytes = read(at, 4) ?: return null
            return bytes.indices.fold(0L) { value, i -> value or ((bytes[i].toLong() and 0xff) shl (8 * i)) }
        }
        fun palmCard(at: Long): Boolean {
            if (at < 0 || at > size - 0x19) return false
            val bytes = read(at + 0x10, 9) ?: return false
            return bytes.contentEquals("PalmCard\u0000".toByteArray(Charsets.US_ASCII))
        }
        fun text(at: Long, expected: String): Boolean {
            if (at < 0 || at > size - 32) return false
            val bytes = read(at, 32) ?: return false
            val wanted = (expected + "\u0000").toByteArray(Charsets.US_ASCII)
            return wanted.size <= bytes.size && bytes.copyOfRange(0, wanted.size).contentEquals(wanted)
        }
        var at = 0L
        while (at + 12 <= size) {
            if (read32(at + 8) == 0xfeedbeefL) {
                val company = read32(at + 0x80) ?: return NorInfo()
                val hal = read32(at + 0x84) ?: return NorInfo()
                val device = devices.firstOrNull {
                    it.detectKind == 0 && (it.companyId == 0L || it.companyId == company) && it.halId == hal
                } ?: return NorInfo(hasHeader = true)
                var valid = device.norLayout == 0 || at == device.headerOffset
                if (device.norLayout == 2) {
                    val big = device.bigCardOffset
                    val length = read32(big + 0x6c)
                    valid = valid && palmCard(at) && palmCard(big) && read32(big + 8) == 0xfeedbeefL &&
                        read32(big + 0x80) == company && read32(big + 0x84) == hal &&
                        length != null && big + length == size
                }
                val variant = device.firmwareVariants.firstOrNull {
                    it.norSize == size && it.headerOffset == at &&
                        it.companyId == company && it.halId == hal &&
                        text(at + 0x10, it.cardName) && text(at + 0x30, it.manufacturer) &&
                        (!it.cardEndsAtEof || read32(at + 0x6c)?.let { length -> at + length == size } == true)
                }
                return NorInfo(device, true, valid, variant)
            }
            at += 32768
        }
        return NorInfo()
    }

    // Compatibility diagnostics for the original devices; detection uses the profile inventory.
    fun header(nor: File): Header {
        val info = inspect(nor)
        return when (info.device) {
            Device.E2 -> if (info.validLayout) Header.E2 else Header.E2_WRONG_PLACE
            Device.ZIRE72 -> if (info.validLayout) Header.ZIRE72 else Header.ZIRE72_BAD_LAYOUT
            else -> if (info.hasHeader) Header.OTHER else Header.NONE
        }
    }

    fun detect(nor: File): Device? {
        val info = try { inspect(nor) } catch (e: IOException) { return null }
        info.device?.let { return it }
        val rom = try { inspectRom68k(nor) } catch (e: IOException) { return null }
        return rom.device ?: Device.entries.firstOrNull { it.detectKind == 1 && nor.length() == it.norSize }
    }

    const val NOT_DETECTED = "Frond can't tell which device this firmware is for. Choose the device."

    // A 68k ROM's big-endian card header, read like common/device_profile5.h's deviceProfileInspect68kRom5:
    // only the header, so frond-68k still checks the ROM properly when it starts.
    data class Rom68kInfo(val device: Device? = null, val hasHeader: Boolean = false)

    // Card flags: 0x0001 RAM only, 0x0020 runs on the 68EZ328. The IIIx's flash part holds 2 MiB from 0x10c00000.
    private const val ROM68K_EZ_BASE = 0x10c00000L
    private const val ROM68K_MAX_SIZE = 2L shl 20
    private const val DANA_BASE = 0x10000000L

    fun inspectRom68k(rom: File, devices: List<Device> = Device.entries): Rom68kInfo = RandomAccessFile(rom, "r").use { file ->
        val size = file.length()
        if (size < 0x100) return@use Rom68kInfo()
        val h = ByteArray(0x100)
        file.readFully(h)
        fun be16(at: Int) = ((h[at].toInt() and 0xff) shl 8) or (h[at + 1].toInt() and 0xff)
        fun be32(at: Int) = (be16(at).toLong() shl 16) or be16(at + 2).toLong()
        if (!h.copyOfRange(0x10, 0x19).contentEquals("PalmCard\u0000".toByteArray(Charsets.US_ASCII))) return@use Rom68kInfo()
        if (be32(8) != 0xfeedbeefL) return@use Rom68kInfo()
        val version = be16(0x0c)
        val flags = be16(0x0e)
        val reset = be32(4)
        val cardBytes = be32(0x6c)
        val company = if (version >= 5) be32(0x7e) else 0L
        val hal = if (version >= 5) be32(0x82) else 0L
        val header = Rom68kInfo(hasHeader = true)
        val dana = devices.firstOrNull { it.id == "alphasmart-dana" }
        if (dana != null && version == 5 && company == dana.companyId && hal == dana.halId) {
            // A VZ small card with a second card at 32 KiB and padding after it; flag 0x0040 is the VZ.
            if (flags and 0x0040 == 0 || flags and 0x0001 != 0 || size != dana.norSize ||
                cardBytes < 0x100 || cardBytes > 0x8000 ||
                reset < DANA_BASE || reset - DANA_BASE >= cardBytes) return@use header
            val big = ByteArray(0x100)
            file.seek(0x8000)
            file.readFully(big)
            fun bword(at: Int) = (0 until 4).fold(0L) { v, i -> (v shl 8) or (big[at + i].toLong() and 0xff) }
            val length = bword(0x6c)
            val bigReset = bword(4)
            if (!big.copyOfRange(0x10, 0x19).contentEquals("PalmCard\u0000".toByteArray(Charsets.US_ASCII)) ||
                bword(8) != 0xfeedbeefL || big[0x0c].toInt() != 0 || big[0x0d].toInt() != 5 ||
                big[0x0f].toInt() and 0x40 == 0 || big[0x0f].toInt() and 1 != 0 ||
                bword(0x7e) != company || bword(0x82) != hal ||
                length < 0x100 || length > size - 0x8000 ||
                bigReset < DANA_BASE + 0x8000 || bigReset - DANA_BASE - 0x8000 >= length) return@use header
            return@use Rom68kInfo(dana, true)
        }
        if (version !in 3..5 || flags and 0x0001 != 0 || flags and 0x0020 == 0) return@use header
        if (size > ROM68K_MAX_SIZE || size % 0x1000 != 0L || cardBytes > size) return@use header
        if (reset < ROM68K_EZ_BASE || reset - ROM68K_EZ_BASE >= size) return@use header
        val device = devices.firstOrNull { it.detectKind == 3 && (version < 5 || (it.companyId == company && it.halId == hal)) }
        Rom68kInfo(device, true)
    }

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
        if (device.runs68k) return checkRom68k(device, nor, nand)
        if (!nor.canRead()) return "Couldn't read the NOR image."
        val rom = try { inspectRom68k(nor) } catch (e: IOException) { return "Couldn't read the NOR image." }
        if (rom.device != null) return "This is a ${rom.device.label} ROM. Choose ${rom.device.label}."
        val header = try { header(nor) } catch (e: IOException) { return "Couldn't read the NOR image." }
        val info = try { inspect(nor, Device.entries + device) } catch (e: IOException) { return "Couldn't read the NOR image." }
        if (info.device != null && info.device != device && info.device != Device.E2 && info.device != Device.ZIRE72)
            return "This is a ${info.device.label} NOR image. Choose ${info.device.label}."
        val norSize = nor.length()
        val nandSize = nand?.length()
        when (device) {
            Device.TX -> {
                if (header == Header.E2 || header == Header.E2_WRONG_PLACE)
                    return "This is a Tungsten E2 NOR image. Choose Tungsten E2."
                if (header == Header.ZIRE72 || header == Header.ZIRE72_BAD_LAYOUT)
                    return "This is a Zire 72 NOR image. Choose Zire 72."
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
                    Header.ZIRE72, Header.ZIRE72_BAD_LAYOUT -> return "This is a Zire 72 NOR image. Choose Zire 72."
                    Header.OTHER -> return if (info.device != null) "This is a ${info.device.label} NOR image. Choose ${info.device.label}."
                        else "This NOR image isn't for a Tungsten E2."
                    Header.NONE -> return if (norSize == Device.TX.norSize) "This looks like a Palm TX NOR image. Choose a Tungsten E2 one."
                        else "This NOR image has no Tungsten E2 header."
                }
                if (norSize != (info.variant?.norSize ?: device.norSize)) return "Choose an 8 MiB NOR image or a supported de-NVFS image for the Tungsten E2."
                if (nandSize != null && nandSize != device.nandSize)
                    return "The Tungsten E2 NAND image must be a raw ${mib(device.nandSize)} file. This one is ${mib(nandSize)}."
            }
            Device.ZIRE72 -> {
                if (norSize != device.norSize) return "Choose a 14,942,208-byte NOR image for the Zire 72."
                when (header) {
                    Header.ZIRE72 -> {}
                    Header.ZIRE72_BAD_LAYOUT -> return "This Zire 72 NOR image doesn't have a small card at 0x0 and a big card at 0x20000."
                    Header.NONE -> return "This NOR image has no Zire 72 header. Check the device choice."
                    else -> return "This NOR image is not for a Zire 72."
                }
                if (nand != null) return "The Zire 72 has no NAND. Leave the NAND image empty."
            }
            else -> {
                if (info.device == Device.E2) return "This is a Tungsten E2 NOR image. Choose Tungsten E2."
                if (norSize != device.norSize) return "The ${device.label} NOR image must be ${mib(device.norSize)}."
                if (device.detectKind == 0 && info.device != device) return "This NOR image has no ${device.label} header."
                if (!info.validLayout) return "This ${device.label} NOR image has an invalid card layout."
                if (!device.hasNand && nand != null) return "This device has no NAND. Leave the NAND image empty."
                if (device.nandRequired && nand == null) return "Choose a NAND image for the ${device.label}."
                if (nand != null && nandSize != device.nandSize) return "The ${device.label} NAND image must be a raw ${mib(device.nandSize)} file."
            }
        }
        return null
    }

    // A 68k device takes one ROM file and no NAND, as in desktop/FirmwareInput.h.
    private fun checkRom68k(device: Device, rom: File, nand: File?): String? {
        if (!rom.canRead()) return "Couldn't read the ROM image."
        val info = try { inspectRom68k(rom, Device.entries + device) } catch (e: IOException) { return "Couldn't read the ROM image." }
        if (!info.hasHeader) return "This file isn't a ${device.label} ROM."
        if (info.device == null) return "This ROM isn't one the ${device.label} can run."
        // The Dana Wireless runs the Dana's ROM, whose header still says Dana.
        val danaRom = device.id == "alphasmart-dana-wireless" && info.device?.id == "alphasmart-dana"
        if (info.device != device && !danaRom) return "This is a ${info.device.label} ROM. Choose ${info.device.label}."
        if (nand != null) return "The ${device.label} has no NAND. Leave the NAND image empty."
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

    // Resolve once at machine start. Saved machines use their embedded NOR, including older named sessions.
    fun runningDevice(device: Device, session: File?, nor: File): Device {
        if (device.firmwareVariants.none { !it.powerSupported }) return device
        val info = try {
            if (session != null && (session.isFile || session.isDirectory)) Sessions.firmwareInfo(session, device)
            else if (nor.length() in (device.firmwareVariants.map { it.norSize } + device.norSize))
                inspect(nor, listOf(device)).takeIf { it.validLayout && (nor.length() == device.norSize || it.variant != null) }
            else null
        } catch (e: Exception) { null }
        return if (info?.device?.id == device.id) device.withFirmware(info.variant) else device.unresolvedFirmware()
    }

    // Whether the core accepts low-latency audio: the device has a qualified NOR and the one it runs
    // (inside a saved session, else the installed one) matches. Reads the NOR, so not on the UI thread.
    fun fastAudioQualified(device: Device, session: File?, nor: File): Boolean {
        if (device.id == "sony-clie-tg50") return false
        val fingerprints = listOfNotNull(device.fastAudioNorMd5) + device.firmwareVariants.mapNotNull { it.fastAudioNorMd5 }
        if (fingerprints.isEmpty()) return false
        val md5 = if (session != null && (session.isFile || session.isDirectory)) Sessions.norMd5(session, (device.firmwareVariants.map { it.norSize } + device.norSize).toSet()) else md5(nor)
        return md5 != null && md5 in fingerprints
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
