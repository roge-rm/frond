// The Zodiac's Bluetooth as an HCI shim. The ROM's stack sends H4 bytes through its BtlsVdriver serial driver
// to BtlsLib ('pilb'), the link controller for the BTA radio. With a controller set, the shim takes BtlsLib's
// start, stop, write and read calls (code hooks) and puts the bytes on a BluetoothBackend instead. Received
// bytes go to the driver's receive callback, called where an interrupt routine runs: at the idle loop's doze
// entry, or, when the guest hasn't dozed for a second, at the IRQ handler's call of the DAL's interrupt
// dispatch. Both are SVC mode with IRQs off; the callback gets the r9 of the driver's own calls. A callback that
// goes round its wait for the port (another task has to open it) is ended there, as a delivery can't switch
// tasks. BtlsLib's other calls run as they are. With no controller and no radio started there are no hooks, and
// the ROM finds no radio.
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
constexpr uint32_t start = zodiacBluetoothHooks[0], stop = zodiacBluetoothHooks[1], writeCall = zodiacBluetoothHooks[2],
                   readCall = zodiacBluetoothHooks[3], doze = zodiacBluetoothHooks[4], irq = zodiacBluetoothHooks[5],
                   portWait = zodiacBluetoothHooks[6];
constexpr uint32_t exportTable = 0x200f6e48;
constexpr uint32_t exportCode[] = {0xea00024f, 0xea00025d, 0xea0000a7, 0xea00004c, 0xea00007d, 0xea000052,
                                   0xea000060, 0xea00027a, 0xea00025f, 0xea00028a, 0xea000081, 0xea000087};
constexpr uint32_t entryCode[] = {0xe92d4218, 0xe92d4200, 0xe92d4038, 0xe92d4038};
constexpr uint16_t dozeCode[] = {0xb570, 0x4c30, 0x2501, 0x4648, 0x6800, 0x6800, 0x4e15, 0x1836};
// msr cpsr_c, #0x93; push; bl to r9's getter; mov r9, r0; bl to the dispatch; pop.
constexpr uint32_t irqCode[] = {0xe321f093, 0xe92d520f, 0xebff54c7, 0xe1a09000, 0xeb0013cd, 0xe8bd520f};
constexpr uint32_t irqCodeAt = irq - 0x10;
// BtlsVdriver's receive callback (Thumb), the only one BtlsLib is given in this ROM. A delivery calls nothing else.
// It takes its data d and loops, reading while the port's state byte at [[r9+0xc]+0x5c]+0x20+[d]*36 is 1 (open).
// While it isn't, the branch at portWait either returns, reads and drops bytes, or (port record [d+0xc] flags
// bits 0-1 clear, bit 4 set or bit 7 clear, or a time not reached) goes round again, waiting for another task to
// change the byte. BtlsLib's task is switched away from there; a delivery can't be, so a callback that comes back
// to portWait with the port still not open and no bytes read since is ended there, and the bytes left wait for
// a later delivery. Each callback's first pass still runs the ROM's checks, so its timed way out (flags bits 0-1
// set, bit 4 clear, bit 7 set and the port record's time at +0x9c reached: it posts and returns) is taken when it
// comes. With the other flags the ROM waits for good; the shim gives up after portShutTicks of ended callbacks
// with nothing taken, dropping the bytes and taking the link down, so a stall is reported.
constexpr uint32_t driverCallback = 0x200f651f, spinCheckAt = portWait;
constexpr uint16_t driverCallbackCode[] = {0xb5f1, 0xb0ae, 0x982e, 0x68c7, 0x992e, 0x2000, 0x9024, 0xc903, 0x688d,
                                           0x9523, 0x690a, 0x9222, 0x68ca, 0x9221, 0x680e, 0x4649, 0x68c9, 0x6dc9,
                                           0x4a3c, 0x00c3, 0x1818, 0x1852, 0x0080, 0x912b, 0x1c39, 0x3180, 0x912a,
                                           0x902c, 0x922d, 0xa92c, 0xc903, 0x5c08, 0x2801, 0xd152};
