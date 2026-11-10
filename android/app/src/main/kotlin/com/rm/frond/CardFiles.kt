package com.rm.frond

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

// The file manager's operations on a FAT16 card image: paste (copy or move), delete, add from a
// folder tree, copy out to a folder tree, and the name-clash rules. Pure JVM, so the tests run it.
object CardFiles {
    enum class Clash { REPLACE, SKIP, KEEP_BOTH, CANCEL }

    // The answer to a name clash, and whether it holds for the rest of the job.
    class Decision(val clash: Clash, val all: Boolean = false)

    // Asks about one clash; a "for all" answer is reused for the rest of the job.
    class Resolver(private val ask: (name: String) -> Decision) {
        private var remembered: Clash? = null
        fun decide(name: String): Clash {
            remembered?.let { return it }
            val d = ask(name)
            if (d.all && d.clash != Clash.CANCEL) remembered = d.clash
            return d.clash
        }
    }

    class Report(val done: Int, val skipped: Int)

    // "name (2).ext", the first that isn't taken (names compare without case), cut to fit FAT's 255.
    fun keepBothName(name: String, taken: (String) -> Boolean): String {
        val dot = name.lastIndexOf('.').takeIf { it > 0 } ?: -1
        val base = if (dot >= 0) name.substring(0, dot) else name
        val ext = if (dot >= 0) name.substring(dot) else ""
        for (n in 2..99_999) {
            val tail = " ($n)$ext"
            var room = base.take(maxOf(1, 255 - tail.length))
            if (room.isNotEmpty() && room.last().isHighSurrogate()) room = room.dropLast(1)
            val candidate = room.trimEnd() + tail
            if (!taken(candidate)) return candidate
        }
        throw FatImage.FatException("Too many similar names in this folder.")
    }

    // The path with each folder named as the card has it ("/palm/launcher" is "/PALM/Launcher").
    fun realPath(fat: FatImage, path: String): String {
        var shown = "/"
        for (part in FatImage.parts(path)) shown = SdCard.joined(shown, fat.find(SdCard.joined(shown, part))?.name ?: part)
        return shown
    }

    private fun lastName(path: String) = FatImage.parts(path).last()
    private fun parentOf(path: String) = FatImage.parts(path).dropLast(1).joinToString("/", "/")
    private fun names(fat: FatImage, folder: String) = fat.list(folder).map { it.name.lowercase() }.toSet()

    // Paste: copies or moves each source into destFolder. A folder can't go into itself, and nothing
    // starts if any source would. A copy next to its original is named "(2)" without asking.
    fun paste(fat: FatImage, sources: List<String>, destFolder: String, move: Boolean, resolver: Resolver,
              cancelled: () -> Boolean = { false }, bytes: (Long) -> Unit = {}): Report {
        for (source in sources) {
            val entry = fat.find(source) ?: throw FatImage.FatException("${lastName(source)} isn't on the card.")
            if (entry.directory && FatImage.within(source, destFolder)) throw FatImage.FatException("${entry.name} can't go into itself.")
        }
        var done = 0
        var skipped = 0
        var copied = 0L
        for (source in sources) {
            if (cancelled()) throw FatImage.Cancelled()
            val name = lastName(source)
            val here = FatImage.parts(parentOf(source.lowercase())) == FatImage.parts(destFolder.lowercase())
            if (move && here) { skipped++; continue }
            val taken = fat.list(destFolder).firstOrNull { it.name.equals(name, ignoreCase = true) }
            var target = name
            var victim: String? = null
            if (taken != null) {
                val choice = if (here) Clash.KEEP_BOTH else resolver.decide(name)
                when (choice) {
                    Clash.CANCEL -> throw FatImage.Cancelled()
                    Clash.SKIP -> { skipped++; continue }
                    Clash.KEEP_BOTH -> {
                        val used = names(fat, destFolder)
                        target = keepBothName(name) { it.lowercase() in used }
                    }
                    Clash.REPLACE -> {
                        victim = SdCard.joined(destFolder, taken.name)
                        if (FatImage.within(victim, source)) throw FatImage.FatException("${taken.name} holds what you are replacing it with.")
                    }
                }
            }
            val base = copied
            val work = {
                if (move) fat.move(source, destFolder, target)
                else fat.copy(source, destFolder, target, cancelled) { bytes(base + it) }
            }
            // A replace swaps the entries in one commit once the new one is complete.
            if (victim == null) work() else fat.replace(victim, work)
            if (!move) copied += fat.sizeOf(SdCard.joined(destFolder, target))
            done++
        }
        return Report(done, skipped)
    }

    fun rename(fat: FatImage, path: String, newName: String) {
        val name = newName.trim()
        FatImage.checkName(name)
        if (lastName(path) == name) return
        fat.move(path, parentOf(path), name)
    }

