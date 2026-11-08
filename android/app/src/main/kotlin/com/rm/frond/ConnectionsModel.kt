package com.rm.frond

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

// The Connections screen's state, from the link engine's events while the screen shows. Ops go to the
// engine as the desktop's Connections window sends them. UI thread only.
class ConnectionsModel(private val context: Context) {
    data class Nearby(val id: String, val name: String, val address: String)

    // A pairing this side started: the peer's name, whether it answered, and the last problem.
    data class PairOut(val name: String, val answered: Boolean = false, val problem: String? = null,
                       val working: Boolean = false, val canRetry: Boolean = true)

    var name by mutableStateOf("")
        private set
    var loaded by mutableStateOf(false)
        private set
    var rows by mutableStateOf(emptyList<LinkWire.ConnectionRow>())
        private set
    var nearby by mutableStateOf(emptyList<Nearby>())
        private set
    var pairing by mutableStateOf<PairOut?>(null)
        private set
    // Someone pairs with us: their name and the code to show.
    var request by mutableStateOf<Pair<String, String>?>(null)
        private set
    var notice by mutableStateOf<String?>(null)

    private val main = Handler(Looper.getMainLooper())
    private var connection: LinkConnection? = null

    fun open() {
        if (connection != null) return
        val c = Links.open(context, preferHost = false) { event -> main.post { if (connection != null) handle(event) } }
        connection = c
        c.setSticky("watch", JSONObject().put("op", "watch").put("on", true))
    }

    fun close() {
        val c = connection ?: return
        connection = null
        if (request != null) c.send(JSONObject().put("op", "pair-dismiss"))
        if (pairing != null) c.send(JSONObject().put("op", "pair-cancel"))
        c.send(JSONObject().put("op", "watch").put("on", false))
        c.close()
        request = null
        pairing = null
    }

    private fun send(op: JSONObject) = connection?.send(op)

    fun rename(to: String) = to.trim().takeIf { it.isNotEmpty() }?.let { send(JSONObject().put("op", "rename").put("name", it)) }
    fun renamePeer(id: String, to: String) =
        to.trim().takeIf { it.isNotEmpty() }?.let { send(JSONObject().put("op", "peer-rename").put("id", id).put("name", it)) }
    fun setAddress(id: String, address: String) = send(JSONObject().put("op", "peer-address").put("id", id).put("address", address.trim()))
    fun forget(id: String) = send(JSONObject().put("op", "forget").put("id", id))
    // here or there: whose radio Bluetooth uses with this connection.
    fun setRadio(id: String, radio: String) = send(JSONObject().put("op", "peer-radio").put("id", id).put("radio", radio))

    fun pairNearby(n: Nearby) {
        pairing = PairOut(n.name)
        send(JSONObject().put("op", "pair").put("id", n.id))
    }

    fun pairAddress(address: String) {
        val a = address.trim()
        if (a.isEmpty()) return
        pairing = PairOut(a)
        send(JSONObject().put("op", "pair").put("address", a))
    }

    fun enterCode(code: String) {
        pairing = pairing?.copy(problem = null, working = true)
        send(JSONObject().put("op", "pair-code").put("code", code.filter { it.isDigit() }))
    }

    fun cancelPairing() {
        pairing = null
        send(JSONObject().put("op", "pair-cancel"))
    }

    fun dismissRequest() {
        request = null
        send(JSONObject().put("op", "pair-dismiss"))
    }

    private fun handle(e: JSONObject) {
        when (e.optString("type")) {
            "state" -> {
                name = e.optString("name")
                loaded = true
                rows = parseRows(e)
                val near = e.optJSONArray("nearby")
                nearby = (0 until (near?.length() ?: 0)).mapNotNull { near!!.optJSONObject(it) }
                    .map { Nearby(it.optString("id"), it.optString("name"), it.optString("address")) }
                    .sortedBy { it.name.lowercase() }
            }
            "hello" -> if (name.isEmpty()) name = e.optString("name")
            "pair-peer" -> pairing = pairing?.copy(name = e.optString("name", pairing?.name ?: ""), answered = true)
            "pair-result" -> {
                val p = pairing ?: return
                val result = e.optString("result")
                val who = e.optString("name").ifEmpty { p.name }
                if (result == "paired") {
                    pairing = null
                    notice = "Paired with $who"
                } else {
                    pairing = p.copy(problem = LinkWire.pairResultText(result, who), working = false,
                        canRetry = result == "wrongcode" || result == "failed" || result == "timeout")
                }
            }
            "pair-request" -> request = e.optString("name") to e.optString("code")
            "pair-request-end" -> {
                val who = e.optString("name")
                request = null
                if (e.optString("result") == "paired") notice = "Paired with $who"
            }
        }
    }

    companion object {
        fun parseRows(e: JSONObject): List<LinkWire.ConnectionRow> {
            val list = e.optJSONArray("connections") ?: return emptyList()
            return (0 until list.length()).mapNotNull { list.optJSONObject(it) }.map {
                LinkWire.ConnectionRow(
                    id = it.optString("id"), name = it.optString("name"), peerName = it.optString("peerName"),
                    address = it.optString("address"), up = it.optBoolean("up"), rtt = it.optLong("rtt"),
                    keyChanged = it.optBoolean("keyChanged"), forgotten = it.optBoolean("forgotten"),
                    seen = it.optString("seen"), channels = it.optString("channels"),
                    bluetoothRadio = if (it.optString("bluetoothRadio") == "there") "there" else "here",
                )
            }
        }
    }
}
