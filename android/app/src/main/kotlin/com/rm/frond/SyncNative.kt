package com.rm.frond

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException

internal object SyncNative {
    init { System.loadLibrary("frondsync") }

    private external fun exchange(root: ByteArray, request: ByteArray): ByteArray

    fun root(context: Context) = File(context.filesDir, "sync")
    fun deviceKey(id: String) = "machine:$id"

    fun call(context: Context, operation: String, vararg args: Pair<String, Any>): JSONObject {
        val request = JSONObject().put("op", operation)
        args.forEach { (key, value) -> request.put(key, value) }
        val response = JSONObject(exchange(root(context).path.toByteArray(), request.toString().toByteArray()).decodeToString())
        if (response.has("error")) throw IOException(response.getString("error"))
        return response
    }
}

internal fun JSONObject.rows(): List<JSONObject> = optJSONArray("rows")?.objects().orEmpty()
internal fun JSONArray.objects() = (0 until length()).map(::getJSONObject)

internal enum class SyncProtocol(val label: String, val startCommand: String) {
    HOT_SYNC("HotSync", "sync-start"),
    ACTIVE_SYNC("ActiveSync", "activesync-start");

    companion object {
        fun forDevice(device: Device): SyncProtocol? = when {
            device.palmOs && device.hotSyncButton && (device.serialPort || device.usb) -> HOT_SYNC
            device.rx1950 && device.usb -> ACTIVE_SYNC
            else -> null
        }
    }
}

internal fun Device.supportsNetworkHotSync() =
    SyncProtocol.forDevice(this) == SyncProtocol.HOT_SYNC &&
        (id == "palm-tx" || modem || irPhone || usbWifi || builtInWifi)

internal enum class SyncKind(val wire: String, val label: String) {
    CONTACTS("contacts", "Contacts"),
    CALENDAR("calendar", "Calendar"),
    TASKS("tasks", "Tasks"),
    MEMOS("memos", "Memos")
}
