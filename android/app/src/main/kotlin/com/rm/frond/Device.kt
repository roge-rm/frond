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

data class FirmwareVariant(
    val id: String, val norSize: Long, val headerOffset: Long,
    val cardName: String, val manufacturer: String,
    val companyId: Long, val halId: Long, val cardEndsAtEof: Boolean,
    val fastAudioNorMd5: String? = null,
    val powerSupported: Boolean = true,
)

data class CardFamily(
    val family: String, val minBytes: Long, val maxBytes: Long, val sizeRule: String,
    val runtimeSizes: List<Long> = emptyList(), val guestQualifiedSizes: List<Long> = emptyList(),
) {
    val label: String get() = if (family == "legacy-mmc-spi") "MMC" else "SD"
    val protocol: String get() = if (family == "legacy-mmc-spi") "mmc" else "sd"
    fun accepts(bytes: Long): Boolean {
        if (bytes <= 0 || bytes % 512 != 0L || bytes !in minBytes..maxBytes ||
            (runtimeSizes.isNotEmpty() && bytes !in runtimeSizes)) return false
        if (sizeRule != "legacy-csd-fixed-512") return true
        val blocks = bytes / 512
        val mult = (0..7).firstOrNull { (blocks shr (it + 2)) < 0x1000 } ?: return false
        return blocks >= 4 && ((blocks shr (mult + 2)) shl (mult + 2)) == blocks
    }
    val sizes: List<Int> get() = Device.SD_SIZES.filter { accepts(it.toLong() shl 20) }
}

data class StorageSlot(val id: Int, val label: Int, val kind: String, val families: List<CardFamily> = emptyList()) {
    val title: String get() = "Card $label"
    val commandPrefix: String get() = when (id) { 0 -> "sd-"; 2 -> "sd2-"; else -> "ms-" }
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
    // A snap-on modem for the serial port, run by the core.
    val modem: Boolean = false,
    // A mobile phone on the infrared port, run by the core.
    val irPhone: Boolean = false,
    // DragonBall devices run in libmain68k.so (Frond68kActivity), the others in libmain.so (FrondActivity).
    val runs68k: Boolean = false,
    val hotSyncButton: Boolean = false,
    val firmwareVariants: List<FirmwareVariant> = emptyList(),
    val storageSlots: List<StorageSlot> = emptyList(),
) {
    // The real Zire 72 has no serial port, so it's left out of the list rather than greyed.
    val serialAbsent: Boolean get() = id == "palm-zire-72"
    val infraredKeyboard: Boolean get() = keyboard && id != "alphasmart-dana"
    val cardSupported: Boolean get() = sdSlot || memoryStick
    val cardLabel: String get() = if (storageSlot == "memory-stick") "Memory Stick" else "SD card"
    val hasNand: Boolean get() = nandSize > 0
    // What the firmware file is called: a 68k device takes one ROM file.
    val firmwareLabel: String get() = if (runs68k) "ROM image" else "NOR image"
    val fastAudio: Boolean get() = fastAudioNorMd5 != null || firmwareVariants.any { it.fastAudioNorMd5 != null }
    // ARM devices use the preview on qualified firmware, otherwise the queue limits only.
    val lowLatencyAudio: Boolean get() = sound && !runs68k
    val coreFlags: List<String> get() = listOf("--device", id)
    // New SD card sizes in MiB, 32 MB to the device's largest card. "2 GB" is 1886 MiB like a real 2 GB card.
    val sdSizes: List<Int> get() = if (!sdSlot) emptyList() else
        storageSlots.firstOrNull()?.families?.firstOrNull { it.family == "sdsc" }?.sizes
            ?: SD_SIZES.filter { (it.toLong() shl 20) in sdMin..sdMax }
    val appNames: List<String> get() = appNamesText.split('|')
    val buttons: List<String> get() = buttonsText.split('|')

    internal fun unresolvedFirmware(): Device = withFirmware(firmwareVariants.firstOrNull { !it.powerSupported })

    fun withFirmware(variant: FirmwareVariant?): Device =
        if (variant?.powerSupported == false && variant in firmwareVariants)
            copy(buttonsText = buttons.filter { it != "power" }.joinToString("|")) else this

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