constexpr uint16_t spinCheckCode[] = {0x2f00, 0xd01d, 0x6b38, 0x0781, 0xd0ec};
constexpr uint32_t portStatesAt = 0x5c, portStatesOffset = 0x20, portStateSize = 36;
constexpr uint32_t globalsAt = 0x180, runningAt = 0x10390, callbackAt = 0x1061c;
constexpr uint32_t errorNoRadio = 0x310c, writeFailed = 0x23d8, readFailed = 0x23d7, queueFull = 0x23d9,
                   maxChunk = 0x80;
// How many callbacks one delivery makes. A callback copies at most 0x80 bytes; one that runs a whole second of
// guest time is taken as lost.
constexpr unsigned callbacksPerDelivery = 64;
// Guest ticks (32768 a second) without a doze before the IRQ handler delivers.
constexpr uint64_t ticksPerSecond = ZodiacInput::ticksPerSecond, irqGapTicks = ticksPerSecond;
// Guest ticks between start calls that begin a new burst of radio setup (a second).
constexpr uint64_t startGap = ticksPerSecond;
// Guest ticks of deliveries ended at the port wait with no bytes taken before the link goes down (5 seconds).
constexpr uint64_t portShutTicks = 5 * ticksPerSecond;
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
                     RomHas(irqCodeAt, irqCode, sizeof irqCode) &&
                     RomHas(driverCallback & ~1u, driverCallbackCode, sizeof driverCallbackCode) &&
                     RomHas(spinCheckAt, spinCheckCode, sizeof spinCheckCode) &&
                     RomHas(0x200f6634, &portStatesOffset, 4);
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

