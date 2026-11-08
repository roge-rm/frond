package com.rm.frond

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.concurrent.CompletableFuture

// A name clash waiting for the user, asked from a worker thread and answered from the screen.
class ClashPrompt {
    class Ask(val name: String, val several: Boolean)

    var asking by mutableStateOf<Ask?>(null)
        private set
    private var future: CompletableFuture<CardFiles.Decision>? = null

    // Blocks the worker until answer() is called. `onUi` runs the state change on the main thread.
    fun ask(name: String, several: Boolean, onUi: (() -> Unit) -> Unit): CardFiles.Decision {
        val f = CompletableFuture<CardFiles.Decision>()
        future = f
        onUi { asking = Ask(name, several) }
        return f.get()
    }

    fun answer(decision: CardFiles.Decision) {
        asking = null
        future?.complete(decision)
        future = null
    }
}

// What the file view shows and does, for the running machine's card and for a card opened from Cards.
interface FilesModel {
    val title: String
    val memoryStick: Boolean
    // False for a write-protected card.
    val canWrite: Boolean
    val path: String?
    val entries: List<FatImage.Entry>
    val free: Long
    val total: Long
    val changed: Boolean
    val busy: String?
    // Folders worth a shortcut, from /PALM/Launcher and /DCIM, when the card has them.
    val shortcuts: List<String>
    // What the button that leaves the view says.
    val leaveLabel: String
    val prompt: ClashPrompt

    fun open(at: String)
    fun up()
    fun leaveFiles()
    fun insertEdited()
    fun addFiles(uris: List<Uri>)
    fun addFolder(tree: Uri)
    fun makeFolder(name: String)
    fun renameEntry(entry: FatImage.Entry, newName: String)
    fun deleteEntries(entries: List<FatImage.Entry>)
    fun paste(sources: List<String>, move: Boolean)
    fun exportFile(entry: FatImage.Entry, to: Uri)
    fun exportEntries(entries: List<FatImage.Entry>, tree: Uri)
}

object FileList {
    enum class Sort(val label: String) { NAME("Name"), SIZE("Size"), DATE("Date") }

    // Folders first, then by the chosen key; size and date put the biggest and newest first.
    fun sorted(entries: List<FatImage.Entry>, sort: Sort): List<FatImage.Entry> = entries.sortedWith(
        compareBy<FatImage.Entry> { !it.directory }.let {
            when (sort) {
                Sort.NAME -> it.thenBy { e -> e.name.lowercase() }
                Sort.SIZE -> it.thenByDescending { e -> if (e.directory) 0L else e.size }.thenBy { e -> e.name.lowercase() }
                Sort.DATE -> it.thenByDescending { e -> e.modified }.thenBy { e -> e.name.lowercase() }
            }
        })
}
