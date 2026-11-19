// The Zodiac's Bluetooth as an HCI shim. The ROM's stack sends H4 bytes through its BtlsVdriver serial driver
// to BtlsLib ('pilb'), the link controller for the BTA radio. With a controller set, the shim takes BtlsLib's
// start, stop, write and read calls (code hooks) and puts the bytes on a BluetoothBackend instead. Received
// bytes go to the driver's receive callback, called where an interrupt routine runs: at the idle loop's doze
// entry, or, when the guest hasn't dozed for a while (a busy app), at the IRQ handler's call of the DAL's
// interrupt dispatch. Both are SVC mode with IRQs off; the callback gets the r9 of the driver's own calls.
// BtlsLib's other calls run as they are. With no controller and no radio started there are no hooks, and the
// ROM finds no radio.
//
// Facts of the 1.1 ROM: BtlsLib's calls are the ARM branches at 200f6e48 (start at offset 0, stop 4, write
// 0x20, read 0x1c, set callback 0x24, running flag get 0xc and set 0x10). Its globals are at [[r9]+0x180]: the
// running flag at +0x10390, the callback and its data at +0x1061c and +0x10620. Write and read take a buffer, a
// count (at most 0x80) and where to put the count done, and return 0 or an error: write 0x23d8 and read 0x23d7
// when the UART under them fails, with the count 0. The callback takes its data and returns 0x23d9 while the
// serial queue is full. A start BtlsLib can't do returns 0x310c. The IRQ handler (200c9ac8) switches to SVC
// mode, gets the system r9 and calls the dispatch (200cea24) at 200c9ae8, then checks for a task switch.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>

#include "soc_IMX1.h"

namespace {
constexpr uint32_t start = zodiacBluetoothHooks[0], stop = zodiacBluetoothHooks[1], write = zodiacBluetoothHooks[2],
                   read = zodiacBluetoothHooks[3], doze = zodiacBluetoothHooks[4], irq = zodiacBluetoothHooks[5];
constexpr uint32_t exportTable = 0x200f6e48;
constexpr uint32_t exportCode[] = {0xea00024f, 0xea00025d, 0xea0000a7, 0xea00004c, 0xea00007d, 0xea000052,
                                   0xea000060, 0xea00027a, 0xea00025f, 0xea00028a, 0xea000081, 0xea000087};
constexpr uint32_t entryCode[] = {0xe92d4218, 0xe92d4200, 0xe92d4038, 0xe92d4038};
constexpr uint16_t dozeCode[] = {0xb570, 0x4c30, 0x2501, 0x4648, 0x6800, 0x6800, 0x4e15, 0x1836};
// msr cpsr_c, #0x93; push; bl to r9's getter; mov r9, r0; bl to the dispatch; pop.
constexpr uint32_t irqCode[] = {0xe321f093, 0xe92d520f, 0xebff54c7, 0xe1a09000, 0xeb0013cd, 0xe8bd520f};
constexpr uint32_t irqCodeAt = irq - 0x10;
constexpr uint32_t globalsAt = 0x180, runningAt = 0x10390, callbackAt = 0x1061c;
constexpr uint32_t errorNoRadio = 0x310c, writeFailed = 0x23d8, readFailed = 0x23d7, queueFull = 0x23d9,
                   maxChunk = 0x80;
// How many callbacks one delivery makes. A callback copies at most 0x80 bytes; one that runs a whole second of
// guest time is taken as lost.
constexpr unsigned callbacksPerDelivery = 64;
// Guest ticks (32768 a second) without a doze before the IRQ handler delivers: two of the 100 Hz system ticks.
constexpr uint64_t ticksPerSecond = ZodiacInput::ticksPerSecond, dozeGapTicks = ticksPerSecond / 50;
// Slices of the callback's run, as RunToStop's (about 1 ms).
constexpr uint64_t sliceTicks = 32;

// FROND_ZODIAC_BT_LOG=1 prints the H4 bytes each way on stderr, and where received bytes are delivered.
bool logOn() {
    static const bool on = std::getenv("FROND_ZODIAC_BT_LOG") != nullptr;
    return on;
}
void logBytes(const char* way, const uint8_t* bytes, uint32_t count) {
    if (!logOn() || !count) return;
    std::fprintf(stderr, "zodiac bt %s", way);
    for (uint32_t i = 0; i < count; ++i) std::fprintf(stderr, " %02x", bytes[i]);
    std::fprintf(stderr, "\n");
}
}  // namespace

