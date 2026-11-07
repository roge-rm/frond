package com.rm.frond

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile

// A host folder copied once onto a new SD card or Memory Stick Pro: how much room it needs on a
// FAT16 card of each size, a blank FAT16 image like the card tool's, and the copy with FatImage.
// The sums match desktop/FolderCard.h.
object FolderCard {
    // A folder or file to copy; a picked document tree in the app, a fake in the tests.
    interface Node {
        val name: String
        val isDirectory: Boolean
        val size: Long
        fun children(): List<Node>
        fun open(): InputStream
    }

    // What a folder takes: file sizes and the directory slots each folder needs.
    class Plan(
        val problem: String?,
        val bytes: Long,
        val files: Int,
        val folders: Int,
        val rootSlots: Int,
        val sizes: List<Long>,
        val folderSlots: List<Int>,
    )

    class Geometry(val clusterBytes: Int, val clusters: Int)

    // The FAT16 layout of a card of `mib` MiB (the core's frondSd::layout).
    fun geometry(mib: Int): Geometry {
        val sectors = (mib.toLong() shl 20) / 512 - 63
        val cluster = if (mib <= 64) 4 else if (mib <= 256) 8 else if (mib == 512) 16 else if (mib == 1024) 32 else 64
        var fatSectors = 1
        var clusters: Long
        while (true) {
            clusters = minOf((sectors - 1 - 32 - 2 * fatSectors) / cluster, 65524L)
            val required = ((clusters + 2) * 2 + 511).toInt() / 512
            if (fatSectors >= required) break
            fatSectors = required
        }
        return Geometry(cluster * 512, clusters.toInt())
    }

    private val FORBIDDEN = Regex("[\\u0000-\\u001f\\u007f*?<>|\":\\\\/]")

    fun nameUsable(name: String) = name.isNotEmpty() && name.length <= 255 && name != "." && name != ".." &&
        !name.endsWith('.') && !name.endsWith(' ') && !name.startsWith(' ') && !FORBIDDEN.containsMatchIn(name)

    private fun slotsFor(name: String) = 1 + (name.length + 12) / 13

    // Walks the tree. A name FAT can't hold, or two names that differ only in case, sets `problem`.
    fun measure(root: Node, cancelled: () -> Boolean = { false }): Plan {
        var problem: String? = null
        var bytes = 0L
        var files = 0
        var folders = 0
        var rootSlots = 0
        val sizes = mutableListOf<Long>()
        val folderSlots = mutableListOf<Int>()
        fun walk(node: Node, top: Boolean) {
            if (problem != null) return
            if (cancelled()) { problem = COPY_CANCELLED; return }
            var slots = if (top) 0 else 2
            val seen = HashSet<String>()
            for (child in node.children()) {
                if (!nameUsable(child.name)) { problem = "${child.name} can't be used on a card."; return }
                if (!seen.add(child.name.lowercase())) {
                    problem = "${child.name} and another name differ only in case, which a card can't tell apart."
                    return
                }
                slots += slotsFor(child.name)
                if (child.isDirectory) {
                    folders++
                    walk(child, false)
                    if (problem != null) return
                } else {
                    if (child.size < 0) { problem = "Couldn't read the size of ${child.name}."; return }
                    files++
                    bytes += child.size
                    sizes += child.size
                }
            }
            if (top) rootSlots = slots else folderSlots += slots
        }
        walk(root, true)
        if (problem == null && rootSlots > 511) problem = "There are too many names in the top folder for a card."
        return Plan(problem, bytes, files, folders, rootSlots, sizes, folderSlots)
    }

    // Bytes the folder takes in whole clusters on a card of `mib`, with a few spare clusters.
    fun clusterBytesNeeded(plan: Plan, mib: Int): Long {
        val g = geometry(mib)
        var clusters = 4L
        for (size in plan.sizes) clusters += (size + g.clusterBytes - 1) / g.clusterBytes
        for (slots in plan.folderSlots) clusters += maxOf(1L, (slots * 32L + g.clusterBytes - 1) / g.clusterBytes)
        return clusters * g.clusterBytes
    }

    fun fits(plan: Plan, mib: Int): Boolean {
        if (plan.problem != null) return false
        val g = geometry(mib)
        return clusterBytesNeeded(plan, mib) <= g.clusters.toLong() * g.clusterBytes
    }

    // The smallest of `sizes` (MiB, smallest first) the folder fits on, or null.
    fun smallestFit(plan: Plan, sizes: List<Int>): Int? = sizes.firstOrNull { fits(plan, it) }

    // The types a folder can become: SD and Memory Stick Pro. A classic stick has its own file
    // system, which only the Palm writes.
    fun kinds(devices: List<Device> = Device.entries): List<String> = CardChoices.kindsTaken(devices).filter { it != "ms" }

    // ---- The blank image ----

    private fun put16(b: ByteArray, at: Int, v: Int) { b[at] = v.toByte(); b[at + 1] = (v shr 8).toByte() }
    private fun put32(b: ByteArray, at: Int, v: Long) { put16(b, at, (v and 0xffff).toInt()); put16(b, at + 2, (v shr 16).toInt()) }

