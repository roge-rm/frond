package com.rm.frond

// The core's network modes (network-mode and network-state), in the desktop's order and words.
// Devices offer all of them, or only Disabled and Virtual Wi-Fi (wifiOnlyNetwork).
enum class NetworkMode(val id: String, val label: String) {
    DISABLED("disabled", "Disabled"),
    WIRELESS("wireless", "Virtual Wi-Fi (Frond)"),
    INTERNET("internet", "Internet via host"),
    CONNECTED("connected", "Simulated echo server"),
    TIMEOUT("timeout", "Simulated connection timeout"),
    DISCONNECTED("disconnected", "Simulated disconnection");

    companion object {
        fun byId(id: String?) = entries.firstOrNull { it.id == id }
        fun offered(wifiOnly: Boolean) = if (wifiOnly) listOf(DISABLED, WIRELESS) else entries
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
    val label: String get() = when (family) { "legacy-mmc-spi" -> "MMC"; "memory-stick" -> "Memory Stick"; else -> "SD" }
    val isStick: Boolean get() = family == "memory-stick"
    val protocol: String get() = if (family == "legacy-mmc-spi") "mmc" else "sd"
    fun accepts(bytes: Long): Boolean {
        if (bytes <= 0 || bytes % 512 != 0L || bytes !in minBytes..maxBytes ||
            (runtimeSizes.isNotEmpty() && bytes !in runtimeSizes)) return false
        if (sizeRule != "legacy-csd-fixed-512") return true
        val blocks = bytes / 512
        val mult = (0..7).firstOrNull { (blocks shr (it + 2)) < 0x1000 } ?: return false
        return blocks >= 4 && ((blocks shr (mult + 2)) shl (mult + 2)) == blocks
    }
    val sizes: List<Int> get() = if (isStick) runtimeSizes.map { (it shr 20).toInt() } else Device.SD_SIZES.filter { accepts(it.toLong() shl 20) }
}

data class StorageSlot(val id: Int, val label: Int, val kind: String, val families: List<CardFamily> = emptyList()) {
    val title: String get() = if (stick) "Memory Stick" else "Card $label"
    val commandPrefix: String get() = when (id) { 0 -> "sd-"; 2 -> "sd2-"; else -> "ms-" }
    val stick: Boolean get() = kind == "memory-stick"
}

// Generated inventory values come from deviceProfiles5. IDs are saved in sessions and settings.
data class Device internal constructor(
    val id: String,
    val label: String,
    // Group from deviceProfiles5: ez68k-rom and vz68k-rom are the DragonBall EZ and VZ boards.
    val cohort: String = "",
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
    // A USB printer on the host port that collects raw print jobs, run by the core.
    val usbPrinter: Boolean = false,
    // A USB 802.11b adapter on the same port, with the network through the core's router.
    val usbWifi: Boolean = false,
    // The Dana Wireless's built-in 802.11b module, with the network through the core's router.
    val builtInWifi: Boolean = false,
    // The Network setting offers only Disabled and Virtual Wi-Fi.
    val wifiOnlyNetwork: Boolean = false,
    // Takes the CLIE game controller (PEGA-GC10) on its interface connector, run by the core.
    val clieGamepad: Boolean = false,
    // DragonBall devices run in libmain68k.so (Frond68kActivity), the others in libmain.so (FrondActivity).
    val runs68k: Boolean = false,
    val hotSyncButton: Boolean = false,
    // A connector the device has that isn't emulated yet, shown greyed out; empty for none.
    val pendingUsb: String = "",
    // A colour LCD, shown as the guest draws it: no contrast setting.
    val colourLcd: Boolean = false,
    // The OS it runs: "palm-os" or "windows-ce".
    val os: String = "palm-os",
    val firmwareVariants: List<FirmwareVariant> = emptyList(),
    val storageSlots: List<StorageSlot> = emptyList(),
    val vibrate: Boolean = false,
) {
    // Applications and Hard reset are Palm OS guest services; a Windows CE device has its own or greys them out.
    val palmOs: Boolean get() = os == "palm-os"
    // Windows CE: Install copies the file onto the SD card, where the guest's File Explorer opens it (wceload for a
    // CAB), and Hard reset is a clean boot. The application list stays greyed. The rx1950 only so far.
    val installsToCard: Boolean get() = rx1950
    val hardResetWorks: Boolean get() = palmOs || rx1950
    // The USB link is a network (RNDIS): on or off, with no endpoint.
    val usbNetwork: Boolean get() = !palmOs && !zaurus
    // The name in the Add device and Import firmware lists.
    val listLabel: String get() = label

    // The real Zire 72 has no serial port, so it's left out of the list rather than greyed.
    val serialAbsent: Boolean get() = id == "palm-zire-72"
    val infraredKeyboard: Boolean get() = keyboard && !isDana()
    // A CLIE with the jog side tab gets the Jog controls choice; with a silkscreen it has no
    // keyboard, so the keyboard choice puts them beside the silkscreen and buttons.
    val jogControls: Boolean get() = sideTabWidth > 0
    val jogBesideButtons: Boolean get() = sideTabWidth > 0 && silkscreenHeight > 0
    private val pxaPalm: Boolean get() = when (id) {
        "palm-tx", "palm-e2", "palm-e2-franken", "palm-zire-72" -> true
        else -> false
    }
    val palmWifiCard: Boolean get() = pxaPalm && id != "palm-tx"
    val networkSupported: Boolean get() = network || palmWifiCard
    val networkWifiOnly: Boolean get() = wifiOnlyNetwork || (palmWifiCard && !network)
    val headphonesSupported: Boolean get() = pxaPalm || zaurus
    val sdWriteProtection: Boolean get() = pxaPalm || zaurus || id == "tapwave-zodiac-2"
    // A 68k device's slots take linked card files through frond-68k's sd-, sd2- and ms- commands (SdCard, Cards).
    val linkedCards: Boolean get() = runs68k && storageSlots.isNotEmpty()
    // A 68k grey LCD has a contrast setting; a colour one is shown as the guest draws it.
    val lcdContrast: Boolean get() = runs68k && !colourLcd
    // A 68k device's Memory Stick takes the classic sizes its driver does, as one card family.
    val stickFamily: CardFamily get() = CardFamily("memory-stick", (msSizes.minOrNull() ?: 0).toLong() shl 20,
        (msSizes.maxOrNull() ?: 0).toLong() shl 20, "ms", msSizes.map { it.toLong() shl 20 })
    fun familiesOf(slot: StorageSlot): List<CardFamily> = if (slot.stick) listOf(stickFamily) else slot.families
    val cardSupported: Boolean get() = sdSlot || memoryStick
    val cardLabel: String get() = if (storageSlot == "memory-stick") "Memory Stick" else "SD card"
    val hasNand: Boolean get() = nandSize > 0
    // What the firmware file is called: a 68k device takes one ROM file.
    val firmwareLabel: String get() = if (runs68k) "ROM image" else "NOR image"
    val fastAudio: Boolean get() = fastAudioNorMd5 != null || firmwareVariants.any { it.fastAudioNorMd5 != null }
    // ARM devices use the preview on qualified firmware, otherwise the queue limits only. The Zodiac's
    // sound has neither.
    val lowLatencyAudio: Boolean get() = sound && !runs68k && !zodiac
    val coreFlags: List<String> get() = listOf("--device", id)
    // New SD card sizes in MiB, 32 MB to the device's largest card. "2 GB" is 1886 MiB like a real 2 GB card.
    val sdSizes: List<Int> get() = if (!sdSlot) emptyList() else
        storageSlots.firstOrNull()?.families?.firstOrNull { it.family == "sdsc" }?.sizes
            ?: SD_SIZES.filter { (it.toLong() shl 20) in sdMin..sdMax }
    val appNames: List<String> get() = appNamesText.split('|')
    // The Tapwave Zodiac: a landscape screen, an engine of its own that runs best on Fast, and an
    // analogue stick and triggers that a controller drives through the core's own mapping.
    val zodiac: Boolean get() = id == ZODIAC_ID
    // The HP iPAQ rx1950 (Windows Mobile 5): HP's CEBL.nbf as the NOR and CEOS.nbf as the NAND.
    val rx1950: Boolean get() = id == RX1950_ID
    val zaurus: Boolean get() = id == ZAURUS_ID
    // New machines of the Zodiac, rx1950, T|X and Zire 72 start on Fast; they fall behind Original speed on Exact.
    val defaultEngine: Engine get() =
        if (zodiac || rx1950 || zaurus || id == "palm-tx" || id == "palm-zire-72") Engine.FAST else Engine.EXACT
    val buttons: List<String> get() = buttonsText.split('|')

    internal fun unresolvedFirmware(): Device = withFirmware(firmwareVariants.firstOrNull { !it.powerSupported })

    fun withFirmware(variant: FirmwareVariant?): Device =
        if (variant?.powerSupported == false && variant in firmwareVariants)
            copy(buttonsText = buttons.filter { it != "power" }.joinToString("|")) else this

    companion object {
        val SD_SIZES = listOf(32, 64, 128, 256, 512, 1024, 1886)
        const val ZODIAC_ID = "tapwave-zodiac-2"
        const val RX1950_ID = "hp-ipaq-rx1950"
        const val ZAURUS_ID = "sharp-zaurus-sl-c3000"
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
