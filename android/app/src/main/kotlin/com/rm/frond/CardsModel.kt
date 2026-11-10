package com.rm.frond

import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

// What the Cards screen shows. Card work runs through the home screen's job, so it shows the same
// progress dialog with Cancel and survives a recreated activity.
class CardsModel(private val activity: HomeActivity, private val home: HomeModel) {
    private val app = activity.applicationContext
    val sessions: File get() = Sessions.folder(app)
    val library: File get() = Cards.libraryFor(sessions)

    var rows by mutableStateOf<List<CardChoices.Row>?>(null)
        private set
    // The machine with an SD exchange open or pending; its card's menu is greyed.
    var exchanging by mutableStateOf<File?>(null)
        private set
    // The Link dialog's card and machines, once read.
    var linking by mutableStateOf<Pair<Cards.Card, List<CardChoices.LinkOption>>?>(null)
    // A picked folder, measured, waiting for the card's name, type and size.
    class FolderDraft(val root: FolderCard.Node, val plan: FolderCard.Plan)
    var folder by mutableStateOf<FolderDraft?>(null)

    // An image that fits more than one type, waiting for the type.
    var importing by mutableStateOf<Pair<File, List<String>>?>(null)

    // The file view of a card opened with Files…; open while its path is set.
    // One per process, so it outlives a recreated home screen.
    val files: CardImageFiles get() = CardImageFiles.shared(app, sessions)

    // Reconciles first, as the plan asks whenever Cards opens.
    fun open() {
        activity.lifecycleScope.launch {
            withContext(Dispatchers.IO) { Cards.reconcile(sessions) }?.let { home.alert = "Cards" to it }
            reload()
        }
    }

    fun reload() {
        activity.lifecycleScope.launch {
            val (r, e) = withContext(Dispatchers.IO) { CardChoices.rows(sessions) to SdCard.pendingMachine(app) }
            rows = r
            exchanging = e
        }
    }

    // Whether the row's card is in a machine with an SD exchange open or pending.
    fun exchangeOpen(row: CardChoices.Row): Boolean =
        row.holderDir != null && exchanging?.canonicalFile == row.holderDir.canonicalFile

    fun make(name: String, kind: String, size: Int) =
        home.runJob("Making the card", "Couldn't make the card", null, cancellable = true) { cancelled, progress ->
            Cards.makeCard(sessions, name, kind, size, cancelled, progress)
        }

    // Measures the picked folder tree, then opens the Import folder dialog.
    fun startFolder(tree: Uri) {
        activity.lifecycleScope.launch {
            val draft = withContext(Dispatchers.IO) {
                try {
                    val root = FolderCard.Document.root(app.contentResolver, tree)
                    FolderDraft(root, FolderCard.measure(root))
                } catch (e: Exception) {
                    null
                }
            }
            val sizes = FolderCard.kinds().flatMap { kind -> CardChoices.sizesFor(kind) }
            when {
                draft == null -> home.alert = "Import folder as card" to "Couldn't read the folder."
                draft.plan.problem != null -> home.alert = "Import folder as card" to draft.plan.problem
                sizes.none { FolderCard.fits(draft.plan, it) } -> home.alert = "Import folder as card" to "The folder is too big for a card."
                else -> folder = draft
            }
        }
    }

    fun makeFromFolder(draft: FolderDraft, name: String, kind: String, size: Int) =
        home.runJob("Building the card", "Couldn't build the card", null, cancellable = true) { cancelled, progress ->
            Cards.importFolderCard(sessions, draft.root, name, kind, size, cancelled, progress)
        }

    fun rename(row: CardChoices.Row, name: String) {
        activity.lifecycleScope.launch {
            withContext(Dispatchers.IO) { Cards.renameCard(sessions, row.card.id, name) }?.let { home.alert = "Couldn't rename the card" to it }
            reload()
        }
    }

    fun duplicate(row: CardChoices.Row) =
        home.runJob("Copying the card", "Couldn't copy the card", null, cancellable = true) { cancelled, progress ->
            Cards.duplicateCard(sessions, row.card.id, cancelled, progress)
        }

