package com.rm.frond

// The core's network modes (network-mode and network-state), in the desktop's order and words.
// The TX has them all; other devices only Disabled.
enum class NetworkMode(val id: String, val label: String) {
    DISABLED("disabled", "Disabled"),
    WIRELESS("wireless", "Virtual Wi-Fi (Frond)"),
    INTERNET("internet", "Internet via host"),
    CONNECTED("connected", "Simulated echo server"),
    TIMEOUT("timeout", "Simulated connection timeout"),
    DISCONNECTED("disconnected", "Simulated disconnection");

    companion object {
        fun byId(id: String?) = entries.firstOrNull { it.id == id }
    }
}

// Generated inventory values come from deviceProfiles5. IDs are saved in sessions and settings.
data class Device internal constructor(
    val id: String,
    val label: String,
    val norSize: Long,
    val nandSize: Long,
    val nandRequired: Boolean,
    val fastAudioNorMd5: String?,
    val sound: Boolean,
    val storageSlot: String,
    val sdSlot: Boolean,
    val network: Boolean,
    val serialPort: Boolean,
    val camera: Boolean,
    val keyboard: Boolean, val usb: Boolean, val infrared: Boolean, val bluetooth: Boolean,
    val detectKind: Int, val companyId: Long, val halId: Long,
    val norLayout: Int, val headerOffset: Long, val bigCardOffset: Long,
    val width: Int, val height: Int, val silkscreenHeight: Int,
    internal val appNamesText: String, internal val buttonsText: String,
    val homeX: Int, val homeY: Int, val menuX: Int, val menuY: Int,
    val rotatingStatusBar: Boolean,
    val sdMin: Long, val sdMax: Long,
    val sideTabWidth: Int = 0,
    val homeKey: Int = 0,
    val holdPresent: Boolean = false, val holdSupported: Boolean = false,
    val memoryStick: Boolean = false,
    val microphone: Boolean = false,
) {
    val cardSupported: Boolean get() = sdSlot || memoryStick
    val cardLabel: String get() = if (storageSlot == "memory-stick") "Memory Stick" else "SD card"
    val hasNand: Boolean get() = nandSize > 0
    val fastAudio: Boolean get() = fastAudioNorMd5 != null
    val coreFlags: List<String> get() = listOf("--device", id)
    val appNames: List<String> get() = appNamesText.split('|')
    val buttons: List<String> get() = buttonsText.split('|')

    companion object {
        val entries: List<Device> get() = deviceInventory
        val shown: List<Device> get() = entries
        fun byId(id: String?) = entries.firstOrNull { it.id == id }
        // Compatibility names for callers and saved-setting migrations.
        val TX: Device get() = byId("palm-tx")!!
        val E2: Device get() = byId("palm-e2")!!
        val ZIRE72: Device get() = byId("palm-zire-72")!!
    }
}
