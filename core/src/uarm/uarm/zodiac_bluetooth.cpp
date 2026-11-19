// The Zodiac's Bluetooth as an HCI shim. The ROM's stack sends H4 bytes through its BtlsVdriver serial driver
// to BtlsLib ('pilb'), the link controller for the BTA radio. With a controller set, the shim takes BtlsLib's
// start, stop, write and read calls (code hooks) and puts the bytes on a BluetoothBackend instead; received
// bytes go to the driver's receive callback, called from the idle loop's doze as the radio's interrupt would.
// BtlsLib's other calls run as they are. With no controller there are no hooks, and the ROM finds no radio.
//
// Facts of the 1.1 ROM: BtlsLib's calls are the ARM branches at 200f6e48 (start at offset 0, stop 4, write
// 0x20, read 0x1c, set callback 0x24, running flag get 0xc and set 0x10). Its globals are at [[r9]+0x180]: the
// running flag at +0x10390, the callback and its data at +0x1061c and +0x10620. Write and read take a buffer, a
// count (at most 0x80) and where to put the count done, and return 0 or an error. The callback takes its data
// and returns 0x23d9 while the serial queue is full. A start BtlsLib can't do returns 0x310c.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "soc_IMX1.h"

namespace {
constexpr uint32_t start = zodiacBluetoothHooks[0], stop = zodiacBluetoothHooks[1], write = zodiacBluetoothHooks[2],
                   read = zodiacBluetoothHooks[3], doze = zodiacBluetoothHooks[4];
constexpr uint32_t exportTable = 0x200f6e48;
constexpr uint32_t exportCode[] = {0xea00024f, 0xea00025d, 0xea0000a7, 0xea00004c, 0xea00007d, 0xea000052,
                                   0xea000060, 0xea00027a, 0xea00025f, 0xea00028a, 0xea000081, 0xea000087};
constexpr uint32_t entryCode[] = {0xe92d4218, 0xe92d4200, 0xe92d4038, 0xe92d4038};
constexpr uint16_t dozeCode[] = {0xb570, 0x4c30, 0x2501, 0x4648, 0x6800, 0x6800, 0x4e15, 0x1836};
constexpr uint32_t globalsAt = 0x180, runningAt = 0x10390, callbackAt = 0x1061c;
constexpr uint32_t errorNoRadio = 0x310c, queueFull = 0x23d9, maxChunk = 0x80;
// How many callbacks one doze makes, and the guest time each may take.
constexpr unsigned callbacksPerDoze = 64;
constexpr uint64_t callbackTicks = 32768 / 100;

// FROND_ZODIAC_BT_LOG=1 prints the H4 bytes each way on stderr.
void logBytes(const char* way, const uint8_t* bytes, uint32_t count) {
    static const bool on = std::getenv("FROND_ZODIAC_BT_LOG") != nullptr;
    if (!on || !count) return;
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
        bool known = RomHas(exportTable, exportCode, sizeof exportCode) && RomHas(doze, dozeCode, sizeof dozeCode);
        for (unsigned i = 0; i < 4; ++i) known = known && RomHas(zodiacBluetoothHooks[i], &entryCode[i], 4);
        if (!known) {
            error = "This ROM's Bluetooth library isn't one Frond knows.";
            return false;
        }
        BluetoothBackend candidate;
        if (!candidate.Open(path, error)) return false;
        bluetooth.romKnown = true;
    }
    bluetooth.backend.Close();
    bluetooth.path = path;
    bluetooth.lastError.clear();
    ApplyCodeHooks();
    error.clear();
    return true;
}

std::string SocIMX1::BluetoothState() {
    if (bluetooth.path.empty()) return "off";
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

bool SocIMX1::BluetoothOpen() {
    if (bluetooth.backend.Connected()) return true;
    if (!bluetooth.backend.Open(bluetooth.path, bluetooth.lastError)) return false;
    bluetooth.lastError.clear();
    return true;
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
        if (!bluetooth.backend.Connected() && bluetooth.lastError.empty()) bluetooth.lastError = "the controller closed";
    }
}

