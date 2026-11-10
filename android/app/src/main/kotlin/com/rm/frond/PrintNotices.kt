package com.rm.frond

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build

// The one notification for prints that arrive while the app is in the background, shared by every printer on the
// machine: one held job gets its own text, jobs on more than one printer one "N prints ready". It goes with the last
// held job. Main thread only.
class PrintNotices(private val context: Context, private val device: Device) {
    private val printers = mutableListOf<PrintJobs>()

    fun add(jobs: PrintJobs) { printers += jobs }

    // The machine stops: no notification is left behind.
    fun remove(jobs: PrintJobs) {
        printers -= jobs
        manager().cancel(ID)
    }

    // post: a job just arrived in the background. Otherwise a notification still showing is brought up to date.
    fun changed(post: Boolean) {
        val manager = manager()
        val (title, text) = text(device.label, printers.mapNotNull { p -> p.held?.let { p.printerName to it.bytes } })
            ?: return manager.cancel(ID)
        if (!post && manager.activeNotifications.none { it.id == ID }) return
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "Printing", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(
            context, 0,
            Intent(context, Emulators.activity(device)).putExtra(FrondActivity.EXTRA_DEVICE, device.id)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        manager.notify(ID, Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification).setContentTitle(title).setContentText(text)
            .setContentIntent(open).setAutoCancel(true).build())
    }

    private fun manager() = context.getSystemService(NotificationManager::class.java)

    companion object {
        const val CHANNEL = "print"
        const val ID = 41

        // The notification's title and text for the held jobs, each (printer, bytes); null with none.
        fun text(device: String, held: List<Pair<String, Long>>): Pair<String, String>? = when (held.size) {
            0 -> null
            1 -> "Print received" to "${held[0].second} bytes from the ${held[0].first} on the $device"
            else -> "${held.size} prints ready" to "From the " + held.joinToString(" and the ") { it.first } + " on the $device"
        }

        // The snackbar's text and action for the held jobs, null with none: one job keeps its own notice with
        // Options, more than one get "N prints ready" with Show.
        fun prompt(held: List<PrintJobs.Held>): Pair<String, String>? = when (held.size) {
            0 -> null
            1 -> held[0].title to "Options"
            else -> "${held.size} prints ready" to "Show"
        }
    }
}
