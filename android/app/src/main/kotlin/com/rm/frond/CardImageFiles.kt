package com.rm.frond

import android.content.Context
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

// The file view for a card opened from Cards: a free card, or one in a machine that isn't running.
// The card file is edited in place (CardFile), each change committed as it's made. The card file's
// lock, and the machine's run lock when a machine holds it, are kept until the view closes. A
// machine's slot gets swap before the first write, so it loads the card as changed.
// One per process (`shared`), so a recreated home screen finds the visit still open; it ends when
// the view is left or the home screen really goes.
class CardImageFiles private constructor(private val app: Context, private val sessions: File) : FilesModel {
    // Jobs and alerts go to whichever home screen is current.
    private val home: HomeModel? get() = HomeWork.model
    private var source: Cards.Source? = null
    private var card: Cards.Card? = null
    private var cardFile: CardFile? = null

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
    override val busy: String? get() = if (HomeWork.copying) HomeWork.label else null
    override var shortcuts by mutableStateOf(emptyList<String>())
        private set
    override val leaveLabel get() = "Done"
    override val prompt = ClashPrompt()
    var note by mutableStateOf<String?>(null)

    private fun ui(block: () -> Unit) { HomeWork.scope.launch { block() } }
    private fun percent(progress: (Long, Long) -> Unit): (Float) -> Unit = { progress((it.coerceIn(0f, 1f) * 1000).toLong(), 1000) }
    private fun resolver(several: Boolean) = CardFiles.Resolver { name -> prompt.ask(name, several) { ui(it) } }
    // Whether a visit is open.
    val isOpen: Boolean get() = cardFile != null || path != null

    // Opens the card file and lists its top folder.
    fun start(row: CardChoices.Row) {
        if (path != null) return
        val home = home ?: return
        home.runJob("Opening the card", "Couldn't open the card", null, cancellable = false, reload = false) { _, _ ->
            val (found, error) = Cards.cardSource(sessions, row.card.id)
            if (found == null) return@runJob error
            val kept = found.card
            val file = Cards.fileOfKind(kept.kind)
            if (kept.kind == "ms") { found.close(); return@runJob "A Memory Stick can't be opened here." }
            if (found.path.name != file) {
                found.close()
                return@runJob "Open ${found.holder?.let { Cards.machineName(it) } ?: "the machine"} once to finish its last save."
            }
            val opened = try {
                CardFile.open(found.path, !kept.writeProtected)
            } catch (e: IOException) {
                found.close()
                return@runJob e.message ?: "Couldn't read the card."
            }
            // A machine loads the card as changed from the first write on, even after a crash.
            found.holder?.let { dir ->
                opened.beforeFirstWrite = {
                    val slot = Cards.readSlot(dir)
                    if (!slot.swap && !Cards.writeSlot(dir, slot.copy(swap = true))) throw IOException("Couldn't write the card.")
                }
            }
            val shown = try {
                FatImage.over(opened).use { listing(it, "/") to SHORTCUTS.filter { s -> it.find(s)?.directory == true } }
            } catch (e: Exception) {
                opened.close(); found.close()
                return@runJob e.message ?: "Couldn't read the card."
            }
            source = found
            card = kept
            cardFile = opened
            ui {
                title = kept.name
                canWrite = !kept.writeProtected
                changed = false
                shortcuts = shown.second
                show("/", shown.first)
                // A home screen recreated while it opened shows it too.
                HomeWork.model?.openCards()
            }
            null
        }
    }

    private fun listing(fat: FatImage, at: String) = SdCard.Listing(fat.list(at), fat.freeBytes, fat.totalBytes)

    // The path with each folder named as the card has it.
    private fun shownPath(fat: FatImage, at: String) = CardFiles.realPath(fat, at)

    private fun show(at: String, list: SdCard.Listing) {
        path = at
        entries = list.entries
        free = list.free
        total = list.total
    }

    // The open card's file system, one user at a time.
    private fun <T> fat(block: (FatImage) -> T): T {
        val c = cardFile ?: throw IOException("The card is closed.")
        return synchronized(c) { FatImage.over(c).use(block) }
    }

    private fun relist(at: String) {
        val (real, list) = fat { shownPath(it, at).let { p -> p to listing(it, p) } }
        ui { show(real, list) }
    }

    override fun open(at: String) {
        if (HomeWork.copying) return
        HomeWork.scope.launch {
            try {
                val (real, list) = withContext(Dispatchers.IO) { fat { shownPath(it, at).let { p -> p to listing(it, p) } } }
                show(real, list)
            } catch (e: Exception) {
                HomeWork.model?.alert = "Couldn't open the folder" to (e.message ?: "Couldn't read the card.")
            }
        }
    }

    override fun up() {
        val p = path ?: return
        if (p != "/") open(p.substringBeforeLast('/').ifEmpty { "/" })
    }

    // Runs a change on the card, then lists the folder again. Cancel keeps what was done.
    private fun change(label: String, failTitle: String, work: (FatImage, () -> Boolean, (Float) -> Unit, String) -> String?) {
        val at = path ?: return
        val home = home ?: return
        home.runJob(label, failTitle, null, cancellable = true, reload = false) { cancelled, progress ->
            val bar = percent(progress)
            var message: String? = null
            var error: String? = null
            try {
                fat { fat ->
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
            } catch (e: Exception) {
                error = e.message ?: "Couldn't change the card."
            }
            try { relist(at) } catch (e: Exception) { if (error == null) error = e.message ?: "Couldn't read the card." }
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
        val home = home ?: return
        home.runJob("Copying", "Couldn't export the file", null, cancellable = true, reload = false) { _, _ ->
            try {
                val out = app.contentResolver.openOutputStream(to, "wt") ?: return@runJob "Couldn't write there."
                out.use { o -> fat { it.read(it.find(SdCard.joined(at, entry.name)) ?: throw IOException("Not on the card."), o) } }
                ui { note = "${entry.name} exported" }
                null
            } catch (e: IOException) {
                e.message ?: "Couldn't write there."
            }
        }
    }

    override fun exportEntries(entries: List<FatImage.Entry>, tree: Uri) {
        val at = path ?: return
        val home = home ?: return
        home.runJob("Copying", "Couldn't copy to the folder", null, cancellable = true, reload = false) { cancelled, progress ->
            val bar = percent(progress)
            try {
                fat { fat ->
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

    // Changes are already on the card, so leaving only closes it.
    override fun leaveFiles() = close()

    override fun insertEdited() = close()

    // Closes the card file and lets go of the machine's lock.
    fun close() {
        val held = source
        val opened = cardFile
        source = null
        card = null
        cardFile = null
        ui { path = null; changed = false }
        HomeWork.scope.launch(Dispatchers.IO) {
            try { opened?.let { synchronized(it) { it.close() } } } catch (_: Exception) { }
            held?.close()
        }
    }

    companion object {
        private val SHORTCUTS = listOf("/PALM/Launcher", "/DCIM")

        // The process's file view of a card from Cards, made on first use.
        @Volatile var current: CardImageFiles? = null
            private set

        fun shared(context: Context, sessions: File): CardImageFiles = synchronized(this) {
            current ?: CardImageFiles(context.applicationContext, sessions).also { current = it }
        }
    }
}