// True when the shim took the call; false lets the ROM run it.
bool SocIMX1::BluetoothHook(uint32_t pc, const uint32_t (&r)[16]) {
    if (pc == start) {
        BluetoothReturn(BluetoothOpen() ? 0 : errorNoRadio);
        return true;
    }
    if (pc == stop) {
        bluetooth.backend.Close();
        BluetoothReturn(0);
        return true;
    }
    if (pc == write || pc == read) {
        // Before a start the calls go to BtlsLib, which has no radio to talk to.
        const uint32_t globals = BluetoothGlobals(r[9]);
        uint32_t running = 0;
        if (!globals || !ReadGuest(globals + runningAt, &running, 4) || !running) return false;
        const uint32_t count = std::min(r[1], maxChunk);
        uint8_t bytes[maxChunk];
        uint32_t done = 0, result = 0;
        if (pc == write) {
            if (count && (!ReadGuest(r[0], bytes, count) || !BluetoothOpen() || !bluetooth.backend.Write(bytes, count)))
                result = errorNoRadio;
            else
                done = count;
            logBytes(">", bytes, done);
            bluetooth.backend.Poll(false);
        } else {
            while (done < count) {
                const uint16_t c = bluetooth.backend.Read();
                if (c > 0xff) break;
                bytes[done++] = uint8_t(c);
            }
            if (done && !WriteGuest(r[0], bytes, done)) result = errorNoRadio;
            logBytes("<", bytes, done);
        }
        if (r[2]) WriteGuest(r[2], &done, 4);
        BluetoothReturn(result);
        return true;
    }
    if (pc == doze) {
        if (bluetooth.delivering) return false;
        PollBluetooth();
        if (!bluetooth.backend.Pending()) return false;
        const uint32_t globals = BluetoothGlobals(r[9]);
        uint32_t running = 0;
        if (globals && ReadGuest(globals + runningAt, &running, 4) && running) BluetoothDeliver(globals);
        return false;
    }
    return false;
}

// Calls the driver's receive callback from the doze's entry (SVC, interrupts off) until the bytes are taken
// or the serial queue is full, then puts the registers back and lets the doze run.
void SocIMX1::BluetoothDeliver(uint32_t globals) {
    uint32_t saved[16], cpsr = 0, callback[2] = {};
    if (!GuestRegisters(saved, &cpsr) || !ReadGuest(globals + callbackAt, callback, 8) || !callback[0]) return;
    bluetooth.delivering = true;
    for (unsigned i = 0; i < callbacksPerDoze && bluetooth.backend.Pending() && !halted; ++i) {
        const size_t before = bluetooth.backend.Pending();
        if (!SetGuestRegister(0, callback[1]) || !SetGuestRegister(14, zodiacInjectedReturn) ||
            !JumpGuest(callback[0]) || !imx1MachineSetPcStop(machine, zodiacInjectedReturn))
            break;
        const uint64_t until = imx1MachineGetStatus(machine).time.ticks + callbackTicks;
        bool returned = false;
        while (!halted) {
            Imx1MachineBounds bounds;
            bounds.retired = 4000;
            bounds.work = 20000;
            bounds.deadline = {until, 0, 1};
            const auto run = imx1MachineRunBounded(machine, bounds);
            if (run.reason == Imx1MachineYield::terminal || run.reason == Imx1MachineYield::stopped ||
                run.reason == Imx1MachineYield::invalidBounds) {
                halted = true;
                std::cerr << "zodiac: the machine stopped in a Bluetooth callback: " << StopText() << std::endl;
                break;
            }
            if (run.reason == Imx1MachineYield::codeHook) {
                CodeHookHit();
                continue;
            }
            if (run.reason == Imx1MachineYield::pcStop) { returned = true; break; }
            if (run.reason == Imx1MachineYield::waiting || run.total.time.ticks >= until) break;
        }
        imx1MachineClearPcStop(machine);
        uint32_t result[16];
        if (!returned || !GuestRegisters(result)) {
            std::cerr << "zodiac: a Bluetooth callback didn't return" << std::endl;
            break;
        }
        if (result[0] == queueFull || bluetooth.backend.Pending() == before) break;
    }
    if (!halted) {
        for (unsigned i = 0; i < 15; ++i) SetGuestRegister(i, saved[i]);
        SetGuestRegister(16, cpsr);
        if (!JumpGuest(saved[15])) std::cerr << "zodiac: back to the doze failed" << std::endl;
        imx1MachineClearPcStop(machine);
    }
    bluetooth.delivering = false;
}
