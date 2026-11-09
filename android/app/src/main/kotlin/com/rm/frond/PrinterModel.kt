package com.rm.frond

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.print.PrintManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream

// The Dana's USB printer while a machine runs: the plugged-in setting, the core's job polled once a second,
// and the held job. When bytes stop arriving for PrintJob.PAUSE_MS the core saves the job to a private
// temp file, which is appended to the held file, so more printing adds to the same job until it is printed,
// saved or discarded. Held bytes stay in private storage and go on Discard, after a raw save and at the
// next machine start.
class PrinterModel(
    private val host: ModelHost,
    private val link: ControlLink,
    private val sessionId: String?,
    initiallyPlugged: Boolean,
    private val inBackground: () -> Boolean,
    private val problem: (String) -> Unit,
) {
    class Held(val bytes: Long, val pages: PrintJob.Pages, val version: Int)

    var plugged by mutableStateOf(initiallyPlugged)
        private set
    var bytes by mutableStateOf(0L)
        private set
    var held by mutableStateOf<Held?>(null)
        private set
    // The choices sheet is open.
    var choosing by mutableStateOf(false)

    private val dir = File(host.applicationContext.filesDir, "print")
    private val heldFile = File(dir, "held.raw")
    private var version = 0

    init {
        dir.mkdirs()
        heldFile.delete()
        dir.listFiles()?.forEach { if (it.name.startsWith("job-")) it.delete() }
        host.lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                if (plugged) poll()
                delay(1000)
            }
        }
    }

    fun plug(on: Boolean) {
        plugged = on
        if (!on) bytes = 0
        sessionId?.let { id ->
            try { Sessions.updateSettings(host.applicationContext, id) { it.copy(usbPrinter = on) } } catch (_: Exception) { }
        }
        host.lifecycleScope.launch(Dispatchers.IO) {
            val reply = link.request("printer", "attach " + if (on) "on" else "off", timeoutSeconds = 30)
            if (!reply.ok) host.runOnUiThread { problem(reply.message) }
        }
    }

    private suspend fun poll() {
        val status = PrintJob.parseStatus(link.request("printer", "status", timeoutSeconds = 5).message)
        if (!status.valid) return
        host.runOnUiThread { bytes = status.bytes }
        if (!status.received) return
        val job = File(dir, "job-${++version}.raw")
        val reply = link.request("printer", "save " + job.absolutePath, timeoutSeconds = 30)
        try {
            if (!reply.ok || !job.isFile) {
                host.runOnUiThread { problem(PrintJob.parseStatus(reply.message).error.ifEmpty { reply.message }) }
                return
            }
            FileOutputStream(heldFile, true).use { out ->
                job.inputStream().use { it.copyTo(out) }
                out.fd.sync()
            }
        } finally {
            job.delete()
        }
        val size = heldFile.length()
        val pages = if (size > MAX_RENDER) PrintJob.Pages(false, PrintJob.NOT_TEXT, emptyList())
        else PrintJob.render(heldFile.readBytes())
        host.runOnUiThread {
            held = Held(size, pages, version)
            if (inBackground()) notify(size)
        }
    }

    // Draws the held pages through the system print dialog, which also saves as PDF.
    fun print(activity: Activity) {
        val h = held ?: return
        if (!h.pages.renderable) return
        val manager = activity.getSystemService(Context.PRINT_SERVICE) as PrintManager
        val name = "Dana print"
        manager.print(name, PagesAdapter(activity, name, h.pages.pages), null)
    }

    // Copies the held job to the chosen file, then deletes it.
    fun saveRaw(uri: Uri?) {
        if (uri == null || held == null) return
        host.lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                try {
                    host.contentResolver.openOutputStream(uri, "wt")!!.use { out -> heldFile.inputStream().use { it.copyTo(out) } }
                    null
                } catch (e: Exception) { e.message ?: "Couldn't save the file." }
            }
            if (error != null) problem(error)
            else drop()
        }
    }

    fun discard() {
        host.lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) { link.request("printer", "clear", timeoutSeconds = 30) }
            if (!reply.ok) problem(reply.message)
            drop()
        }
    }

    private fun drop() {
        heldFile.delete()
        held = null
        choosing = false
        cancelNotification()
    }

    fun close() = cancelNotification()

    private fun notify(size: Long) {
        val context = host.applicationContext
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "Printing", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(
            context, 0,
            Intent(context, FrondActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        manager.notify(NOTIFICATION, Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification).setContentTitle("Print received")
            .setContentText("$size bytes from the Dana").setContentIntent(open).setAutoCancel(true).build())
    }

    private fun cancelNotification() =
        host.applicationContext.getSystemService(NotificationManager::class.java).cancel(NOTIFICATION)

    companion object {
        const val CHANNEL = "print"
        const val NOTIFICATION = 41
        // A bigger job is held for Save raw only.
        const val MAX_RENDER = 8L shl 20
    }
}
