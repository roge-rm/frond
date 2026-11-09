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
import android.provider.DocumentsContract
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

// The Dana's USB port while a machine runs: the choice of printer, Wi-Fi adapter or nothing, with the Wi-Fi link
// state polled once a second. For the printer, the core's job is polled once a second,
// and the held job. When bytes stop arriving for PrintJob.PAUSE_MS the core saves the job to a private
// temp file, which is appended to the held file, so more printing adds to the same job until it is printed,
// saved or discarded. Held bytes stay in private storage and go on Discard, after a raw save and at the
// next machine start.
class PrinterModel(
    private val host: ModelHost,
    private val device: Device,
    private val link: ControlLink,
    private val sessionId: String?,
    initialPort: String,
    initialModule: Boolean,
    private val inBackground: () -> Boolean,
    private val problem: (String) -> Unit,
) {
    class Held(val bytes: Long, val pages: PrintJob.Pages, val version: Int)

    var port by mutableStateOf(initialPort)
        private set
    val plugged get() = port == UsbPort.PRINTER
    // The Wi-Fi adapter's link readout, null while it isn't the port's device.
    var wifiText by mutableStateOf<String?>(null)
        private set
    // The Dana Wireless's built-in Wi-Fi module: its radio, and the link readout polled once a second.
    var moduleOn by mutableStateOf(initialModule)
        private set
    var moduleText by mutableStateOf<String?>(null)
        private set
    var bytes by mutableStateOf(0L)
        private set
    var held by mutableStateOf<Held?>(null)
        private set
    // The choices sheet is open.
    var choosing by mutableStateOf(false)

    private val store = PrintStore(File(host.applicationContext.filesDir, "print")) { argument ->
        link.request("printer", argument, timeoutSeconds = if (argument == "status") 5 else 30)
    }
    private var lastError = ""

    init {
        store.sweep()
        host.lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                if (plugged) poll() else if (port == UsbPort.WIFI) pollWifi()
                if (device.builtInWifi) pollModule()
                delay(1000)
            }
        }
    }

    // Updates the held state in the order the store changed the file.
    private fun publish(size: Long, version: Int) {
        val h = if (size > 0) Held(size, if (size > MAX_RENDER) PrintJob.Pages(false, PrintJob.NOT_TEXT, emptyList())
            else PrintJob.render(store.heldBytes()), version) else null
        host.runOnUiThread {
            held = h
            if (h == null) { choosing = false; cancelNotification() }
            else if (inBackground()) notify(size)
        }
    }

    // Switches the port's device. The core keeps what it had when the change fails, and the row shows which that is.
    fun choose(choice: String) {
        if (choice == port) return
        val before = port
        port = choice
        bytes = 0
        wifiText = if (choice == UsbPort.WIFI) PrintJob.wifiReadout(PrintJob.Port(true, UsbPort.WIFI)) else null
        remember(choice)
        host.lifecycleScope.launch(Dispatchers.IO) {
            val error = if (choice == UsbPort.PRINTER) store.attach(true) else {
                val reply = link.request("usb-port", choice, timeoutSeconds = 30)
                if (reply.ok) null else reply.message
            }
            if (error != null) {
                val state = PrintJob.parsePort(link.request("usb-port-state", timeoutSeconds = 5).message)
                val kept = if (state.valid) state.port else before
                host.runOnUiThread { port = kept; bytes = 0; wifiText = null; remember(kept); problem(error) }
            }
        }
    }

    // Takes the core's port as it is after the start request, so the row and settings match the core.
    fun adopt(state: PrintJob.Port) {
        if (!state.valid || state.port == port) return
        port = state.port
        bytes = 0
        wifiText = PrintJob.wifiReadout(state)
        remember(state.port)
    }

    private fun remember(choice: String) {
        sessionId?.let { id ->
            try { Sessions.updateSettings(host.applicationContext, id) { it.copy(usbPort = choice) } } catch (_: Exception) { }
        }
    }

    // Turns the built-in module's radio on or off. A refused change goes back to what the core has.
    fun chooseModule(on: Boolean) {
        val request = UsbPort.moduleRequest(device, on) ?: return
        if (on == moduleOn) return
        val before = moduleOn
        moduleOn = on
        moduleText = PrintJob.moduleReadout("off", on)
        rememberModule(on)
        host.lifecycleScope.launch(Dispatchers.IO) {
            val reply = link.request(request.first, request.second, timeoutSeconds = 30)
            if (!reply.ok) host.runOnUiThread { moduleOn = before; rememberModule(before); problem(reply.message) }
        }
    }

    private fun rememberModule(on: Boolean) {
        sessionId?.let { id ->
            try { Sessions.updateSettings(host.applicationContext, id) { it.copy(wifiModule = on) } } catch (_: Exception) { }
        }
    }

    private fun pollModule() {
        val reply = link.request("wifi-module-state", timeoutSeconds = 5)
        if (reply.ok) host.runOnUiThread { moduleText = PrintJob.moduleReadout(reply.message, moduleOn) }
    }

    private fun pollWifi() {
        val reply = link.request("usb-port-state", timeoutSeconds = 5)
        val state = PrintJob.parsePort(reply.message)
        if (reply.ok && state.port == UsbPort.WIFI) host.runOnUiThread { if (port == UsbPort.WIFI) wifiText = PrintJob.wifiReadout(state) }
    }

    private suspend fun poll() {
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

    // Draws the held pages through the system print dialog, which also saves as PDF. The job is dropped when
    // the pages were written; a cancelled print keeps it.
    fun print(activity: Activity) {
        val h = held ?: return
        if (!h.pages.renderable) return
        val manager = activity.getSystemService(Context.PRINT_SERVICE) as PrintManager
        val name = "Dana print"
        manager.print(name, PagesAdapter(activity, name, h.pages.pages) { wrote ->
            if (wrote) host.lifecycleScope.launch(Dispatchers.IO) { store.dropIfVersion(h.version, ::publish) }
        }, null)
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
        cancelNotification()
        store.sweep()
    }

    private fun notify(size: Long) {
        val context = host.applicationContext
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "Printing", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(
            context, 0,
            Intent(context, Emulators.activity(device)).putExtra(FrondActivity.EXTRA_DEVICE, device.id)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP),
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