// Keeps the r9 of a driver's write or read, most recent first.
void SocIMX1::BluetoothNoteR9(uint32_t r9) {
    auto& seen = bluetooth.driverR9;
    if (seen[0] == r9) return;
    if (logOn()) std::fprintf(stderr, "zodiac bt driver r9 %08x\n", r9);
    auto at = std::find(seen.begin(), seen.end(), r9);
    if (at == seen.end()) at = seen.end() - 1;
    std::rotate(seen.begin(), at, at + 1);
    seen[0] = r9;
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
        // The stack sets the radio up in bursts: Preferences' radio On or Discoverable, the end of an exchange and
        // the first use after a session load each send the whole setup (name, class, scan enable, flow control,
        // buffer sizes), with a start before every command of it. A start a second or more after the last one
        // begins a burst.
        const uint64_t now = imx1MachineGetStatus(machine).time.ticks;
        const bool burst = !bluetooth.lastStart || now < bluetooth.lastStart || now - bluetooth.lastStart >= startGap;
        bluetooth.lastStart = now;
        if (logOn())
            std::fprintf(stderr, "zodiac bt start call at %.3f s%s\n", double(now) / ticksPerSecond,
                         burst ? ", a new burst" : "");
        // A start on a running link keeps it, as reopening would make a new controller that lost its setup.
        if (bluetooth.started && bluetooth.backend.Connected()) {
            BluetoothReturn(0);
            return true;
        }
        // A radio that went down (a lost link or a changed controller) stays down for the rest of the burst: a
        // reopen there would give a controller without the setup commands sent before it. The guest's stop, or
        // the next burst, which sends the whole setup, opens it again.
        if (bluetooth.started && !burst) {
            if (logOn()) std::fprintf(stderr, "zodiac bt start: refused in a burst, %s\n", bluetooth.lastError.c_str());
            BluetoothReturn(errorNoRadio);
            return true;
        }
        // Open the controller afresh, so the H4 stream starts at a packet.
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
        bluetooth.portShutSince = 0;
        if (logOn()) std::fprintf(stderr, "zodiac bt start: %s\n", opened ? "open" : bluetooth.lastError.c_str());
        bluetooth.lastDoze = imx1MachineGetStatus(machine).time.ticks;
        BluetoothReturn(opened ? 0 : errorNoRadio);
        // With no controller set the hooks go once no radio is started.
        if (!BluetoothHooksOn()) ApplyCodeHooks();
        return true;
    }
    if (pc == stop) {
        // A Write_BD_ADDR from the guest changed the radio's address.
        std::array<uint8_t, 6> address{};
        if (bluetooth.backend.Address(address) && address != std::array<uint8_t, 6>{}) bluetooth.address = address;
        bluetooth.backend.Close();
        bluetooth.started = false;
        bluetooth.driverR9 = {};
        bluetooth.portShutSince = 0;
        if (logOn()) std::fprintf(stderr, "zodiac bt stop\n");
        BluetoothReturn(0);
        // With no controller set the hooks go once the guest's radio is down.
        if (!BluetoothHooksOn()) ApplyCodeHooks();
        return true;
    }
    if (pc == writeCall || pc == readCall) {
        // Before a start the calls go to BtlsLib, which has no radio to talk to.
        if (!BluetoothRunning(r[9])) return false;
        BluetoothNoteR9(r[9]);
        const uint32_t count = std::min(r[1], maxChunk);
        uint8_t bytes[maxChunk];
        uint32_t done = 0, result = 0;
        const bool up = bluetooth.started && bluetooth.backend.Connected();
        if (pc == writeCall) {
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
            bluetooth.readSincePortWait += done;
            bluetooth.readInDelivery += done;
            logBytes("<", bytes, done);
        }
        if (r[2]) WriteGuest(r[2], &done, 4);
        BluetoothReturn(result);
        return true;
    }
    if (pc == portWait) {
        // A callback going round its wait for the port ends here; its frame goes when the delivery puts the
        // registers back.
        if (!bluetooth.delivering || BluetoothPortOpen(r[9], bluetooth.deliveringData)) return false;
        const bool again = bluetooth.portWaitHits++ && !bluetooth.readSincePortWait;
        bluetooth.readSincePortWait = 0;
        if (!again) return false;
        if (logOn()) std::fprintf(stderr, "zodiac bt callback ended at its wait for the port\n");
        bluetooth.callbackEnded = true;
        return true;
    }
    if (pc == doze || pc == irq) {
        const uint64_t now = imx1MachineGetStatus(machine).time.ticks;
        if (pc == doze) bluetooth.lastDoze = now;
        if (bluetooth.delivering || !bluetooth.started) return false;
        // Bytes go in at the doze. The IRQ handler is only a fallback for a guest that hasn't dozed for a second: a
        // delivery there runs the driver inside whatever task it interrupted, and one inside the serial driver's own
        // calls (PPP over DUN) faulted in it.
        if (pc == irq && now - bluetooth.lastDoze < irqGapTicks) return false;
        PollBluetooth();
        if (!bluetooth.backend.Pending()) return false;
        // The callback runs with an r9 the driver's own calls had, one whose table still holds BtlsLib's running
        // globals: at the doze and the IRQ r9 is whichever task's is running, whose table needn't hold them.
        // A table whose callback isn't the driver's is passed over. Bytes that come while the running flag is clear
        // but the globals are still there (the guest is on its way to stop) wait for the stop.
        uint32_t globals = 0;
        bool known = false, present = false;
        for (const uint32_t r9 : bluetooth.driverR9) {
            if (!r9) continue;
            known = true;
            if (BluetoothGlobals(r9)) present = true;
            if (BluetoothRunning(r9, &globals) && BluetoothDeliver(globals, r9)) return false;
        }
        if (!known) {
            if (BluetoothRunning(r[9], &globals)) BluetoothDeliver(globals, r[9]);
        } else if (!present) {
            // BtlsLib went away from every table the driver used without a stop: its radio goes off with it.
            if (logOn()) std::fprintf(stderr, "zodiac bt library gone, radio off\n");
            bluetooth.backend.Close();
            bluetooth.started = false;
            bluetooth.driverR9 = {};
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
        if (run.reason == Imx1MachineYield::codeHook) {
            CodeHookHit();
            if (bluetooth.callbackEnded) return true;
        } else if (run.reason == Imx1MachineYield::waiting && run.total.time.ticks == before) return false;
    }
    return false;
}