bool SocIMX1::SetBluetooth(const std::string& path, std::string& error) {
    if (path == bluetooth.path) {
        error.clear();
        return true;
    }
    if (!path.empty()) {
        bool known = RomHas(exportTable, exportCode, sizeof exportCode) && RomHas(doze, dozeCode, sizeof dozeCode) &&
                     RomHas(irqCodeAt, irqCode, sizeof irqCode);
        for (unsigned i = 0; i < 4; ++i) known = known && RomHas(zodiacBluetoothHooks[i], &entryCode[i], 4);
        if (!known) {
            error = "This ROM's Bluetooth library isn't one Frond knows.";
            return false;
        }
        BluetoothBackend candidate;
        if (!candidate.Open(path, error)) return false;
        bluetooth.romKnown = true;
        // A random locally administered address, as btvirt gives a controller and the PXA devices make.
        if (bluetooth.address == std::array<uint8_t, 6>{}) {
            std::random_device random;
            for (unsigned i = 0; i < 5; ++i) bluetooth.address[i] = uint8_t(random());
            bluetooth.address[5] = 0x02;
        }
    }
    // A radio the guest has on stays down until it starts it again (BluetoothDown).
    bluetooth.backend.Close();
    bluetooth.path = path;
    bluetooth.lastError.clear();
    if (bluetooth.started) bluetooth.lastError = "the controller changed; turn Bluetooth off and on again";
    ApplyCodeHooks();
    error.clear();
    return true;
}

std::string SocIMX1::BluetoothState() {
    if (bluetooth.path.empty()) return bluetooth.started ? "off; the guest's radio is down" : "off";
    if (bluetooth.backend.Connected()) return "connected";
    if (!bluetooth.lastError.empty()) return "disconnected: " + bluetooth.lastError;
    return "configured; radio off";
}

uint32_t SocIMX1::BluetoothGlobals(uint32_t r9) {
    uint32_t base = 0, globals = 0;
    if (!ReadGuest(r9, &base, 4) || !ReadGuest(base + globalsAt, &globals, 4)) return 0;
    uint32_t probe;
    return ReadGuest(globals + callbackAt + 4, &probe, 4) ? globals : 0;
}

// True while BtlsLib's running flag is set (the guest's radio is on).
bool SocIMX1::BluetoothRunning(uint32_t r9, uint32_t* globals) {
    const uint32_t at = BluetoothGlobals(r9);
    uint32_t running = 0;
    if (globals) *globals = at;
    return at && ReadGuest(at + runningAt, &running, 4) && running;
}

// The controller is gone: the guest's writes and reads fail as BtlsLib's do when its UART fails, until the
// guest stops the radio and starts it again, which opens the controller afresh at a packet boundary.
void SocIMX1::BluetoothDown(const std::string& why) {
    if (bluetooth.backend.Connected()) bluetooth.backend.Close();
    if (bluetooth.lastError.empty()) bluetooth.lastError = why;
}

// Returns from the hooked ARM function with result in r0.
void SocIMX1::BluetoothReturn(uint32_t result) {
    if (!SetGuestRegister(0, result) || !ReturnFromCallback()) {
        std::cerr << "zodiac: Bluetooth call return failed" << std::endl;
        imx1MachineLeaveCodeHook(machine);
    }
}

void SocIMX1::PollBluetooth() {
    if (bluetooth.backend.Connected()) {
        bluetooth.backend.Poll(true);
        if (!bluetooth.backend.Connected()) BluetoothDown("the controller closed");
    }
}

