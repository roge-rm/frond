package com.rm.frond

import org.json.JSONObject

// The Dana and the Dana Wireless share one board, ROM, art and keyboard.
fun Device.isDana() = id == "alphasmart-dana" || id == "alphasmart-dana-wireless"

enum class DanaKeyboard(val label: String, val arg: String) {
    AUTO("Automatic", "auto"), COMPACT("Compact", "compact"), FULL("Full Dana", "full"), HIDDEN("Hidden", "hidden"),
}

enum class DanaZoom(val label: String, val arg: String) {
    FIT("Fit", "fit"), ONE("1×", "1"), ONE_HALF("1.5×", "1.5"), TWO("2×", "2"), THREE("3×", "3"),
}

enum class DanaPointer(val label: String, val arg: String) {
    TOUCH("Touch", "touch"), PAN("Pan", "pan"),
}

// Only user choices belong in the session. Density and attached keyboards are live host state.
// Zoom is guest pixels per dp, so 1x is already 560 dp wide; Fit is the default so the wide
// screen fits a phone's width, centred.
data class DanaView(
    val keyboard: DanaKeyboard = DanaKeyboard.AUTO,
    val zoom: DanaZoom = DanaZoom.FIT,
    val panX: Double = 0.0,
    val panY: Double = 0.0,
    val pointer: DanaPointer = DanaPointer.TOUCH,
) {
    fun argument() = "keyboard=${keyboard.arg} zoom=${zoom.arg} pan-x=$panX pan-y=$panY pointer=${pointer.arg}"

    fun json(): JSONObject = JSONObject().put("keyboard", keyboard.arg).put("zoom", zoom.arg)
        .put("panX", panX).put("panY", panY).put("pointer", pointer.arg)

    companion object {
        fun from(json: JSONObject?, fallback: DanaView = DanaView()): DanaView {
            if (json == null) return fallback
            return DanaView(
                DanaKeyboard.entries.firstOrNull { it.arg == json.optString("keyboard") } ?: fallback.keyboard,
                DanaZoom.entries.firstOrNull { it.arg == json.optString("zoom") } ?: fallback.zoom,
                pan(json.optDouble("panX", fallback.panX), 560.0) ?: fallback.panX,
                pan(json.optDouble("panY", fallback.panY), 160.0) ?: fallback.panY,
                DanaPointer.entries.firstOrNull { it.arg == json.optString("pointer") } ?: fallback.pointer,
            )
        }

        internal fun pan(value: Double, maximum: Double): Double? = value.takeIf { it.isFinite() && it in 0.0..maximum }
    }
}

data class DanaViewState(
    val view: DanaView,
    val resolved: DanaKeyboard,
    val density: Double,
    val physical: Boolean,
    val page: String,
) {
    fun toggledKeyboard(previous: DanaKeyboard): DanaKeyboard =
        if (resolved != DanaKeyboard.HIDDEN) DanaKeyboard.HIDDEN
        else if (previous == DanaKeyboard.AUTO && physical) DanaKeyboard.COMPACT else previous

    companion object {
        // Queries and notifications have the same prefix. Reject incomplete or repeated fields.
        fun parse(message: String): DanaViewState? {
            if (!message.startsWith("dana-view:")) return null
            val fields = mutableMapOf<String, String>()
            for (token in message.removePrefix("dana-view:").trim().split(Regex("\\s+"))) {
                val pair = token.split('=', limit = 2)
                if (pair.size != 2 || pair[0].isEmpty() || pair[1].isEmpty() || fields.put(pair[0], pair[1]) != null) return null
            }
            val keyboard = DanaKeyboard.entries.firstOrNull { it.arg == fields["keyboard"] } ?: return null
            val resolved = DanaKeyboard.entries.firstOrNull { it.arg == fields["resolved"] && it != DanaKeyboard.AUTO } ?: return null
            val zoom = DanaZoom.entries.firstOrNull { it.arg == fields["zoom"] } ?: return null
            val panX = fields["pan-x"]?.toDoubleOrNull()?.let { DanaView.pan(it, 560.0) } ?: return null
            val panY = fields["pan-y"]?.toDoubleOrNull()?.let { DanaView.pan(it, 160.0) } ?: return null
            val pointer = DanaPointer.entries.firstOrNull { it.arg == fields["pointer"] } ?: return null
            val density = fields["density"]?.toDoubleOrNull()?.takeIf { it.isFinite() && it > 0 } ?: return null
            val physical = when (fields["physical"]) { "on" -> true; "off" -> false; else -> return null }
            val page = fields["page"]?.takeIf { it in setOf("abc", "symbols", "more", "fn") } ?: return null
            return DanaViewState(DanaView(keyboard, zoom, panX, panY, pointer), resolved, density, physical, page)
        }
    }
}

// InputDevice snapshots keep hardware detection testable without an Android runtime.
internal data class DanaKeyboardDevice(val virtual: Boolean, val alphabetic: Boolean, val enabled: Boolean)
internal fun hasDanaHardwareKeyboard(devices: List<DanaKeyboardDevice>) = devices.any { !it.virtual && it.alphabetic && it.enabled }
internal fun danaEnvironment(density: Float, physical: Boolean) = "density=$density physical=${if (physical) "on" else "off"}"