// The receive callback's port state (see driverCallback) is open, or can't be read.
bool SocIMX1::BluetoothPortOpen(uint32_t r9, uint32_t data) {
    uint32_t table = 0, index = 0;
    uint8_t state = 0;
    return !ReadGuest(r9 + 0xc, &table, 4) || !ReadGuest(table + portStatesAt, &table, 4) ||
           !ReadGuest(data, &index, 4) || !ReadGuest(table + portStatesOffset + index * portStateSize, &state, 1) ||
           state == 1;
}

// Calls the driver's receive callback until the bytes are taken or the serial queue is full, then puts the
// registers and any pc stops of a guest call in progress back, and lets the hooked instruction run.
bool SocIMX1::BluetoothDeliver(uint32_t globals, uint32_t r9) {
    uint32_t saved[16], cpsr = 0, callback[2] = {};
    if (!GuestRegisters(saved, &cpsr) || !ReadGuest(globals + callbackAt, callback, 8)) return false;
    // Globals left in memory BtlsLib no longer owns could hold anything: only the driver's own callback is called.
    if (callback[0] != driverCallback) {
        if (logOn()) std::fprintf(stderr, "zodiac bt r9 %08x: callback %08x isn't the driver's\n", r9, callback[0]);
        return false;
    }
    bluetooth.readInDelivery = 0;
    const Imx1MachinePcStops stops = imx1MachineSavePcStops(machine);
    if (logOn())
        std::fprintf(stderr, "zodiac bt deliver at %s%s\n", (saved[15] & ~1u) == doze ? "doze" : "irq",
                     stops.armed ? " in a guest call" : "");
    bluetooth.delivering = true;
    bluetooth.deliveringData = callback[1];
    bluetooth.callbackEnded = false;
    for (unsigned i = 0; i < callbacksPerDelivery && bluetooth.backend.Pending() && !halted; ++i) {
        const size_t before = bluetooth.backend.Pending();
        bluetooth.portWaitHits = 0;
        bluetooth.readSincePortWait = 0;
        if (!SetGuestRegister(0, callback[1]) || !SetGuestRegister(9, r9) ||
            !SetGuestRegister(14, zodiacInjectedReturn) || !JumpGuest(callback[0]) ||
            !imx1MachineSetPcStop(machine, zodiacInjectedReturn)) {
            std::cerr << "zodiac: couldn't call the Bluetooth receive callback" << std::endl;
            break;
        }
        const bool returned = BluetoothRunCallback();
        imx1MachineClearPcStop(machine);
        uint32_t result[16];
        if (bluetooth.callbackEnded) break;
        if (!returned || !GuestRegisters(result)) {
            if (logOn() && GuestRegisters(result))
                std::fprintf(stderr, "zodiac bt callback stuck at pc %08x lr %08x sp %08x r0 %08x r1 %08x\n",
                             result[15], result[14], result[13], result[0], result[1]);
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
    // Callbacks ended at the port wait with nothing taken: after portShutTicks of that the bytes go and the link
    // goes down, as the port isn't opening.
    if (!bluetooth.callbackEnded || bluetooth.readInDelivery) {
        bluetooth.portShutSince = 0;
    } else if (!halted) {
        const uint64_t now = imx1MachineGetStatus(machine).time.ticks;
        if (!bluetooth.portShutSince || now < bluetooth.portShutSince) {
            bluetooth.portShutSince = std::max<uint64_t>(now, 1);
        } else if (now - bluetooth.portShutSince >= portShutTicks) {
            std::cerr << "zodiac: the Bluetooth serial port stayed shut with bytes waiting; the link is down until "
                         "the radio is turned on again" << std::endl;
            bluetooth.portShutSince = 0;
            BluetoothDown("the guest's serial port stayed shut");
        }
    }
    return true;
}
