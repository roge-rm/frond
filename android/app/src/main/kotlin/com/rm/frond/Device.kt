package com.rm.frond

// The peripherals the desktop shows for every device (desktop/FirmwareInput.h) that Android doesn't
// do yet; these show greyed out.
enum class Peripheral(val label: String) {
    BLUETOOTH("Bluetooth"),
    USB("USB HotSync"),
    SERIAL("Serial port"),
}

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

// A Palm the app can run. Sizes match the core's deviceProfiles5 in common/device_profile5.h;
// coreType is its DeviceType5 number, as stored in session files.
enum class Device(
    val id: String,
    val label: String,
    val coreType: Int,
    val norSize: Long,
    val nandSize: Long,
    val nandRequired: Boolean,
    // Arguments the core needs before a raw NOR image for this device.
    val coreFlags: List<String>,
    // MD5 of the one NOR image the core allows low-latency audio with (native/main.cpp).
    val fastAudioNorMd5: String?,
    val sdSlot: Boolean = true,
    // Virtual Wi-Fi and the other network modes; the core refuses them on other devices.
    val network: Boolean = false,
) {
    TX(
        "palm-tx", "Palm TX", 3, 16L shl 20, 2112L * 64 * 1024, true, listOf("--experimental-tx"),
        "85b048a692aa317763a7077ea72da6cb", network = true,
    ),

    // The E2 is recognised from its NOR header, so it needs no flag. Its NAND is optional, as on the
    // desktop.
    E2(
        "tungsten-e2", "Tungsten E2", 0, 8L shl 20, 34_603_008L, false, emptyList(),
        "2a9faf69b43ba590cfb9ba78e4eef11c",
    );

    companion object {
        // Devices shown on the home screen, in order.
        val shown = listOf(TX, E2)

        fun byId(id: String?) = entries.firstOrNull { it.id == id }
        fun byCoreType(type: Int) = entries.firstOrNull { it.coreType == type }
    }
}