    fun takeOut(row: CardChoices.Row) =
        home.runJob("Taking the card out", "Couldn't take the card out", null, cancellable = true) { cancelled, progress ->
            Cards.takeOutStopped(sessions, row.card.id, cancelled, progress)
        }

    fun delete(row: CardChoices.Row) {
        activity.lifecycleScope.launch {
            withContext(Dispatchers.IO) { Cards.deleteCard(sessions, row.card.id) }?.let { home.alert = "Couldn't delete the card" to it }
            reload()
        }
    }

    fun startLink(row: CardChoices.Row) {
        activity.lifecycleScope.launch {
            linking = row.card to withContext(Dispatchers.IO) { CardChoices.linkOptions(sessions, row.card.id) }
        }
    }

    fun link(card: Cards.Card, machines: List<File>) {
        linking = null
        activity.lifecycleScope.launch {
            val moved = mutableListOf<String>()
            val error = withContext(Dispatchers.IO) { Cards.setLinks(sessions, card.id, machines, null, moved) }
            if (error != null) home.alert = "Couldn't link the card" to error
            else if (moved.isNotEmpty()) home.notice = "${moved.joinToString(", ")} now linked to ${card.name}"
            reload()
        }
    }

    // The card as an image. An SD or Pro card goes straight into the document through its file
    // descriptor. A classic stick (4 to 128 MB) and a document that isn't a plain file go through a
    // private copy.
    fun export(row: CardChoices.Row, to: Uri) =
        home.runJob("Exporting the image", "Couldn't export the image", "Card image exported", cancellable = true) { cancelled, progress ->
            val direct = if (row.card.kind == "ms") null else openRegular(to, "wt")
            if (direct != null) {
                val error = direct.use { Cards.exportCardImageFd(sessions, row.card.id, it.fd, cancelled, progress) }
                if (error != null) removeDocument(to)
                return@runJob error
            }
            val temp = File(app.cacheDir, "card-export.img")
            try {
                temp.delete()
                Cards.exportCardImage(sessions, row.card.id, temp, cancelled, progress)?.let { return@runJob it }
                try {
                    app.contentResolver.openOutputStream(to, "wt")?.use { out ->
                        temp.inputStream().use { input ->
                            val buffer = ByteArray(1 shl 20)
                            while (true) {
                                if (cancelled()) return@runJob COPY_CANCELLED
                                val n = input.read(buffer)
                                if (n < 0) break
                                out.write(buffer, 0, n)
                            }
                        }
                    } ?: return@runJob "Couldn't write the image."
                } catch (e: IOException) {
                    return@runJob "Couldn't write the image."
                }
                null
            } finally {
                temp.delete()
            }
        }

    // The picked image is read in place through its file descriptor (/proc/self/fd/N), so a 2 GB
    // card needs no second copy. A document that isn't a plain file is copied to a private file first.
    // Asks for the type when SD and Pro both fit.
    fun importImage(from: Uri) =
        home.runJob("Reading the image", "Couldn't import the image", null, cancellable = true) { cancelled, progress ->
            val base = (displayName(from) ?: "").substringBeforeLast('.')
            val direct = openRegular(from, "r")
            if (direct != null) {
                val size = direct.statSize
                val head = ByteArray(8)
                if (size < 0 || size > (1886L shl 20) + 4096) {
                    direct.close()
                    return@runJob "No device here takes a card like that."
                }
                if (!readHead(direct, head)) { direct.close(); return@runJob "Couldn't read the image." }
                val kinds = CardChoices.imageKinds(head, size)
                if (kinds.isEmpty()) { direct.close(); return@runJob "No device here takes a card like that." }
                val image = File("/proc/self/fd/${direct.fd}")
                if (kinds.size > 1) {
                    activity.runOnUiThread { importing = image to kinds; pendingName = base; pendingFd = direct }
                    return@runJob null
                }
                return@runJob direct.use { Cards.importCard(sessions, image, kinds.first(), base, cancelled, progress) }
            }
            copyAndImport(from, base, cancelled, progress)
        }

