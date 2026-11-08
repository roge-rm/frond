package com.rm.frond

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

// The file view for a card opened from Cards: a free card, or one in a machine that isn't running.
// The card is exported to a private raw image, edited there, and written back when the view closes.
// The machine's run lock (if it has one) is held until then.
class CardImageFiles(private val activity: HomeActivity, private val home: HomeModel, private val sessions: File) : FilesModel {
    private val app = activity.applicationContext
    private val image = File(app.filesDir, "card-files/work.img")
    private var source: Cards.Source? = null
    private var card: Cards.Card? = null

    override var title by mutableStateOf("")
        private set
    override val memoryStick get() = false
    override var canWrite by mutableStateOf(true)
        private set
    override var path by mutableStateOf<String?>(null)
        private set
    override var entries by mutableStateOf(emptyList<FatImage.Entry>())
        private set
    override var free by mutableStateOf(0L)
        private set
    override var total by mutableStateOf(0L)
        private set
    override var changed by mutableStateOf(false)
        private set
    override val busy: String? get() = if (home.copying) home.copyLabel else null
    override var shortcuts by mutableStateOf(emptyList<String>())
        private set
    override val leaveLabel get() = "Done"
    override var askingLeave by mutableStateOf(false)
    override val prompt = ClashPrompt()
    var note by mutableStateOf<String?>(null)

    private fun ui(block: () -> Unit) = activity.runOnUiThread(block)
    private fun percent(progress: (Long, Long) -> Unit): (Float) -> Unit = { progress((it.coerceIn(0f, 1f) * 1000).toLong(), 1000) }
    private fun resolver(several: Boolean) = CardFiles.Resolver { name -> prompt.ask(name, several) { activity.runOnUiThread(it) } }

    // Exports the card and lists its top folder.
    fun start(row: CardChoices.Row) {
        if (path != null) return
        home.runJob("Opening the card", "Couldn't open the card", null, cancellable = true, reload = false) { cancelled, progress ->
            image.parentFile?.mkdirs()
            image.delete()
            val (found, error) = Cards.cardSource(sessions, row.card.id)
            if (found == null) return@runJob error
            val kept = found.card
            val file = Cards.fileOfKind(kept.kind)
            if (kept.kind == "ms") { found.close(); return@runJob "A Memory Stick can't be opened here." }
            if (found.path.name != file) {
                found.close()
                return@runJob "Open ${found.holder?.let { Cards.machineName(it) } ?: "the machine"} once to finish its last save."
            }
            val need = 2 * kept.bytes + Cards.SPARE_BYTES
            val room = Cards.freeSpace(image.parentFile!!)
            if (room in 0 until need) { found.close(); return@runJob Cards.spaceMessage(2 * kept.bytes) }
            val r = Cards.runTool(arrayOf("export", found.path.path, image.path), cancelled, progress)
            if (!r.ok) { found.close(); image.delete(); return@runJob Cards.toolError(r) }
            val shown = try {
                FatImage.open(image, false).use { listing(it, "/") to SHORTCUTS.filter { s -> it.find(s)?.directory == true } }
            } catch (e: Exception) {
                found.close(); image.delete()
                return@runJob e.message ?: "Couldn't read the card."
            }
            source = found
            card = kept
            ui {
                title = kept.name
                canWrite = !kept.writeProtected
                changed = false
                shortcuts = shown.second
                show("/", shown.first)
            }
            null
        }
    }

    private fun listing(fat: FatImage, at: String) = SdCard.Listing(fat.list(at), fat.freeBytes, fat.totalBytes)

    private fun show(at: String, list: SdCard.Listing) {
        path = at
        entries = list.entries
        free = list.free
        total = list.total
    }

    private fun relist(at: String) {
        val list = FatImage.open(image, false).use { listing(it, at) }
        ui { show(at, list) }
    }