// True when the shim took the call; false lets the ROM run it.
bool SocIMX1::BluetoothHook(uint32_t pc, const uint32_t (&r)[16]) {
    if (pc == start) {
        // BtlsLib calls start again before each command once the radio is up. A start on a
        // running link keeps it, as reopening would make a new controller that lost its setup.
        if (bluetooth.started && bluetooth.backend.Connected()) {
            BluetoothReturn(0);
            return true;
        }
        // Otherwise open the controller afresh, so the H4 stream starts at a packet.
        bluetooth.backend.Close();
        bluetooth.lastError.clear();
        bool opened = false;
        if (bluetooth.path.empty()) {
            bluetooth.lastError = "no controller";
        } else {
            bluetooth.backend.SetAddress(bluetooth.address);
            opened = bluetooth.backend.Open(bluetooth.path, bluetooth.lastError);
        }
        bluetooth.started = opened;
        if (logOn()) std::fprintf(stderr, "zodiac bt start: %s\n", opened ? "open" : bluetooth.lastError.c_str());
        bluetooth.lastDoze = imx1MachineGetStatus(machine).time.ticks;
        BluetoothReturn(opened ? 0 : errorNoRadio);
        return true;
    }
    if (pc == stop) {
        // A Write_BD_ADDR from the guest changed the radio's address.
        std::array<uint8_t, 6> address{};
        if (bluetooth.backend.Address(address) && address != std::array<uint8_t, 6>{}) bluetooth.address = address;
        bluetooth.backend.Close();
        bluetooth.started = false;
        bluetooth.driverR9 = 0;
        if (logOn()) std::fprintf(stderr, "zodiac bt stop\n");
        BluetoothReturn(0);
        // With no controller set the hooks go once the guest's radio is down.
        if (!BluetoothHooksOn()) ApplyCodeHooks();
        return true;
    }
    if (pc == write || pc == read) {
        // Before a start the calls go to BtlsLib, which has no radio to talk to.
        if (!BluetoothRunning(r[9])) return false;
        bluetooth.driverR9 = r[9];
        const uint32_t count = std::min(r[1], maxChunk);
        uint8_t bytes[maxChunk];
        uint32_t done = 0, result = 0;
        const bool up = bluetooth.started && bluetooth.backend.Connected();
        if (pc == write) {
            if (!up || (count && (!ReadGuest(r[0], bytes, count) || !bluetooth.backend.Write(bytes, count)))) {
                if (up) BluetoothDown("a write to the controller failed");
                result = writeFailed;
            } else {
                done = count;
            }
            logBytes(">", bytes, done);
            if (bluetooth.backend.Connected()) bluetooth.backend.Poll(false);
        } else if (!up) {
            result = readFailed;
        } else {
            while (done < count) {
                const uint16_t c = bluetooth.backend.Read();
                if (c > 0xff) break;
                bytes[done++] = uint8_t(c);
            }
            if (done && !WriteGuest(r[0], bytes, done)) result = readFailed, done = 0;
            logBytes("<", bytes, done);
        }
        if (r[2]) WriteGuest(r[2], &done, 4);
        BluetoothReturn(result);
        return true;
    }
    if (pc == doze || pc == irq) {
        const uint64_t now = imx1MachineGetStatus(machine).time.ticks;
        if (pc == doze) bluetooth.lastDoze = now;
        if (bluetooth.delivering || !bluetooth.started) return false;
        // A busy guest that doesn't doze gets its bytes from the IRQ handler; an idling one from the doze.
        if (pc == irq && now - bluetooth.lastDoze < dozeGapTicks) return false;
        PollBluetooth();
        if (!bluetooth.backend.Pending()) return false;
        uint32_t globals = 0;
        // The callback runs with the r9 the driver's own calls had: at the doze and the IRQ r9 is whichever
        // task's is running, whose globals table needn't hold BtlsLib's.
        const uint32_t r9 = bluetooth.driverR9 ? bluetooth.driverR9 : r[9];
        if (BluetoothRunning(r9, &globals)) {
            BluetoothDeliver(globals, r9);
        } else if (bluetooth.driverR9 && !globals) {
            // BtlsLib was unloaded without a stop: its radio goes off with it.
            if (logOn()) std::fprintf(stderr, "zodiac bt library gone, radio off\n");
            bluetooth.backend.Close();
            bluetooth.started = false;
            bluetooth.driverR9 = 0;
            if (!BluetoothHooksOn()) ApplyCodeHooks();
        }
        return false;
    }
    return false;
}