    fun delete(fat: FatImage, paths: List<String>, cancelled: () -> Boolean = { false }): Int {
        var n = 0
        for (path in paths) {
            if (cancelled()) throw FatImage.Cancelled()
            fat.remove(path)
            n++
        }
        return n
    }

    // The bytes in the sources, for a progress bar.
    fun bytesIn(fat: FatImage, paths: List<String>): Long = paths.sumOf { fat.sizeOf(it) }

    // A file into the folder under `name`, with the clash rule. Returns the name it went in under,
    // or null when skipped.
    fun addStream(fat: FatImage, folder: String, name: String, size: Long, open: () -> InputStream, resolver: Resolver): String? {
        FatImage.checkName(name)
        var target = name
        var victim: String? = null
        val taken = fat.list(folder).firstOrNull { it.name.equals(name, ignoreCase = true) }
        if (taken != null) {
            when (resolver.decide(name)) {
                Clash.CANCEL -> throw FatImage.Cancelled()
                Clash.SKIP -> return null
                Clash.KEEP_BOTH -> {
                    val used = names(fat, folder)
                    target = keepBothName(name) { it.lowercase() in used }
                }
                Clash.REPLACE -> {
                    if (taken.directory) throw FatImage.FatException("$name is a folder on the card.")
                    victim = SdCard.joined(folder, taken.name)
                }
            }
        }
        // A replace keeps the old file until the new one is complete, then swaps them in one commit.
        val work = { open().use { fat.add(folder, target, it, size) } }
        if (victim == null) work() else fat.replace(victim, work)
        return target
    }

    // Copies a picked folder into `folder` as a folder of its own, keeping the structure. A folder
    // already there is filled; a file's clash follows the resolver.
    fun importTree(fat: FatImage, root: FolderCard.Node, folder: String, resolver: Resolver,
                   cancelled: () -> Boolean = { false }, bytes: (Long) -> Unit = {}): Report {
        var added = 0
        var skipped = 0
        var copied = 0L
        fun into(parent: String, node: FolderCard.Node) {
            if (cancelled()) throw FatImage.Cancelled()
            var name = node.name
            if (!FolderCard.nameUsable(name)) throw FatImage.FatException("\"$name\" can't be used on a card.")
            val taken = fat.list(parent).firstOrNull { it.name.equals(name, ignoreCase = true) }
            if (node.isDirectory) {
                if (taken == null) fat.makeFolder(parent, name)
                else if (taken.directory) name = taken.name
                else {
                    val used = names(fat, parent)
                    name = keepBothName(name) { it.lowercase() in used }
                    fat.makeFolder(parent, name)
                }
                val here = SdCard.joined(parent, name)
                for (child in node.children()) into(here, child)
            } else {
                val size = node.size
                if (size < 0) throw FatImage.FatException("Couldn't read the size of ${node.name}.")
                val base = copied
                val put = addStream(fat, parent, name, size, {
                    Counting(node.open(), cancelled) { n -> bytes(base + n) }
                }, resolver)
                if (put == null) skipped++ else added++
                copied += size
            }
        }
        into(folder, root)
        return Report(added, skipped)
    }

    // Reports the bytes read from an input as it is read, and stops it when cancelled.
    private class Counting(input: InputStream, val cancelled: () -> Boolean, val onBytes: (Long) -> Unit) : java.io.FilterInputStream(input) {
        private var total = 0L
        override fun read(): Int {
            if (cancelled()) throw FatImage.Cancelled()
            val b = super.read()
            if (b >= 0) onBytes(++total)
            return b
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (cancelled()) throw FatImage.Cancelled()
            val n = super.read(b, off, len)
            if (n > 0) { total += n; onBytes(total) }
            return n
        }
    }

    // A folder on the host (a picked document tree) that files and folders can be written into.
    interface HostFolder {
        fun folder(name: String): HostFolder
        fun file(name: String): OutputStream
    }

    // Copies files and folders off the card into a host folder. Returns the number of files.
    fun exportTree(fat: FatImage, paths: List<String>, to: HostFolder, cancelled: () -> Boolean = { false },
                   bytes: (Long) -> Unit = {}): Int {
        var files = 0
        var copied = 0L
        fun out(path: String, entry: FatImage.Entry, target: HostFolder) {
            if (cancelled()) throw FatImage.Cancelled()
            if (entry.directory) {
                val inner = target.folder(entry.name)
                for (child in fat.list(path)) out(SdCard.joined(path, child.name), child, inner)
            } else {
                target.file(entry.name).use { o ->
                    fat.read(entry, object : OutputStream() {
                        override fun write(b: Int) = o.write(b)
                        override fun write(b: ByteArray, off: Int, len: Int) {
                            if (cancelled()) throw FatImage.Cancelled()
                            o.write(b, off, len)
                            copied += len
                            bytes(copied)
                        }
                    })
                }
                files++
            }
        }
        for (path in paths) {
            val entry = fat.find(path) ?: throw IOException("${lastName(path)} isn't on the card.")
            out(path, entry, to)
        }
        return files
    }
}
