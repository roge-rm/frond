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
    val msSizes: List<Int> = emptyList(),
    val msProMax: Int = 0,
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
    // New SD card sizes in MiB, 32 MB to the device's largest card. "2 GB" is 1886 MiB like a real 2 GB card.
    val sdSizes: List<Int> get() = if (!sdSlot) emptyList() else SD_SIZES.filter { (it.toLong() shl 20) in sdMin..sdMax }
    // New Memory Stick sizes in MB, 128 MB first, then the Memory Stick Pro sizes the device takes.
    val memoryStickSizes: List<Int> get() = if (!memoryStick) emptyList()
        else msSizes.sortedDescending() + MEMORY_STICK_PRO_SIZES.filter { it <= msProMax }
    // The largest SD image the device takes.
    val sdImageMax: Long get() = minOf(sdMax, FatImage.MAX_BYTES)
    val appNames: List<String> get() = appNamesText.split('|')
    val buttons: List<String> get() = buttonsText.split('|')

    companion object {
        val SD_SIZES = listOf(32, 64, 128, 256, 512, 1024, 1886)
        fun sizeLabel(mib: Int) = if (mib >= 1024) "${(mib + 512) / 1024} GB" else "$mib MB"
        fun sizeLabel(bytes: Long) = if (bytes >= 1L shl 30) "${bytes shr 30} GB" else "${bytes shr 20} MB"
        // Classic Memory Stick physical bytes: each MB is 2048 pages of 512 + 9 bytes.
        fun memoryStickPhysicalBytes(mb: Int) = mb.toLong() * 2048 * 521
        val MEMORY_STICK_SIZES = listOf(4, 8, 16, 32, 64, 128)
        // Memory Stick Pro sizes in MiB; "2 GB" is 1886 MiB like a real 2 GB card.
        val MEMORY_STICK_PRO_SIZES = listOf(256, 512, 1024, 1886)
        val entries: List<Device> get() = deviceInventory
        val shown: List<Device> get() = entries
        fun byId(id: String?) = entries.firstOrNull { it.id == id }
        // Compatibility names for callers and saved-setting migrations.
        val TX: Device get() = byId("palm-tx")!!
        val E2: Device get() = byId("palm-e2")!!
        val ZIRE72: Device get() = byId("palm-zire-72")!!
    }
}