// Runs the guest from a callback's entry until it returns to zodiacInjectedReturn (the pc stop), taking code hooks
// on the way. At most a second of guest time; a wait for an interrupt goes on while guest time moves.
bool SocIMX1::BluetoothRunCallback() {
    const uint64_t until = imx1MachineGetStatus(machine).time.ticks + ticksPerSecond;
    while (!halted) {
        const uint64_t before = imx1MachineGetStatus(machine).time.ticks;
        if (before >= until) return false;
        Imx1MachineBounds bounds;
        bounds.retired = 4000;
        bounds.work = 20000;
        bounds.deadline = {std::min(until, (before / sliceTicks + 1) * sliceTicks), 0, 1};
        const auto run = imx1MachineRunBounded(machine, bounds);
        if (run.reason == Imx1MachineYield::terminal || run.reason == Imx1MachineYield::stopped ||
            run.reason == Imx1MachineYield::invalidBounds) {
            halted = true;
            std::cerr << "zodiac: the machine stopped in a Bluetooth callback: " << StopText() << std::endl;
            return false;
        }
        if (run.reason == Imx1MachineYield::pcStop) return true;
        if (run.reason == Imx1MachineYield::codeHook) CodeHookHit();
        else if (run.reason == Imx1MachineYield::waiting && run.total.time.ticks == before) return false;
    }
    return false;
}

// Calls the driver's receive callback until the bytes are taken or the serial queue is full, then puts the
// registers and any pc stops of a guest call in progress back, and lets the hooked instruction run.
void SocIMX1::BluetoothDeliver(uint32_t globals, uint32_t r9) {
    uint32_t saved[16], cpsr = 0, callback[2] = {};
    if (!GuestRegisters(saved, &cpsr) || !ReadGuest(globals + callbackAt, callback, 8) || !callback[0]) return;
    const Imx1MachinePcStops stops = imx1MachineSavePcStops(machine);
    if (logOn())
        std::fprintf(stderr, "zodiac bt deliver at %s%s\n", (saved[15] & ~1u) == doze ? "doze" : "irq",
                     stops.armed ? " in a guest call" : "");
    bluetooth.delivering = true;
    for (unsigned i = 0; i < callbacksPerDelivery && bluetooth.backend.Pending() && !halted; ++i) {
        const size_t before = bluetooth.backend.Pending();
        if (!SetGuestRegister(0, callback[1]) || !SetGuestRegister(9, r9) ||
            !SetGuestRegister(14, zodiacInjectedReturn) || !JumpGuest(callback[0]) ||
            !imx1MachineSetPcStop(machine, zodiacInjectedReturn)) {
            std::cerr << "zodiac: couldn't call the Bluetooth receive callback" << std::endl;
            break;
        }
        const bool returned = BluetoothRunCallback();
        imx1MachineClearPcStop(machine);
        uint32_t result[16];
        if (!returned || !GuestRegisters(result)) {
            if (!halted) {
                std::cerr << "zodiac: the Bluetooth receive callback didn't return within a second of guest time; "
                             "the link is down until the radio is turned on again" << std::endl;
                BluetoothDown("the guest's receive callback didn't return");
            }
            break;
        }
        if (result[0] == queueFull || bluetooth.backend.Pending() == before) break;
    }
    if (!halted) {
        for (unsigned i = 0; i < 15; ++i) SetGuestRegister(i, saved[i]);
        SetGuestRegister(16, cpsr);
        if (!JumpGuest(saved[15])) std::cerr << "zodiac: back from the Bluetooth callback failed" << std::endl;
        imx1MachineRestorePcStops(machine, stops);
    }
    bluetooth.delivering = false;
}