    override fun open(at: String) {
        if (home.copying) return
        activity.lifecycleScope.launch {
            try {
                val list = withContext(Dispatchers.IO) { FatImage.open(image, false).use { listing(it, at) } }
                show(at, list)
            } catch (e: Exception) {
                home.alert = "Couldn't open the folder" to (e.message ?: "Couldn't read the card.")
            }
        }
    }

    override fun up() {
        val p = path ?: return
        if (p != "/") open(p.substringBeforeLast('/').ifEmpty { "/" })
    }

    // Runs a change on the edited image, then lists the folder again. Cancel keeps what was done.
    private fun change(label: String, failTitle: String, work: (FatImage, () -> Boolean, (Float) -> Unit, String) -> String?) {
        val at = path ?: return
        home.runJob(label, failTitle, null, cancellable = true, reload = false) { cancelled, progress ->
            val bar = percent(progress)
            var message: String? = null
            var error: String? = null
            try {
                FatImage.open(image, true).use { fat ->
                    try {
                        message = work(fat, cancelled, bar, at)
                    } finally {
                        if (fat.committed) ui { changed = true }
                    }
                }
            } catch (e: FatImage.Cancelled) {
                error = COPY_CANCELLED
            } catch (e: IOException) {
                error = e.message ?: "Couldn't change the card."
            }
            relist(at)
            message?.let { m -> ui { note = m } }
            error
        }
    }

    private fun counted(report: CardFiles.Report, what: String) =
        "${report.done} $what" + if (report.skipped > 0) ", ${report.skipped} skipped" else ""

    override fun addFiles(uris: List<Uri>) = change("Copying", "Couldn't add the files") { fat, cancelled, bar, at ->
        val resolver = resolver(uris.size > 1)
        val content = app.contentResolver
        var added = 0
        var skipped = 0
        uris.forEachIndexed { i, uri ->
            if (cancelled()) throw FatImage.Cancelled()
            val name = HostFiles.displayName(content, uri) ?: throw IOException("That file has no name.")
            var size = HostFiles.size(content, uri)
            var temp: File? = null
            try {
                if (size == null) {
                    temp = File(app.cacheDir, "card-files-incoming")
                    (content.openInputStream(uri) ?: throw IOException("Couldn't read $name.")).use { input -> temp.outputStream().use { input.copyTo(it) } }
                    size = temp.length()
                }
                val put = CardFiles.addStream(fat, at, name, size, {
                    temp?.inputStream() ?: content.openInputStream(uri) ?: throw IOException("Couldn't read $name.")
                }, resolver)
                if (put == null) skipped++ else added++
            } finally { temp?.delete() }
            bar((i + 1f) / uris.size)
        }
        counted(CardFiles.Report(added, skipped), "added")
    }

    override fun addFolder(tree: Uri) = change("Copying", "Couldn't add the folder") { fat, cancelled, bar, at ->
        val root = FolderCard.Document.root(app.contentResolver, tree)
        val plan = FolderCard.measure(root, cancelled)
        plan.problem?.let { throw IOException(it) }
        if (plan.bytes > fat.freeBytes) throw IOException("There isn't room on the card for that folder.")
        val all = maxOf(plan.bytes, 1L)
        counted(CardFiles.importTree(fat, root, at, resolver(true), cancelled) { bar(it.toFloat() / all) }, "files added")
    }

    override fun makeFolder(name: String) = change("Copying", "Couldn't make the folder") { fat, _, _, at -> fat.makeFolder(at, name.trim()); null }

    override fun renameEntry(entry: FatImage.Entry, newName: String) =
        change("Renaming", "Couldn't rename the entry") { fat, _, _, at -> CardFiles.rename(fat, SdCard.joined(at, entry.name), newName); null }

    override fun deleteEntries(entries: List<FatImage.Entry>) = change("Deleting", "Couldn't delete") { fat, cancelled, _, at ->
        CardFiles.delete(fat, entries.map { SdCard.joined(at, it.name) }, cancelled)
        "Deleted"
    }