    // The non-zero sectors of a fresh card by sector number, as the core's frondSd::layout makes them.
    fun layout(mib: Int): Map<Long, ByteArray> {
        val start = 63L
        val total = (mib.toLong() shl 20) / 512
        val sectors = total - start
        val cluster = if (mib <= 64) 4 else if (mib <= 256) 8 else if (mib == 512) 16 else if (mib == 1024) 32 else 64
        val aligned = mib > 128
        var fatSectors = 1
        var reserved = 1
        var clusters: Long
        while (true) {
            clusters = minOf((sectors - reserved - 32 - 2 * fatSectors) / cluster, 65524L)
            val required = ((clusters + 2) * 2 + 511).toInt() / 512
            if (fatSectors >= required) break
            fatSectors = required
        }
        if (aligned) reserved = (total - clusters * cluster - start - 32 - 2 * fatSectors).toInt()
        val out = LinkedHashMap<Long, ByteArray>()
        val mbr = ByteArray(512)
        mbr[446 + 1] = 1; mbr[446 + 2] = 1
        mbr[446 + 4] = 6; mbr[446 + 5] = 254.toByte(); mbr[446 + 6] = 255.toByte(); mbr[446 + 7] = 255.toByte()
        put32(mbr, 446 + 8, start); put32(mbr, 446 + 12, sectors)
        mbr[510] = 0x55; mbr[511] = 0xaa.toByte()
        out[0] = mbr
        val boot = ByteArray(512)
        boot[0] = 0xeb.toByte(); boot[1] = 0x3c; boot[2] = 0x90.toByte()
        "FROND001".toByteArray(Charsets.US_ASCII).copyInto(boot, 3)
        put16(boot, 11, 512); boot[13] = cluster.toByte(); put16(boot, 14, reserved)
        boot[16] = 2; put16(boot, 17, 512)
        if (sectors < 65536) put16(boot, 19, sectors.toInt()) else put32(boot, 32, sectors)
        boot[21] = 0xf8.toByte(); put16(boot, 22, fatSectors); put16(boot, 24, 63); put16(boot, 26, 255)
        put32(boot, 28, start); boot[36] = 0x80.toByte(); boot[38] = 0x29; put32(boot, 39, 0x46524f4e)
        "FROND      ".toByteArray(Charsets.US_ASCII).copyInto(boot, 43)
        "FAT16   ".toByteArray(Charsets.US_ASCII).copyInto(boot, 54)
        boot[510] = 0x55; boot[511] = 0xaa.toByte()
        out[start] = boot
        for (fat in 0 until 2) {
            val sector = ByteArray(512)
            put16(sector, 0, 0xfff8); put16(sector, 2, 0xffff)
            out[start + reserved + fat.toLong() * fatSectors] = sector
        }
        val root = ByteArray(512)
        "FROND      ".toByteArray(Charsets.US_ASCII).copyInto(root, 0)
        root[11] = 8
        out[start + reserved + 2L * fatSectors] = root
        return out
    }

    // A blank FAT16 card of `mib` MiB as a sparse file.
    fun blank(file: File, mib: Int) {
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(mib.toLong() shl 20)
            for ((sector, bytes) in layout(mib)) {
                raf.seek(sector * 512)
                raf.write(bytes)
            }
        }
    }

    // ---- The copy ----

    private class Stopped : IOException("stopped")

    private class Counting(input: InputStream, val cancelled: () -> Boolean, val onBytes: (Int) -> Unit) : FilterInputStream(input) {
        override fun read(): Int {
            if (cancelled()) throw Stopped()
            val b = super.read()
            if (b >= 0) onBytes(1)
            return b
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (cancelled()) throw Stopped()
            val n = super.read(b, off, len)
            if (n > 0) onBytes(n)
            return n
        }
    }

    // Copies the tree onto the blank image at `image`. Returns an error, COPY_CANCELLED, or null.
    fun copy(image: File, root: Node, plan: Plan, cancelled: () -> Boolean, progress: ((Long, Long) -> Unit)?): String? {
        val total = maxOf(plan.bytes, 1L)
        var done = 0L
        try {
            FatImage.open(image, true).use { fat ->
                fat.bulk = true
                fun put(path: String, nodes: List<Node>) {
                    for (node in nodes) {
                        if (cancelled()) throw Stopped()
                        if (node.isDirectory) {
                            fat.makeFolder(path, node.name)
                            put(SdCard.joined(path, node.name), node.children())
                        } else {
                            node.open().use { input ->
                                fat.add(path, node.name, Counting(input, cancelled) { n -> done += n; progress?.invoke(done, total) }, node.size)
                            }
                        }
                    }
                }
                put("/", root.children())
                fat.sync()
            }
        } catch (e: Stopped) {
            return COPY_CANCELLED
        } catch (e: FatImage.FatException) {
            return if (e.message?.contains("enough space") == true) "The folder doesn't fit on a card that size." else "Couldn't copy the folder: ${e.message}"
        } catch (e: IOException) {
            return "Couldn't copy the folder: ${e.message ?: "read error"}"
        }
        return null
    }

    // ---- A picked document tree ----

    class Document(
        private val resolver: ContentResolver,
        private val tree: Uri,
        private val id: String,
        override val name: String,
        override val isDirectory: Boolean,
        override val size: Long,
    ) : Node {
        override fun children(): List<Node> {
            val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id)
            val columns = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
            )
            val out = mutableListOf<Node>()
            resolver.query(uri, columns, null, null, null)?.use { c ->
                while (c.moveToNext()) {
                    val dir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                    out += Document(resolver, tree, c.getString(0), c.getString(1) ?: "", dir, if (c.isNull(3)) -1L else c.getLong(3))
                }
            }
            return out.sortedBy { it.name.lowercase() }
        }

        override fun open(): InputStream =
            resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id)) ?: throw IOException("Couldn't read $name")

        companion object {
            fun root(resolver: ContentResolver, tree: Uri): Document {
                val id = DocumentsContract.getTreeDocumentId(tree)
                val name = try {
                    resolver.query(DocumentsContract.buildDocumentUriUsingTree(tree, id),
                        arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { c ->
                        if (c.moveToFirst()) c.getString(0) else null
                    }
                } catch (e: Exception) {
                    null
                }
                return Document(resolver, tree, id, name ?: "Card", true, 0)
            }
        }
    }
}