    private var pendingFd: ParcelFileDescriptor? = null

    private fun copyAndImport(from: Uri, base: String, cancelled: () -> Boolean, progress: (Long, Long) -> Unit): String? {
        val temp = File(app.cacheDir, "card-import.img")
        temp.delete()
        val size = sizeOf(from)
        if (size != null) {
            val free = Cards.freeSpace(app.cacheDir)
            if (free >= 0 && free < 2 * size + Cards.SPARE_BYTES) return Cards.spaceMessage(2 * size)
        }
        try {
            app.contentResolver.openInputStream(from)?.use { input ->
                temp.outputStream().use { out ->
                    val buffer = ByteArray(1 shl 20)
                    var done = 0L
                    while (true) {
                        if (cancelled()) { temp.delete(); return COPY_CANCELLED }
                        val n = input.read(buffer)
                        if (n < 0) break
                        out.write(buffer, 0, n)
                        done += n
                        // Bigger than any card: not one.
                        if (done > (1886L shl 20) + 4096) { temp.delete(); return "No device here takes a card like that." }
                        if (size != null) progress(done, size)
                    }
                }
            } ?: return "Couldn't read the image."
        } catch (e: IOException) {
            temp.delete()
            return "Couldn't read the image."
        }
        val head = ByteArray(8)
        temp.inputStream().use { it.read(head) }
        val kinds = CardChoices.imageKinds(head, temp.length())
        if (kinds.isEmpty()) { temp.delete(); return "No device here takes a card like that." }
        if (kinds.size > 1) {
            activity.runOnUiThread { importing = temp to kinds; pendingName = base }
            return null
        }
        return finishImport(temp, kinds.first(), base, cancelled, progress)
    }

    // Opens the document when it's a plain file the card tool can reach as /proc/self/fd/N.
    private fun openRegular(uri: Uri, mode: String): ParcelFileDescriptor? = try {
        app.contentResolver.openFileDescriptor(uri, mode)?.let { pfd ->
            val plain = try { OsConstants.S_ISREG(Os.fstat(pfd.fileDescriptor).st_mode) } catch (e: ErrnoException) { false }
            if (plain) pfd else { pfd.close(); null }
        }
    } catch (e: Exception) {
        null
    }

    private fun readHead(pfd: ParcelFileDescriptor, head: ByteArray): Boolean = try {
        var got = 0
        while (got < head.size) {
            val n = Os.pread(pfd.fileDescriptor, head, got, head.size - got, got.toLong())
            if (n <= 0) break
            got += n
        }
        true
    } catch (e: ErrnoException) {
        false
    }

    private fun removeDocument(uri: Uri) {
        try { DocumentsContract.deleteDocument(app.contentResolver, uri) } catch (_: Exception) { }
    }

    private var pendingName = ""

    fun importAs(kind: String?) {
        val (image, _) = importing ?: return
        importing = null
        val fd = pendingFd
        pendingFd = null
        if (kind == null) { fd?.close(); if (fd == null) image.delete(); return }
        val base = pendingName
        home.runJob("Importing the image", "Couldn't import the image", null, cancellable = true) { cancelled, progress ->
            if (fd != null) fd.use { Cards.importCard(sessions, image, kind, base, cancelled, progress) }
            else finishImport(image, kind, base, cancelled, progress)
        }
    }

    private fun finishImport(temp: File, kind: String, base: String, cancelled: () -> Boolean, progress: (Long, Long) -> Unit): String? = try {
        Cards.importCard(sessions, temp, kind, base, cancelled, progress)
    } finally {
        temp.delete()
    }

    private fun sizeOf(uri: Uri): Long? = try {
        app.contentResolver.openFileDescriptor(uri, "r")?.use { it.statSize.takeIf { s -> s >= 0 } }
    } catch (e: Exception) {
        null
    }

    private fun displayName(uri: Uri): String? = try {
        app.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }
}
