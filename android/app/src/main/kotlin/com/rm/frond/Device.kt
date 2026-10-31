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

// A Palm the app can run. id is the device's fixed name from the core's deviceProfiles5 in
// common/device_profile5.h; it's what session files, saved settings and the core's --device use.
// Sizes match that table too.
enum class Device(
    val id: String,
    val label: String,
    val norSize: Long,
    val nandSize: Long,
    val nandRequired: Boolean,
    // MD5 of the one NOR image the core allows low-latency audio with (native/main.cpp).
    val fastAudioNorMd5: String?,
    val sdSlot: Boolean = true,
    // Virtual Wi-Fi and the other network modes; the core refuses them on other devices.
    val network: Boolean = false,
    // The cradle serial port (serial-mode); the core refuses it on other devices.
    val serialPort: Boolean = false,
) {
    TX(
        "palm-tx", "Palm TX", 16L shl 20, 2112L * 64 * 1024, true,
        "85b048a692aa317763a7077ea72da6cb", network = true, serialPort = true,
    ),

    // The E2's NAND is optional, as on the desktop.
    E2(
        "palm-e2", "Tungsten E2", 8L shl 20, 34_603_008L, false,
        "2a9faf69b43ba590cfb9ba78e4eef11c",
    );

    // Arguments the core needs before a raw NOR image for this device.
    val coreFlags: List<String> get() = listOf("--device", id)

    companion object {
        // Devices shown on the home screen, in order.
        val shown = listOf(TX, E2)

        fun byId(id: String?) = entries.firstOrNull { it.id == id }
    }
}
