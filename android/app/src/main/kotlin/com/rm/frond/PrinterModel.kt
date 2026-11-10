package com.rm.frond

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

// The Dana's USB port while a machine runs: the choice of printer, Wi-Fi adapter or nothing, with the Wi-Fi link
// state polled once a second. The printer's jobs are PrintJobs.
class PrinterModel(
    private val host: ModelHost,
    private val device: Device,
    private val link: ControlLink,
    private val sessionId: String?,
    initialPort: String,
    initialModule: Boolean,
    notices: PrintNotices,
    inBackground: () -> Boolean,
    private val otherActive: () -> Boolean,
    private val problem: (String) -> Unit,
) {
    var port by mutableStateOf(initialPort)
        private set
    val plugged get() = port == UsbPort.PRINTER
    // The Wi-Fi adapter's link readout, null while it isn't the port's device.
    var wifiText by mutableStateOf<String?>(null)
        private set
    // The Dana Wireless's built-in Wi-Fi module: its radio, and the link readout polled once a second.
    var moduleOn by mutableStateOf(initialModule)
        private set
    var moduleText by mutableStateOf<String?>(null)
        private set
    // The received jobs; source names this printer when the infrared printer could also have sent one.
    val jobs = PrintJobs(host, device, link, "printer", "print", "USB printer", notices, inBackground, { if (otherActive()) "USB printer" else null }, problem)

    init {
        host.lifecycleScope.launch(Dispatchers.IO) {
            while (isActive) {
                if (plugged) jobs.poll() else if (port == UsbPort.WIFI) pollWifi()
                if (device.builtInWifi) pollModule()
                delay(1000)
            }
        }
    }

    // Switches the port's device. The core keeps what it had when the change fails, and the row shows which that is.
    fun choose(choice: String) {
        if (choice == port) return
        val before = port
        port = choice
        jobs.resetBytes()
        wifiText = if (choice == UsbPort.WIFI) PrintJob.wifiReadout(PrintJob.Port(true, UsbPort.WIFI)) else null
        remember(choice)
        host.lifecycleScope.launch(Dispatchers.IO) {
            val error = if (choice == UsbPort.PRINTER) jobs.attach(true) else {
                val reply = link.request("usb-port", choice, timeoutSeconds = 30)
                if (reply.ok) null else reply.message
            }
            if (error != null) {
                val state = PrintJob.parsePort(link.request("usb-port-state", timeoutSeconds = 5).message)
                val kept = if (state.valid) state.port else before
                host.runOnUiThread { port = kept; jobs.resetBytes(); wifiText = null; remember(kept); problem(error) }
            }
        }
    }

    // Takes the core's port as it is after the start request, so the row and settings match the core.
    fun adopt(state: PrintJob.Port) {
        if (!state.valid || state.port == port) return
        port = state.port
        jobs.resetBytes()
        wifiText = PrintJob.wifiReadout(state)
        remember(state.port)
    }

    private fun remember(choice: String) {
        sessionId?.let { id ->
            try { Sessions.updateSettings(host.applicationContext, id) { it.copy(usbPort = choice) } } catch (_: Exception) { }
        }
    }

    // Turns the built-in module's radio on or off. A refused change goes back to what the core has.
    fun chooseModule(on: Boolean) {
        val request = UsbPort.moduleRequest(device, on) ?: return
        if (on == moduleOn) return
        val before = moduleOn
        moduleOn = on
        moduleText = PrintJob.moduleReadout("off", on)
        rememberModule(on)
        host.lifecycleScope.launch(Dispatchers.IO) {
            val reply = link.request(request.first, request.second, timeoutSeconds = 30)
            if (!reply.ok) host.runOnUiThread { moduleOn = before; rememberModule(before); problem(reply.message) }
        }
    }

    private fun rememberModule(on: Boolean) {
        sessionId?.let { id ->
            try { Sessions.updateSettings(host.applicationContext, id) { it.copy(wifiModule = on) } } catch (_: Exception) { }
        }
    }

    private fun pollModule() {
        val reply = link.request("wifi-module-state", timeoutSeconds = 5)
        if (reply.ok) host.runOnUiThread { moduleText = PrintJob.moduleReadout(reply.message, moduleOn) }
    }

    private fun pollWifi() {
        val reply = link.request("usb-port-state", timeoutSeconds = 5)
        val state = PrintJob.parsePort(reply.message)
        if (reply.ok && state.port == UsbPort.WIFI) host.runOnUiThread { if (port == UsbPort.WIFI) wifiText = PrintJob.wifiReadout(state) }
    }

    fun close() = jobs.close()
}
