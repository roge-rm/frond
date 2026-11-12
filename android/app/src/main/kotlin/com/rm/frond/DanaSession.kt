package com.rm.frond

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

// Inspection only. The core checks media ownership, durable history and machine state on load.
internal object DanaSession {
    data class Header(val offset: Long, val version: Long, val portable: Boolean, val device: String)
    private val magics = listOf("FRONDLINKSD1\n", "FRONDPORTSD1\n")
    fun wrapped(file: File): Boolean = try {
        file.inputStream().use { input ->
            val bytes = ByteArray(13)
            java.io.DataInputStream(input).readFully(bytes)
            String(bytes, Charsets.US_ASCII) in magics
        }
    } catch (_: IOException) { false }

    fun inspect(file: File): Header? = try {
        RandomAccessFile(file, "r").use { input ->
            if (input.length() !in 64..(512L shl 20)) return null
            val magic = ByteArray(13).also { input.readFully(it) }.toString(Charsets.US_ASCII)
            val portable = when (magic) { magics[0] -> false; magics[1] -> true; else -> return null }
            fun u32(): Long = Integer.toUnsignedLong(Integer.reverseBytes(input.readInt()))
            val count = u32()
            val machineHash = u32()
            val manifestHash = u32()
            if (count !in 1..8192 || input.length() < 25 + count + 40) return null
            val manifest = ByteArray(count.toInt()).also { input.readFully(it) }
            fun hash(bytes: ByteArray, start: Long = 2166136261L): Long {
                var result = start
                for (b in bytes) result = ((result xor (b.toLong() and 255)) * 16777619) and 0xffffffffL
                return result
            }
            if (hash(manifest) != manifestHash || !validManifest(manifest.toString(Charsets.UTF_8), portable)) return null
            val offset = input.filePointer
            var result = 2166136261L
            val buffer = ByteArray(65536)
            while (true) {
                val countRead = input.read(buffer)
                if (countRead < 0) break
                for (n in 0 until countRead) result = ((result xor (buffer[n].toLong() and 255)) * 16777619) and 0xffffffffL
            }
            if (result != machineHash) return null
            input.seek(offset)
            if (u32() != 0x19800819L) return null
            val version = u32()
            // The newest version is checked by Sessions with the rest of the header.
            if (version < 6) return null
            val name = ByteArray(32).also { input.readFully(it) }.toString(Charsets.US_ASCII).substringBefore('\u0000')
            // Devices whose slots take linked card files: the Dana and the VZ Palms.
            if (Device.entries.none { it.id == name && it.linkedCards }) return null
            Header(offset, version, portable, name)
        }
    } catch (_: Exception) { null }

    private fun validManifest(text: String, portable: Boolean): Boolean {
        if ('\u0000' in text || '\r' in text) return false
        val lines = text.removeSuffix("\n").split('\n')
        if (lines.size !in 2..3 || lines[0] != "frond-${if (portable) "portable" else "linked"}-sd 3") return false
        val ids = mutableSetOf<String>()
        val identities = mutableSetOf<String>()
        val serials = mutableSetOf<String>()
        for (line in lines.drop(1)) {
            val words = Regex("\"(?:[^\"\\\\]|\\\\.)*\"|[^\\s]+").findAll(line).map { it.value }.toList()
            if (words.size != if (portable) 10 else 13) return false
            val id = words[0]
            if (id !in listOf("0", "2") || !ids.add(id) || words[1] != "sd") return false
            if (portable && words[2] != if (id == "0") "sd0.frondsd" else "sd2.frondsd") return false
            if (!portable && (!words[2].startsWith("\"/") || !words[2].endsWith('"'))) return false
            val key = words[3]
            if (!key.matches(Regex("[0-9a-f]{32}")) || !identities.add(key) || !serials.add(key.take(8))) return false
            if (words[4].toLongOrNull() != key.take(8).toLong(16) || words[6] !in listOf("0", "1") || words[7] !in listOf("0", "1")) return false
            val protocol = words[if (portable) 9 else 11]
            if (protocol !in listOf("sd", "mmc")) return false
            val rule = CardFamily(if (protocol == "mmc") "legacy-mmc-spi" else "sdsc", 32L shl 20,
                2048L shl 20, "legacy-csd-fixed-512", if (protocol == "mmc") listOf(32L,64L,128L,256L).map { it shl 20 } else emptyList())
            if (!rule.accepts(words[5].toLongOrNull() ?: return false)) return false
            for (index in if (portable) listOf(8) else listOf(8,9,10,12)) if (words[index].toULongOrNull() == null) return false
        }
        return true
    }
}
