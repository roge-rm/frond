package com.rm.frond

import java.io.File
import java.io.DataOutputStream
import java.io.IOException
import java.io.RandomAccessFile

// Older 68k saves with linked card files, and portable copies of those saves.
internal object DanaSession {
    data class Header(val offset: Long, val version: Long, val portable: Boolean, val device: String,
                      val machineHash: Long, val rows: List<List<String>>)
    private val word = Regex("""(?:"(?:[^"\\]|\\.)*"|[^\s]+)""")
    private val absolute = Regex("""^"(?:(?:[A-Za-z]:)?[/\\])""")
    private fun hash(bytes: ByteArray): Long = bytes.fold(2166136261L) { value, byte ->
        ((value xor (byte.toLong() and 255)) * 16777619) and 0xffffffffL
    }
    private val magics = listOf("FRONDLINKSD1\n", "FRONDPORTSD1\n")
    fun wrapped(file: File): Boolean = try {
        file.inputStream().use { input ->
            val bytes = ByteArray(13)
            java.io.DataInputStream(input).readFully(bytes)
            String(bytes, Charsets.US_ASCII) in magics
        }
    } catch (_: IOException) { false }

    // A portable folder's save, or a linked one with a card outside its machine folder's own card
    // files (sd.frondsd, sd2.frondsd, ms.frondms), as dana-cards/ cards were before Cards.
    fun independent(file: File): Boolean {
        if (!wrapped(file)) return false
        val header = inspect(file) ?: return true
        if (header.portable) return true
        val dir = file.absoluteFile.parentFile?.normalize() ?: return true
        return Cards.linkedRows(file).any { File(it.path).absoluteFile.normalize() != File(dir, sibling(it.n, it.stick)) &&
            !File(dir, sibling(it.n, it.stick)).isFile && !Cards.readSlot(dir, it.n).swap }
    }

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
            if (hash(manifest) != manifestHash) return null
            val rows = parseManifest(manifest.toString(Charsets.UTF_8), portable) ?: return null
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
            Header(offset, version, portable, name, machineHash, rows)
        }
    } catch (_: Exception) { null }

    private fun sibling(n: Int, stick: Boolean) = if (stick) "ms.frondms" else if (n == 1) "sd2.frondsd" else "sd.frondsd"

    private fun archivePath(name: String) = "\"/frond-archive/$name\""

    private fun storedBytes(info: CardFile.Info): Long {
        val chunk = 1L shl 20
        return if (info.side == 0L) info.payload else (info.payload + chunk - 1) / chunk * chunk + info.side
    }

    // Materialise the cards beside a movable copy of the save. Swapped slots attach these
    // siblings on startup and never need the original absolute paths.
    fun stage(file: File, dest: File, packed: Boolean, includeCards: Boolean = true,
              cancelled: () -> Boolean = { false }, library: File? = null): String? {
        val header = inspect(file) ?: return "That session's card list is damaged."
        val device = Device.byId(header.device) ?: return "That session's device is unknown."
        val dir = file.absoluteFile.parentFile ?: return "Couldn't read the session."
        val rows = Cards.linkedRows(file).associateBy { it.n }
        val manifest = StringBuilder("frond-linked-sd 3\n")
        val mismatch = "That session's card files don't match."
        val cardRows = header.rows.toMutableList()
        if (!packed && includeCards && !header.portable && library != null) {
            for (n in 0 until Cards.SLOTS) {
                val name = Cards.cardFileIn(dir, n) ?: continue
                val slot = Cards.readSlot(dir, n)
                val previous = cardRows.firstOrNull { (it[0] == "2") == (n == 1) }
                if (!slot.swap && previous != null) continue
                val card = slot.card?.let { Cards.readCard(Cards.entryDir(library, it)) } ?: return mismatch
                val stick = name == "ms.frondms"
                if (name != sibling(n, stick)) return mismatch
                val info = CardFile.info(File(dir, name)) ?: return mismatch
                val used = cardRows.filter { it != previous }.map { it[3].take(8) }
                var key = card.key?.takeIf { Cards.validKey(it) } ?: String(Cards.newIdentity(), Charsets.US_ASCII)
                while (key.take(8) in used) key = String(Cards.newIdentity(), Charsets.US_ASCII)
                if (previous != null && previous[3] != key)
                    return "Open and save the session with its replacement card first."
                val inserted = previous?.get(7) ?: "1"
                val id = if (stick) "1" else if (n == 1) "2" else "0"
                val size = if (stick) storedBytes(info) else info.payload
                cardRows.remove(previous)
                cardRows += listOf(id, if (stick) "ms" else "sd", archivePath(name), key,
                    key.take(8).toLong(16).toString(), size.toString(), if (card.writeProtected) "1" else "0",
                    inserted, "0", "0", "0", card.type, "0")
            }
        }
        val expected = cardRows.map { row ->
            if (header.portable) row[2] else sibling(if (row[0] == "2") 1 else 0, row[1] == "ms")
        }.toSet()
        if (packed && (Cards.cardFiles + listOf("sd0.frondsd", "sd2.frondsd")).any {
                it !in expected && File(dir, it).isFile
            }) return mismatch
        for (row in cardRows) {
            if (cancelled()) return COPY_CANCELLED
            val n = if (row[0] == "2") 1 else 0
            val stick = row[1] == "ms"
            val name = sibling(n, stick)
            val protocol = row[if (header.portable) 9 else 11]
            val family = Cards.linkedFamily(device, if (stick) "ms" else protocol, n) ?: return mismatch
            val size = row[5].toLong()
            if (!stick && !family.accepts(size)) return mismatch
            val slot = Cards.readSlot(dir, n)
            val source = File(dir, if (header.portable) row[2] else name)
            val target = File(dest, name)
            val detached = row[2] == archivePath(name) && row.subList(8, 11).all { it == "0" }
            val omitted = slot.swap && !source.isFile && !header.portable && (!packed || detached)
            if (includeCards && !omitted) {
                if (packed && !header.portable && !detached && row[12] != "0")
                    return "Import a portable archive with the card's saved changes."
                if (packed || header.portable || detached) {
                    if (!packed && !header.portable && (Cards.hasGenerations(dir, name) || Cards.hasJournals(dir, name) ||
                        File(dir, ".settling-$name").exists())) return "The card has an unfinished write."
                    if (!source.isFile) return "Import the complete session archive with its card files."
                    val info = CardFile.info(source, checked = true) ?: return mismatch
                    if (info.version != 2L || info.nand || info.classic != stick || storedBytes(info) != size ||
                        (stick && !family.accepts(classicV2Physical(info.payload, info.side) / 521 * 512))) return mismatch
                    if (header.portable && CardFile.header(source)?.let { le32(it, 24) } != row[8].toLongOrNull()) return mismatch
                    source.inputStream().use { input ->
                        target.outputStream().use { output ->
                            val buffer = ByteArray(1 shl 20)
                            while (true) {
                                if (cancelled()) return COPY_CANCELLED
                                val count = input.read(buffer)
                                if (count < 0) break
                                output.write(buffer, 0, count)
                            }
                        }
                    }
                } else {
                    val linked = rows[n] ?: return mismatch
                    val from = if (source.isFile) source else File(linked.path)
                    val result = Cards.runTool(arrayOf("copy-linked", from.path, target.path, row[3],
                        row.subList(8, 11).joinToString(" "), row[12], row[5], row[1]), cancelled)
                    if (!result.ok) return Cards.toolError(result)
                }
            }
            val saved = row.subList(3, 8).joinToString(" ")
            manifest.append("${row[0]} ${row[1]} ${archivePath(name)} $saved 0 0 0 $protocol 0\n")
            if (!Cards.writeSlot(dest, Cards.Slot(present = true, swap = true, name = slot.name), n))
                return "Couldn't save the session's card list."
        }
        val bytes = manifest.toString().toByteArray(Charsets.UTF_8)
        RandomAccessFile(file, "r").use { input ->
            input.seek(header.offset)
            DataOutputStream(File(dest, "session.frond").outputStream().buffered()).use { output ->
                output.write(magics[0].toByteArray(Charsets.US_ASCII))
                for (value in listOf(bytes.size.toLong(), header.machineHash, hash(bytes)))
                    output.writeInt(Integer.reverseBytes(value.toInt()))
                output.write(bytes)
                val buffer = ByteArray(1 shl 20)
                while (true) {
                    if (cancelled()) return COPY_CANCELLED
                    val count = input.read(buffer)
                    if (count < 0) break
                    output.write(buffer, 0, count)
                }
            }
        }
        return null
    }

    private fun parseManifest(text: String, portable: Boolean): List<List<String>>? {
        if ('\u0000' in text || '\r' in text) return null
        val lines = text.removeSuffix("\n").split('\n')
        if (lines.size !in 2..3 || lines[0] != "frond-${if (portable) "portable" else "linked"}-sd 3") return null
        val ids = mutableSetOf<String>()
        val identities = mutableSetOf<String>()
        val serials = mutableSetOf<String>()
        val rows = lines.drop(1).map { line -> word.findAll(line).map { it.value }.toList() }
        for (words in rows) {
            if (words.size != if (portable) 10 else 13) return null
            val id = words[0]
            val stick = id == "1" && words[1] == "ms"
            if (!(id in listOf("0", "2") && words[1] == "sd") && !stick || !ids.add(id)) return null
            if (stick && portable) return null
            if (portable && words[2] != if (id == "0") "sd0.frondsd" else "sd2.frondsd") return null
            if (!portable && (!absolute.containsMatchIn(words[2]) || !words[2].endsWith('"'))) return null
            val key = words[3]
            if (!key.matches(Regex("[0-9a-f]{32}")) || !identities.add(key) || !serials.add(key.take(8))) return null
            if (words[4].toLongOrNull() != key.take(8).toLong(16) || words[6] !in listOf("0", "1") || words[7] !in listOf("0", "1")) return null
            val protocol = words[if (portable) 9 else 11]
            if (protocol !in listOf("sd", "mmc")) return null
            // The core checks a stick's size and header on load.
            if (stick) {
                if ((words[5].toLongOrNull() ?: return null) <= 0) return null
                for (index in listOf(8, 9, 10, 12)) if (words[index].toULongOrNull() == null) return null
                continue
            }
            val rule = CardFamily(if (protocol == "mmc") "legacy-mmc-spi" else "sdsc", 32L shl 20,
                2048L shl 20, "legacy-csd-fixed-512", if (protocol == "mmc") listOf(32L,64L,128L,256L).map { it shl 20 } else emptyList())
            if (!rule.accepts(words[5].toLongOrNull() ?: return null)) return null
            for (index in if (portable) listOf(8) else listOf(8,9,10,12)) if (words[index].toULongOrNull() == null) return null
        }
        return rows
    }
}
