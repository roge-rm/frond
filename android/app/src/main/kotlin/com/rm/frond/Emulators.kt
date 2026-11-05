package com.rm.frond

// Which activity runs a device's core, and the process it runs in. ARM devices run libmain.so in
// FrondActivity (":emulator"), DragonBall devices libmain68k.so in Frond68kActivity (":emulator68k").
// Both cores carry a UAE copy with the same symbols, so they never share a process. Each process
// has its own menu activity, as the menu talks to the core in its process.
object Emulators {
    const val ARM_PROCESS = ":emulator"
    const val M68K_PROCESS = ":emulator68k"
    val processes = listOf(ARM_PROCESS, M68K_PROCESS)

    fun activity(device: Device): Class<out FrondActivity> =
        if (device.runs68k) Frond68kActivity::class.java else FrondActivity::class.java

    fun panel(device: Device): Class<out PeripheralsActivity> =
        if (device.runs68k) Peripherals68kActivity::class.java else PeripheralsActivity::class.java

    fun process(device: Device) = if (device.runs68k) M68K_PROCESS else ARM_PROCESS

    fun library(device: Device) = if (device.runs68k) "main68k" else "main"
}

// The 68k core (libmain68k.so) for DragonBall devices like the Palm IIIx, in its own process.
class Frond68kActivity : FrondActivity() {
    override val coreLibrary get() = "main68k"
    override val controlSocket get() = "control68k.sock"
}

// The running menu for Frond68kActivity, in its process.
class Peripherals68kActivity : PeripheralsActivity()