    override fun paste(sources: List<String>, move: Boolean) =
        change(if (move) "Moving" else "Copying", "Couldn't paste") { fat, cancelled, bar, at ->
            val all = maxOf(if (move) 1L else CardFiles.bytesIn(fat, sources), 1L)
            counted(CardFiles.paste(fat, sources, at, move, resolver(sources.size > 1), cancelled) { bar(it.toFloat() / all) }, if (move) "moved" else "copied")
        }

    override fun exportFile(entry: FatImage.Entry, to: Uri) {
        val at = path ?: return
        home.runJob("Copying", "Couldn't export the file", null, cancellable = true, reload = false) { _, _ ->
            try {
                val out = app.contentResolver.openOutputStream(to, "wt") ?: return@runJob "Couldn't write there."
                out.use { o -> FatImage.open(image, false).use { it.read(it.find(SdCard.joined(at, entry.name)) ?: throw IOException("Not on the card."), o) } }
                ui { note = "${entry.name} exported" }
                null
            } catch (e: IOException) {
                e.message ?: "Couldn't write there."
            }
        }
    }

    override fun exportEntries(entries: List<FatImage.Entry>, tree: Uri) {
        val at = path ?: return
        home.runJob("Copying", "Couldn't copy to the folder", null, cancellable = true, reload = false) { cancelled, progress ->
            val bar = percent(progress)
            try {
                FatImage.open(image, false).use { fat ->
                    val paths = entries.map { SdCard.joined(at, it.name) }
                    val all = maxOf(CardFiles.bytesIn(fat, paths), 1L)
                    val n = CardFiles.exportTree(fat, paths, DocumentHost.root(app.contentResolver, tree), cancelled) { bar(it.toFloat() / all) }
                    ui { note = "$n files copied" }
                }
                null
            } catch (e: FatImage.Cancelled) {
                COPY_CANCELLED
            } catch (e: IOException) {
                e.message ?: "Couldn't copy to the folder."
            }
        }
    }

    override fun leaveFiles() {
        if (changed) save() else close()
    }

    override fun insertEdited() = leaveFiles()

    override fun discard() = close()

    // Drops the working image and the machine's lock, and closes the view.
    private fun close() {
        val held = source
        source = null
        card = null
        ui { path = null; changed = false }
        activity.lifecycleScope.launch(Dispatchers.IO) {
            held?.close()
            image.delete()
            FatImage.journalOf(image).delete()
        }
    }

    // Writes the edited image back as the card's file, then closes the view.
    private fun save() {
        val held = source ?: return
        val kept = card ?: return
        home.runJob("Saving the card", "Couldn't save the card", null, cancellable = true, reload = false) { cancelled, progress ->
            val file = held.path
            val temp = File(file.parentFile, file.name + ".files-new")
            temp.delete()
            val need = kept.bytes + Cards.SPARE_BYTES
            val room = Cards.freeSpace(file.parentFile!!)
            if (room in 0 until need) return@runJob Cards.spaceMessage(kept.bytes)
            val r = Cards.runTool(arrayOf("import", image.path, Cards.toolKind(kept.kind), temp.path), cancelled, progress)
            if (!r.ok) { temp.delete(); return@runJob Cards.toolError(r) }
            val library = Cards.libraryFor(sessions)
            val error = Cards.LibraryLock(library).use { lock ->
                when {
                    !lock.ok -> Cards.BUSY
                    !file.isFile -> "The card moved to a machine while it was open, so the changes weren't saved."
                    !temp.renameTo(file) -> "Couldn't write the card."
                    else -> {
                        held.holder?.let { dir -> Cards.writeSlot(dir, Cards.readSlot(dir).copy(swap = true)) }
                        null
                    }
                }
            }
            temp.delete()
            // Only a retry that could still work keeps the view open.
            if (error == null || error != Cards.BUSY) close()
            error
        }
    }

    private companion object {
        val SHORTCUTS = listOf("/PALM/Launcher", "/DCIM")
    }
}
