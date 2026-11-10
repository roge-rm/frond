package com.rm.frond

import android.app.Activity
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.print.PrintJobInfo
import android.print.PrintManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File
import java.io.IOException

// One source of print jobs while a machine runs: the core's job is polled once a second. When bytes stop
// arriving for PrintJob.PAUSE_MS the core saves the job to a private temp file, which is appended to the held
// file, so more printing adds to the same job until it is printed, saved or discarded. Held bytes stay in
// private storage and go on Discard, after a raw save and at the next machine start. command is the core's
// printer command (printer or irprinter), folder the private folder under files/ and printerName what the
// notification calls it (USB printer or infrared printer). source names this printer in the texts when another one
// could also have sent the job, and is null otherwise. notices holds the one notification for every printer.
class PrintJobs(
    private val host: ModelHost,
    private val device: Device,
    private val link: ControlLink,
    private val command: String,
    folder: String,
    val printerName: String,
    private val notices: PrintNotices,
    private val inBackground: () -> Boolean,
    private val source: () -> String?,
    private val problem: (String) -> Unit,
) {
    class Held(val bytes: Long, val pages: PrintJob.Pages, val version: Int, val source: String?, val printer: String) {
        val title get() = "Print received" + (source?.let { " from the $it" } ?: "") + " ($bytes bytes)"
        // The title when it's listed with another printer's job.
        val namedTitle get() = "Print received from the $printer ($bytes bytes)"
    }

    var bytes by mutableStateOf(0L)
        private set
    var held by mutableStateOf<Held?>(null)
        private set
    // The choices sheet is open.
    var choosing by mutableStateOf(false)

    private val store = PrintStore(File(host.applicationContext.filesDir, folder)) { argument ->
        link.request(command, argument, timeoutSeconds = if (argument == "status") 5 else 30)
    }
    private var lastError = ""

    init {
        store.sweep()
        notices.add(this)
    }

    // Polls once a second for as long as the machine runs, while active() says the core is printing here.
    fun startPolling(active: () -> Boolean) {
        host.lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                if (active()) poll()
                delay(1000)
            }
        }
    }

    fun attach(on: Boolean): String? = store.attach(on)

    fun resetBytes() { bytes = 0 }

    // True when output is held or still arriving.
    val unsaved get() = held != null || bytes > 0

    // Throws the output away, after the core stopped printing.
    fun dropAll() {
        host.lifecycleScope.launch(Dispatchers.IO) {
            store.dropLocal(::publish)
            host.runOnUiThread { bytes = 0 }
        }
    }

    // Updates the held state in the order the store changed the file.
    private fun publish(size: Long, version: Int) {
        val h = if (size > 0) Held(size, if (size > MAX_RENDER) PrintJob.Pages(false, PrintJob.NOT_TEXT, emptyList())
            else PrintJob.render(store.heldBytes()), version, source(), printerName) else null
        host.runOnUiThread {
            held = h
            if (h == null) choosing = false
            notices.changed(h != null && inBackground())
        }
    }

    suspend fun poll() {
        val result = try {
            store.poll(::publish)
        } catch (e: java.io.IOException) {
            PrintStore.Poll.Failed(e.message ?: "Couldn't store the print.")
        }
        when (result) {
            is PrintStore.Poll.Bytes -> host.runOnUiThread { bytes = result.bytes }
            is PrintStore.Poll.Added -> { lastError = ""; host.runOnUiThread { bytes = result.bytes } }
            is PrintStore.Poll.Failed -> if (result.message != lastError) {
                lastError = result.message
                host.runOnUiThread { problem(result.message) }
            }
            PrintStore.Poll.Idle -> { }
        }
    }

    // Draws the held pages through the system print dialog, which also saves as PDF, on Letter paper unless
    // the dialog changes it. The job is dropped once the system print job went ahead; the preview writes the
    // pages too, so a dialog closed without printing keeps it.
    fun print(activity: Activity) {
        val h = held ?: return
        if (!h.pages.renderable) return
        val manager = activity.getSystemService(Context.PRINT_SERVICE) as PrintManager
        val name = "${device.label} print"
        var job: android.print.PrintJob? = null
        job = manager.print(name, PagesAdapter(activity, name, h.pages.pages) { wrote ->
            if (wrote && wentAhead(job?.info?.state)) host.lifecycleScope.launch(Dispatchers.IO) { store.dropIfVersion(h.version, ::publish) }
        }, PagesAdapter.letter())
    }

    // Copies the held job to the chosen file, then deletes it. A failed copy deletes the new file and keeps the job.
    fun saveRaw(uri: Uri?) {
        if (uri == null || held == null) return
        host.lifecycleScope.launch(Dispatchers.IO) {
            val error = store.saveRaw(::publish) { file ->
                try {
                    val out = host.contentResolver.openOutputStream(uri, "wt") ?: throw IOException("Couldn't open the file.")
                    val copied = out.use { o -> val n = file.inputStream().use { it.copyTo(o) }; o.flush(); n }
                    if (copied != file.length()) throw IOException("The file was not fully written.")
                } catch (e: Exception) {
                    try { DocumentsContract.deleteDocument(host.contentResolver, uri) } catch (_: Exception) { }
                    throw e
                }
            }
            if (error != null) host.runOnUiThread { problem(error) }
        }
    }

    fun discard() {
        host.lifecycleScope.launch(Dispatchers.IO) {
            val error = store.discard(::publish)
            if (error != null) host.runOnUiThread { problem(error) }
        }
    }

    fun close() {
        notices.remove(this)
        store.sweep()
    }

    companion object {
        // A bigger job is held for Save raw only.
        const val MAX_RENDER = 8L shl 20

        // Whether a system print job in this PrintJobInfo state was printed or saved, or is on its way.
        fun wentAhead(state: Int?): Boolean = when (state) {
            PrintJobInfo.STATE_QUEUED, PrintJobInfo.STATE_STARTED, PrintJobInfo.STATE_BLOCKED, PrintJobInfo.STATE_COMPLETED -> true
            else -> false
        }
    }
}
