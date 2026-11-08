package com.rm.frond

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipInputStream

// Files… on Internal: the running machine's RAM databases as one flat list of .prc and .pdb files,
// through the core's database commands (db-list, install, export-db, delete-db). One job at a time,
// off the main thread; the list is read again after each one.
class InternalFiles(
    private val host: ModelHost,
    private val databases: PalmDatabases,
    private val closed: () -> Unit,
) : FilesModel {
    override val title get() = "Internal"
    override val canWrite get() = true
    override var path by mutableStateOf<String?>("/")
        private set
    override var entries by mutableStateOf(emptyList<FatImage.Entry>())
        private set
    override val free get() = 0L
    override val total get() = 0L
    override val changed get() = false
    override var busy by mutableStateOf<String?>(null)
        private set
    override val shortcuts get() = emptyList<String>()
    override val leaveLabel get() = "Done"
    override val prompt = ClashPrompt()
    override val folders get() = false
    override var summary by mutableStateOf<String?>("Reading the databases…")
        private set
    var alert by mutableStateOf<Pair<String, String>?>(null)
    var note by mutableStateOf<String?>(null)
    private var named = emptyMap<String, PalmDatabases.Info>()

    override fun detailOf(entry: FatImage.Entry) = named[entry.name]?.let { "${it.type} · ${it.creator}" }

    fun refresh() = work("Reading", "Couldn't read the databases") { null }

    // Runs task off the main thread, then reads the list again. task returns the note to show.
    private fun work(label: String, failTitle: String, task: (PalmDatabases) -> String?) {
        if (busy != null) return
        busy = label
        host.lifecycleScope.launch {
            var message: String? = null
            var listed: List<PalmDatabases.Info>? = null
            val error = withContext(Dispatchers.IO) {
                val failed = try {
                    message = task(databases)
                    null
                } catch (e: Exception) {
                    failure(e)
                }
                listed = try { databases.list() } catch (e: Exception) { null }
                failed
            }
            busy = null
            listed.let { all ->
                if (all == null) summary = "Couldn't read the databases"
                else {
                    val (shown, by) = entriesOf(all)
                    entries = shown
                    named = by
                    summary = if (all.size == 1) "1 database" else "${all.size} databases"
                }
            }
            if (error != null) alert = failTitle to error else if (message != null) note = message
        }
    }

    override fun open(at: String) = Unit
    override fun up() = leaveFiles()
    override fun leaveFiles() = insertEdited()
    override fun insertEdited() {
        if (busy != null) return
        path = null
        closed()
    }

    override fun addFiles(uris: List<Uri>) = install(uris.map { uri ->
        Source(HostFiles.displayName(host.contentResolver, uri) ?: "A file") { host.contentResolver.openInputStream(uri) }
    })

    // A folder adds the files in it, not its subfolders.
    override fun addFolder(tree: Uri) {
        val files = try {
            FolderCard.Document.root(host.contentResolver, tree).children().filter { !it.isDirectory }
        } catch (e: Exception) {
            alert = "Couldn't add the folder" to "Couldn't read the folder."
            return
        }
        install(files.map { f -> Source(f.name) { f.open() } })
    }

    private class Source(val name: String, val open: () -> InputStream?)

    private fun install(sources: List<Source>) {
        if (sources.isEmpty()) return
        work("Installing", "Couldn't install") { d ->
            var last = ""
            val result = InstallBatch.run(sources, { it.name }, { n, all ->
                if (all > 1) host.runOnUiThread { busy = "Installing $n of $all" }
            }) { last = d.install(it.open) }
            result.summary()?.let { throw PalmDatabases.Failed(it) }
            if (sources.size == 1) last else "${sources.size} added"
        }
    }

    override fun makeFolder(name: String) = Unit
    override fun renameEntry(entry: FatImage.Entry, newName: String) = Unit
    override fun paste(sources: List<String>, move: Boolean) = Unit

    override fun deleteEntries(entries: List<FatImage.Entry>) {
        val names = entries.mapNotNull { named[it.name]?.name }
        if (names.isEmpty()) return
        work("Deleting", "Couldn't delete") { d ->
            val failed = names.mapNotNull { name ->
                try { d.delete(name); null } catch (e: Exception) { "$name: ${failure(e)}" }
            }
            if (failed.isNotEmpty()) throw PalmDatabases.Failed(failed.joinToString("\n"))
            if (names.size == 1) "${names.first()} deleted" else "${names.size} deleted"
        }
    }

    override fun exportFile(entry: FatImage.Entry, to: Uri) {
        val name = named[entry.name]?.name ?: return
        work("Copying", "Couldn't export the file") { d ->
            d.exported(name) { zip ->
                val out = host.contentResolver.openOutputStream(to, "wt") ?: throw IOException("Couldn't write there.")
                out.use { o -> zip.inputStream().use { unzip(it) { _ -> NoClose(o) } } }
            }
            "${entry.name} exported"
        }
    }

    override fun exportEntries(entries: List<FatImage.Entry>, tree: Uri) {
        val names = entries.mapNotNull { named[it.name]?.name }
        if (names.isEmpty()) return
        work("Copying", "Couldn't copy to the folder") { d ->
            val target = DocumentHost.root(host.contentResolver, tree)
            var files = 0
            for (name in names) d.exported(name) { zip -> files += zip.inputStream().use { unzip(it) { n -> target.file(n) } } }
            if (files == 1) "1 file copied" else "$files files copied"
        }
    }

    // Leaves the picked document open for the next part of the copy.
    private class NoClose(private val out: OutputStream) : OutputStream() {
        override fun write(b: Int) = out.write(b)
        override fun write(b: ByteArray, off: Int, len: Int) = out.write(b, off, len)
        override fun close() = out.flush()
    }

    companion object {
        private fun failure(e: Exception) = when (e) {
            is PalmDatabases.Failed -> e.message ?: "Couldn't read or write the file."
            is IOException -> e.message ?: "Couldn't read or write the file."
            else -> "Couldn't read or write the file."
        }

        // The file a database exports as, like the desktop's: its name with \ / : * ? " < > | made _,
        // and .prc for a resource database, else .pdb.
        fun fileName(d: PalmDatabases.Info): String {
            var base = d.name.map { if (it in "\\/:*?\"<>|") '_' else it }.joinToString("")
            if (base.isEmpty() || base == "." || base == "..") base = "Database"
            return base + if (d.resource) ".prc" else ".pdb"
        }

        // The list as entries, by file name; a name that comes up twice gets " (2)".
        fun entriesOf(all: List<PalmDatabases.Info>): Pair<List<FatImage.Entry>, Map<String, PalmDatabases.Info>> {
            val by = LinkedHashMap<String, PalmDatabases.Info>()
            for (d in all) {
                var file = fileName(d)
                if (by.containsKey(file)) file = CardFiles.keepBothName(file) { by.containsKey(it) }
                by[file] = d
            }
            return by.keys.map { FatImage.Entry(it, it, false, 0L, 0) } to by
        }

        // Writes each file in an export's ZIP to out(name), under its own name. Returns how many.
        fun unzip(zip: InputStream, out: (String) -> OutputStream): Int {
            var files = 0
            ZipInputStream(zip).use { z ->
                while (true) {
                    val e = z.nextEntry ?: break
                    val name = e.name.substringAfterLast('/')
                    if (e.isDirectory || name.isEmpty() || name == "." || name == "..") continue
                    out(name).use { z.copyTo(it, 1 shl 16) }
                    files++
                }
            }
            return files
        }
    }
}
