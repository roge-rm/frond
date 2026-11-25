// Boots the HP iPAQ rx1950's bootloader on the S3C2442 machine and keeps what its UARTs send.
//
//   rx1950-boot --cebl CEBL.nbf [--ceos CEOS.nbf] [--nand FILE] [--out DIR] [--instructions N]
//               [--mmio-trace N] [--history N] [--debug-messages] [--any-firmware] [--peek VA]... [--walk MVA]... [--watch PA]...
//               [--seconds S] [--tap T,X,Y[,MS]]... [--key T,NAME[,MS]]... [--shot T]... [--interactive]
//               [--engine exact|fast] [--digest] [--pin-low PIN]... [--serial-cable] [--battery SPEC]
//
// --battery sets the battery as the machine setting does (BatteryPower.h: max, level N or host N, then
// charging or battery; Maximum on battery by default), fed to the ADC and charger pins (rx1950_power.h).
// --pin-low holds an input pin such as F6 low from reset; every other input is high. --serial-cable boots
// with the serial cable attached (rx1950_board.h), which eboot needs to print on UART0.
// --engine picks the interpreter (exact) or the Fast engine (fast, the default), which run the same
// instructions with the same timing. --digest prints the registers and hashes of SDRAM (after the data cache
// is cleaned), the NAND and the SFR state at the end, to compare the engines' runs.
//
// CEBL.nbf's eboot goes straight into SDRAM at 0x30080000 and runs from there with the MMU off and the
// PLL set, as the block 0 loader would leave it (that loader isn't in either dump). CEOS.nbf, if given, fills the NAND
// (s3c2442_nand.h) as rx1950NandImage lays it out. With --nand the NAND lives in that store file: made
// from CEOS.nbf when it doesn't exist yet, loaded when it does, and the blocks the run changed are written
// back at the end. Both NBFs must be the ones Frond accepts (rx1950_nand_image.h) unless --any-firmware.
// DIR gets uart0.log, uart1.log, uart2.log and, with
// --mmio-trace, the last N SFR accesses in mmio.log, and nk.log what the kernel printed. The last line
// printed says why the run stopped.
//
// Input is scripted in guest seconds: --tap puts the stylus on screen pixel X, Y at T for MS milliseconds
// (150 by default), --key holds a key (rx1950_board.h: power, record, calendar, contacts, inbox,
// wireless, up, down, left, right, action) for MS (200), and --shot saves the screen at T as
// screen-T.ppm. --seconds stops the run at that guest time; the instruction budget still applies.
// screen.ppm is the screen when the run stops. --interactive goes on after --seconds with commands from
// stdin (see the loop at the end), for stepping through screens without booting again.
//
// The retail kernel only sends its debug output to a serial port on the debug board (a Super I/O chip on
// nGCS5 it probes for). Without one it keeps the text in a 64 KB ring in RAM, which nk.log is, when
// --debug-messages turns it on in the board settings page (rx1950_nand_image.h).

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <tuple>
#include <string>
#include <vector>

#include <chrono>

#include <sys/wait.h>
#include <unistd.h>

#include "CPU.h"
#include "arm920_state.h"
#include "ZodiacRom.h"
#include "rx1950_board.h"
#include "rx1950_power.h"
#include "rx1950_nand_image.h"
#include "s3c2442_machine.h"

namespace {
    constexpr uint32_t nbfHeader = 0xa5a, ebootBytes = 256 * 1024, ebootPhysical = 0x30080000;
    // The kernel's debug text ring in the WM5 CEOS.nbf (kernel built Jun 1 2005): 64 KB of text and its
    // read and write indices, found in OEMWriteDebugByte.
    constexpr uint32_t nkRing = 0x802d2b40, nkRingIndices = 0x80292e28, nkRingSize = 0x10000;

    // One scripted input: at ns, the stylus (key null) down at raw x, y or up, or a key pressed or let go.
    struct Input {
        uint64_t ns;
        const Rx1950Key* key;
        bool down;
        uint16_t x, y;
    };

    bool writePpm(const std::string& path, const std::vector<uint32_t>& pixels, unsigned w, unsigned h) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (const uint32_t p : pixels) {
            const uint8_t rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
        return true;
    }

    bool readFile(const char* path, std::vector<uint8_t>& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* cebl = nullptr;
    const char* ceos = nullptr;
    std::string out = ".";
    uint64_t instructions = 2000000000ull;
    uint64_t traceLimit = 0;
    size_t history = 0;
    Rx1950NandOptions nandOptions;
    const char* nandPath = nullptr;
    bool anyFirmware = false;
    std::vector<uint32_t> peeks;  // virtual addresses whose words are printed when the run stops
    std::vector<uint32_t> walks;
    std::vector<uint32_t> watches;  // SDRAM physical words whose changes are printed (needs --history)
    std::vector<Input> inputs;
    std::vector<double> shots;
    double seconds = 0;
    bool interactive = false;
    bool fastEngine = true, digest = false, serialCable = false;
    BatteryPower battery;  // Maximum on the charger, the machine setting's default
    std::vector<std::pair<unsigned, unsigned>> lowPins;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--cebl") cebl = next();
        else if (a == "--ceos") ceos = next();
        else if (a == "--out") out = next();
        else if (a == "--instructions") instructions = std::strtoull(next(), nullptr, 0);
        else if (a == "--mmio-trace") traceLimit = std::strtoull(next(), nullptr, 0);
        else if (a == "--history") history = std::strtoull(next(), nullptr, 0);
        else if (a == "--debug-messages") nandOptions.debugMessages = true;
        else if (a == "--nand") nandPath = next();
        else if (a == "--any-firmware") anyFirmware = true;
        else if (a == "--peek") peeks.push_back(uint32_t(std::strtoul(next(), nullptr, 0)));
        else if (a == "--watch") watches.push_back(uint32_t(std::strtoul(next(), nullptr, 0)));
        else if (a == "--walk") walks.push_back(uint32_t(std::strtoul(next(), nullptr, 0)));
        else if (a == "--interactive") interactive = true;
        else if (a == "--digest") digest = true;
        else if (a == "--serial-cable") serialCable = true;
        else if (a == "--battery") {
            if (!BatteryPower::Parse(next(), battery)) {
                std::fprintf(stderr, "--battery is max, level N or host N (0-100), then charging or battery\n");
                return 2;
            }
        }
        else if (a == "--pin-low") {
            const std::string pin = next();
            static const char ports[] = "ABCDEFGHJ";
            const char* port = pin.empty() ? nullptr : std::strchr(ports, std::toupper(pin[0]));
            const unsigned bit = pin.size() > 1 ? unsigned(std::atoi(pin.c_str() + 1)) : 32;
            if (!port || !*port || bit > 31) { std::fprintf(stderr, "--pin-low takes a pin like F6\n"); return 2; }
            lowPins.push_back({unsigned(port - ports), bit});
        }
        else if (a == "--engine") {
            const std::string e = next();
            if (e != "exact" && e != "fast") { std::fprintf(stderr, "--engine is exact or fast\n"); return 2; }
            fastEngine = e == "fast";
        }
        else if (a == "--seconds") seconds = std::strtod(next(), nullptr);
        else if (a == "--shot") shots.push_back(std::strtod(next(), nullptr));
        else if (a == "--tap") {
            double t = 0;
            unsigned x = 0, y = 0, ms = 150;
            if (std::sscanf(next(), "%lf,%u,%u,%u", &t, &x, &y, &ms) < 3 || x >= 240 || y >= 320) {
                std::fprintf(stderr, "--tap T,X,Y[,MS] with X below 240 and Y below 320\n");
                return 2;
            }
            const uint64_t at = uint64_t(t * 1e9);
            inputs.push_back({at, nullptr, true, rx1950TouchRawX(x), rx1950TouchRawY(y)});
            inputs.push_back({at + ms * 1000000ull, nullptr, false, 0, 0});
        } else if (a == "--key") {
            double t = 0;
            char name[32] = {};
            unsigned ms = 200;
            const char* v = next();
            const char* comma = std::strchr(v, ',');
            t = std::strtod(v, nullptr);
            if (comma) std::sscanf(comma + 1, "%31[a-z],%u", name, &ms);
            const Rx1950Key* key = rx1950Key(name);
            if (!key) { std::fprintf(stderr, "--key T,NAME[,MS]: unknown key %s\n", name); return 2; }
            const uint64_t at = uint64_t(t * 1e9);
            inputs.push_back({at, key, true, 0, 0});
            inputs.push_back({at + ms * 1000000ull, key, false, 0, 0});
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (!cebl) {
        std::fprintf(stderr, "usage: rx1950-boot --cebl CEBL.nbf [--ceos CEOS.nbf] [--nand FILE] [--out DIR] [--instructions N] [--mmio-trace N] [--history N] [--debug-messages] [--any-firmware] [--peek VA]... [--walk MVA]... [--watch PA]... [--seconds S] [--tap T,X,Y[,MS]]... [--key T,NAME[,MS]]... [--shot T]... [--interactive]\n");
        return 2;
    }
    std::vector<uint8_t> bl;
    if (!readFile(cebl, bl) || bl.size() != nbfHeader + ebootBytes) {
        std::fprintf(stderr, "%s: not the rx1950's CEBL.nbf (%zu bytes)\n", cebl, bl.size());
        return 1;
    }
    auto accepted = [&](const Rx1950Firmware& expected, const std::vector<uint8_t>& data, const char* path) {
        if (anyFirmware || (data.size() == expected.bytes && frondZodiacRom::sha256Hex(data.data(), data.size()) == expected.sha256))
            return true;
        std::fprintf(stderr, "%s: not the rx1950's %s that Frond runs (HP's WM5 ROM 1.01.00 ENG); --any-firmware runs it anyway\n",
                     path, expected.name);
        return false;
    };
    if (!accepted(rx1950Cebl, bl, cebl)) return 1;
    std::string error;
    S3c2442Options options;
    options.rtcStart = rx1950RtcStart;
    if (!serialCable) options.pinLevels[rx1950CablePort] &= ~(1u << rx1950CableBit);
    rx1950BatteryPins(battery, options);
    for (const auto& [port, bit] : lowPins) options.pinLevels[port] &= ~(1u << bit);
    auto machine = S3c2442Machine::create(options, error);
    if (!machine) { std::fprintf(stderr, "machine: %s\n", error.c_str()); return 1; }
    if (fastEngine && !machine->setFastEngine(true)) { std::fprintf(stderr, "machine: no Fast engine in this build\n"); return 1; }
    machine->loadRam(ebootPhysical, bl.data() + nbfHeader, ebootBytes);
    rx1950ApplyBattery(*machine, battery);
    // ASSUMPTION "rx1950 IPL clocks": the block 0 loader sets the PLL before eboot, which never does.
    // 296.35 MHz from the 16.9344 MHz crystal (MDIV 97, PDIV 1, SDIV 2), FCLK:HCLK:PCLK 1:3:6.
    machine->clock.write(0x04, 0x61012);
    machine->clock.write(0x14, 0x7);
    rx1950LoaderPins(machine->gpio);
    // A wake from sleep starts as power on does: the loader copies eboot from NAND again and sets the clocks.
    // eboot sees OFFRST in GSTATUS2 and goes to the kernel, which resumes from its sleep data.
    machine->resetLoader = [&] {
        machine->loadRam(ebootPhysical, bl.data() + nbfHeader, ebootBytes);
        machine->clock.write(0x04, 0x61012);
        machine->clock.write(0x14, 0x7);
        rx1950LoaderPins(machine->gpio);
        machine->start(ebootPhysical);
    };
    std::vector<uint8_t> flash;
    bool nandNew = true;
    if (nandPath && std::ifstream(nandPath).good()) {
        if (!rx1950NandLoad(nandPath, flash, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        nandNew = false;
    } else if (ceos) {
        std::vector<uint8_t> os;
        if (!readFile(ceos, os)) { std::fprintf(stderr, "%s: can't read\n", ceos); return 1; }
        if (!accepted(rx1950Ceos, os, ceos)) return 1;
        if (!rx1950NandImage(bl.data() + nbfHeader, ebootBytes, os.data(), os.size(), flash, error, nandOptions)) {
            std::fprintf(stderr, "%s: %s\n", ceos, error.c_str());
            return 1;
        }
    }
    if (!flash.empty()) machine->nand.setImage(std::move(flash));
    if (nandPath && nandNew && !machine->nand.flash.empty() && !rx1950NandSave(nandPath, machine->nand, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    FILE* logs[3];
    for (unsigned c = 0; c < 3; ++c) {
        logs[c] = std::fopen((out + "/uart" + std::to_string(c) + ".log").c_str(), "wb");
        if (!logs[c]) { std::fprintf(stderr, "%s: can't write\n", out.c_str()); return 1; }
    }
    machine->uartSink = [&](unsigned channel, uint8_t byte) {
        std::fputc(byte, logs[channel]);
        if (byte == '\n') std::fflush(logs[channel]);
    };
    // The last traceLimit SFR accesses, written out when the run stops.
    struct Access { uint32_t pc, address, value; unsigned size; bool write; };
    std::vector<Access> traced(traceLimit);
    uint64_t tracedCount = 0;
    if (history) machine->keepHistory(history);
    // Watches compare SDRAM and the kernel's cached view (0x80000000 is SDRAM from 0x30100000) after every
    // instruction, so a write held in the D-cache shows apart from its write-back.
    std::vector<std::pair<uint32_t, uint32_t>> watched(watches.size(), {0, 0});
    uint64_t steps = 0;
    if (!watches.empty() && history) {
        machine->stepHook = [&](uint32_t pc) {
            ++steps;
            for (size_t i = 0; i < watches.size(); ++i) {
                uint32_t ram = 0, cached = 0;
                std::memcpy(&ram, machine->sdram() + (watches[i] - 0x30000000), 4);
                const uint32_t va = watches[i] >= 0x30100000 ? 0x80000000 + (watches[i] - 0x30100000) : 0x8c000000 + (watches[i] - 0x30000000);
                if (!cpuInspectRead(machine->cpu(), va, &cached, 4)) cached = 0xdeadbeef;
                if (ram != watched[i].first || cached != watched[i].second) {
                    std::printf("watch %08x at step %" PRIu64 " pc %08x pid %08x: ram %08x cached %08x\n", watches[i], steps, pc,
                                cpuGetPid(machine->cpu()), ram, cached);
                    watched[i] = {ram, cached};
                }
            }
        };
    }
    // For R4: the first ADC conversions in normal mode (battery channels) and reads of the AC and charge
    // status pins (GPFDAT bits 2 and 3) and the charger controls on port J.
    struct Seen { uint32_t pc; uint64_t ns; uint32_t value; };
    std::vector<std::pair<unsigned, Seen>> adcStarts;
    Seen firstGpf{0, 0, 0}, firstGpj{0, 0, 0};
    uint64_t gpfReads = 0, gpjReads = 0;
    // NAND page reads: the total and how often each row was read, to tell new reads from rereads.
    uint64_t nandReads = 0;
    std::map<uint32_t, unsigned> nandRows;
    uint32_t nandCommand = 0xff, nandAddress = 0, nandCycles = 0;
    // "mmio S": counts of each pc, register, direction and value for a while.
    std::map<std::tuple<uint32_t, uint32_t, bool, uint32_t>, unsigned>* mmioCounts = nullptr;
    uint32_t mmioLow = 0, mmioHigh = 0xffffffff;  // "mmio S LOW HIGH" counts only registers in that range
    // "intclog N": the last interrupt controller and UART 0/2 events: writes to SRCPND bit 28, SUBSRCPND, INTSUBMSK
    // (changes only), UART 0 and 2 UCON, UFCON and UTXH, and each time SRCPND bit 28 (INT_UART0)
    // is seen to change. Repeats of one event in a row are counted, not stored.
    struct IntcEvent { uint64_t ns; uint32_t pc, address, value, srcpnd, subsrcpnd, intsubmsk, intmsk, ucon0; bool write; unsigned repeats; };
    std::vector<IntcEvent> intcEvents;
    bool uart0Pending = false;
    uint32_t lastSubmsk = 0x7fff;
    auto intcEvent = [&](uint32_t pc, uint32_t address, bool write, uint32_t value) {
        const S3cIntc& ic = machine->intc;
        if (!intcEvents.empty()) {
            IntcEvent& last = intcEvents.back();
            if (last.pc == pc && last.address == address && last.write == write && last.value == value &&
                last.srcpnd == ic.srcpnd && last.subsrcpnd == ic.subsrcpnd && last.intsubmsk == ic.intsubmsk) {
                ++last.repeats;
                return;
            }
        }
        if (intcEvents.size() >= 8192) intcEvents.erase(intcEvents.begin(), intcEvents.begin() + 4096);
        intcEvents.push_back({machine->nanoseconds(), pc, address, value, ic.srcpnd, ic.subsrcpnd, ic.intsubmsk, ic.intmsk, machine->uarts[0].ucon, write, 0});
    };
    auto observe = [&](uint32_t pc, uint32_t address, unsigned, bool write, uint32_t value) {
        if (write && ((address == 0x4a000000 && (value & 0x10000000)) || address == 0x4a000018 ||
                      (address == 0x4a00001c && value != lastSubmsk) ||
                      ((address & ~0x8000u) >= 0x50000004 && (address & ~0x8000u) <= 0x50000008) ||
                      (address & ~0x8000u) == 0x50000020))
            intcEvent(pc, address, write, value);
        if (write && address == 0x4a00001c) lastSubmsk = value;
        if (bool(machine->intc.srcpnd & 0x10000000) != uart0Pending) {
            uart0Pending = !uart0Pending;
            intcEvent(pc, 0xffffffff, uart0Pending, 0x10000000);
        }
        if (write && address == 0x58000000 && (value & 1) && adcStarts.size() < 16 && !(machine->adc.adctsc & 7))
            adcStarts.push_back({(value >> 3) & 7, {pc, machine->nanoseconds(), value}});
        if (mmioCounts && address >= mmioLow && address <= mmioHigh) ++(*mmioCounts)[{pc, address, write, value}];
        if (write && address == 0x4e000008) {
            nandCommand = value & 0xff;
            nandAddress = nandCycles = 0;
        } else if (write && address == 0x4e00000c && (nandCommand == 0x00 || nandCommand == 0x01 || nandCommand == 0x50)) {
            if (nandCycles >= 1 && nandCycles <= 3) nandAddress |= (value & 0xff) << (8 * (nandCycles - 1));
            if (++nandCycles == 4) { ++nandReads; ++nandRows[nandAddress]; }
        }
        if (!write && address == 0x56000054 && !gpfReads++) firstGpf = {pc, machine->nanoseconds(), value};
        if (!write && address == 0x560000d4 && !gpjReads++) firstGpj = {pc, machine->nanoseconds(), value};
    };
    if (traceLimit)
        machine->mmioTrace = [&, observe](uint32_t pc, uint32_t address, unsigned size, bool write, uint32_t value) {
            traced[tracedCount++ % traceLimit] = {pc, address, value, size, write};
            observe(pc, address, size, write, value);
        };
    else
        machine->mmioTrace = observe;

    std::stable_sort(inputs.begin(), inputs.end(), [](const Input& a, const Input& b) { return a.ns < b.ns; });
    std::sort(shots.begin(), shots.end());
    size_t nextInput = 0, nextShot = 0;
    const uint64_t endNs = seconds > 0 ? uint64_t(seconds * 1e9) : UINT64_MAX;
    auto saveScreen = [&](const std::string& name) {
        std::vector<uint32_t> pixels;
        unsigned w = 0, h = 0;
        std::string why;
        if (!machine->screen(pixels, w, h, why)) {
            std::printf("screen at %.3f s: %s\n", machine->nanoseconds() / 1e9, why.c_str());
            return;
        }
        writePpm(out + "/" + name, pixels, w, h);
        std::printf("screen at %.3f s: %s\n", machine->nanoseconds() / 1e9, name.c_str());
    };
    machine->start(ebootPhysical);
    const auto wallStart = std::chrono::steady_clock::now();
    // Runs in slices of about 7 ms of guest time so scripted input lands near its time.
    constexpr uint64_t slice = 2000000;
    uint64_t left = instructions;
    S3c2442Stop stop = S3c2442Stop::budget;
    auto runUntil = [&](uint64_t untilNs) {
        while (left) {
            const uint64_t now = machine->nanoseconds();
            while (nextInput < inputs.size() && inputs[nextInput].ns <= now) {
                const Input& in = inputs[nextInput++];
                if (in.key) machine->setPin(in.key->port, in.key->bit, !in.down);
                else machine->setStylus(in.down, in.x, in.y);
                std::printf("input at %.3f s: %s %s\n", now / 1e9, in.key ? in.key->name : "stylus", in.down ? "down" : "up");
            }
            while (nextShot < shots.size() && uint64_t(shots[nextShot] * 1e9) <= now) {
                char name[48];
                std::snprintf(name, sizeof name, "screen-%07.3f.ppm", shots[nextShot++]);
                saveScreen(name);
            }
            if (now >= untilNs) return true;
            const uint64_t before = machine->retired() + machine->idleCycles();
            stop = machine->run(std::min(left, slice));
            const uint64_t used = machine->retired() + machine->idleCycles() - before;
            left -= std::min(left, used ? used : 1);
            if (stop != S3c2442Stop::budget) return false;
        }
        return false;
    };
    bool going = runUntil(endNs);
    // --interactive: then commands from stdin, one a line, times from now: "run S", "tap X Y [MS]",
    // "key NAME [MS]", "shot NAME" (NAME.ppm), "peek VA", "stat", "profile S [X]", "aborts N S", "nand" (page reads since the last one), "mmio S [LOW HIGH]", "adc CH VALUE" (an ADC input), "battery SPEC" (as --battery), "battery-state", "power" (sleep, backlight and LEDs), "intclog N", "hard-reset" (store erased from the MBR's block, then power on), "fork" (a checkpoint; quit in the copy returns to it), "quit". Each answers with a line starting "ok" or "error".
    while (going && interactive) {
        char line[256];
        if (!std::fgets(line, sizeof line, stdin)) break;
        char cmd[16] = {}, word[64] = {};
        double sec = 0;
        unsigned x = 0, y = 0, ms = 0;
        const uint64_t now = machine->nanoseconds();
        if (std::sscanf(line, "%15s", cmd) != 1) continue;
        const std::string c = cmd;
        if (c == "quit") break;
        if (c == "run" && std::sscanf(line, "%*s %lf", &sec) == 1) {
            going = runUntil(now + uint64_t(sec * 1e9));
        } else if (c == "tap" && std::sscanf(line, "%*s %u %u %u", &x, &y, &ms) >= 2 && x < 240 && y < 320) {
            inputs.push_back({now, nullptr, true, rx1950TouchRawX(x), rx1950TouchRawY(y)});
            inputs.push_back({now + (ms ? ms : 150) * 1000000ull, nullptr, false, 0, 0});
        } else if (c == "key" && std::sscanf(line, "%*s %63s %u", word, &ms) >= 1 && rx1950Key(word)) {
            inputs.push_back({now, rx1950Key(word), true, 0, 0});
            inputs.push_back({now + (ms ? ms : 200) * 1000000ull, rx1950Key(word), false, 0, 0});
        } else if (c == "peek" && std::sscanf(line, "%*s %63s", word) == 1) {
            const uint32_t va = uint32_t(std::strtoul(word, nullptr, 0));
            uint32_t v = 0;
            if (cpuInspectRead(machine->cpu(), va, &v, 4)) std::printf("peek %08x: %08x\n", va, v);
            else std::printf("peek %08x: not mapped\n", va);
        } else if (c == "profile" && std::sscanf(line, "%*s %lf", &sec) == 1) {
            // Samples the pc every 20000 instructions for sec guest seconds: the top 4 KB pages, with the PID, or
            // the top runs of x bytes with "profile S X".
            std::map<uint64_t, unsigned> pages;
            const uint32_t grain = std::sscanf(line, "%*s %*f %u", &x) == 1 && x ? x : 4096;
            const uint64_t until = now + uint64_t(sec * 1e9);
            unsigned samples = 0;
            while (machine->nanoseconds() < until && left) {
                stop = machine->run(20000);
                left -= std::min<uint64_t>(left, 20000);
                ++pages[uint64_t(cpuGetPid(machine->cpu())) << 32 | (machine->pc() - machine->pc() % grain)];
                ++samples;
                if (stop != S3c2442Stop::budget) { going = false; break; }
            }
            std::vector<std::pair<unsigned, uint64_t>> top;
            for (const auto& [k, n] : pages) top.push_back({n, k});
            std::sort(top.rbegin(), top.rend());
            std::printf("profile: %u samples\n", samples);
            for (size_t i = 0; i < top.size() && i < 25; ++i)
                std::printf("profile %5u pid %08x page %08x\n", top[i].first, uint32_t(top[i].second >> 32), uint32_t(top[i].second));
        } else if (c == "aborts" && std::sscanf(line, "%*s %u %lf", &x, &sec) == 2) {
            // Logs up to x aborts within sec guest seconds: the faulting pc, FAR, FSR, DACR and the L1 entry, then
            // each change the handler makes to that entry and what the L1 entry and TLB hold at the retry.
            auto physical = [&](uint32_t pa, uint32_t* word) {
                if (pa >= 0x30000000 && pa < 0x30100000) return cpuInspectRead(machine->cpu(), 0x8c000000 + (pa - 0x30000000), word, 4);
                if (pa >= 0x30100000 && pa < 0x32000000) return cpuInspectRead(machine->cpu(), 0x80000000 + (pa - 0x30100000), word, 4);
                return false;
            };
            auto tlbFor = [](const Arm920SavedState& st, uint32_t mva) {
                for (size_t i = 0; i + 1 < st.tlb.size(); i += 2)
                    if (st.tlb[i] == (mva & ~0xfffu)) return st.tlb[i + 1];
                return 0xffffffffu;
            };
            const bool hadHistory = history != 0;
            if (!hadHistory) machine->keepHistory(64);
            unsigned logged = 0;
            uint32_t prevPc = 0, faultPc = 0, l1At = 0, l1Seen = 0, faultMva = 0;
            bool pending = false;
            uint64_t step = 0, faultStep = 0;
            machine->stepHook = [&](uint32_t pc) {
                ++step;
                const bool data = (pc & 0xffff) == 0x0010 && (pc >> 16 == 0xffff || pc >> 16 == 0);
                const bool prefetch = (pc & 0xffff) == 0x000c && (pc >> 16 == 0xffff || pc >> 16 == 0);
                if ((data || prefetch) && logged < x) {
                    Arm920SavedState st;
                    cpuArm920CaptureState(machine->cpu(), st);
                    uint32_t l1 = 0;
                    const uint32_t mva = data ? st.far : (prevPc < 0x02000000 ? prevPc | cpuGetPid(machine->cpu()) : prevPc);
                    l1At = (st.ttb & ~0x3fffu) + (mva >> 20) * 4;
                    if (!physical(l1At, &l1)) l1 = 0xdeadbeef;
                    std::printf("abort %u %s at %.6f s pc %08x pid %08x far %08x fsr %03x dacr %08x L1 %08x tlb %08x\n", logged,
                                data ? "data" : "prefetch", machine->nanoseconds() / 1e9, prevPc, cpuGetPid(machine->cpu()),
                                st.far, st.fsr, st.domainCfg, l1, tlbFor(st, mva));
                    ++logged;
                    pending = true; faultPc = prevPc; l1Seen = l1; faultMva = mva; faultStep = step;
                } else if (pending) {
                    uint32_t l1 = 0;
                    if (!physical(l1At, &l1)) l1 = 0xdeadbeef;
                    if (l1 != l1Seen) {
                        std::printf("  L1 for %08x = %08x at pc %08x, step +%" PRIu64 "\n", faultMva, l1, pc, step - faultStep);
                        l1Seen = l1;
                    }
                    if (pc == faultPc) {
                        Arm920SavedState st;
                        cpuArm920CaptureState(machine->cpu(), st);
                        std::printf("  retry at step +%" PRIu64 " pid %08x dacr %08x ttb %08x L1 %08x tlb %08x\n", step - faultStep,
                                    cpuGetPid(machine->cpu()), st.domainCfg, st.ttb, l1, tlbFor(st, faultMva));
                        pending = false;
                    } else if (step - faultStep > 200000) {
                        std::printf("  no retry within 200000 steps\n");
                        pending = false;
                    }
                }
                prevPc = pc;
            };
            const uint64_t until = now + uint64_t(sec * 1e9);
            while (machine->nanoseconds() < until && left && logged < x) {
                stop = machine->run(100000);
                left -= std::min<uint64_t>(left, 100000);
                if (stop != S3c2442Stop::budget) { going = false; break; }
            }
            machine->stepHook = nullptr;
            if (!hadHistory) machine->keepHistory(0);
        } else if (c == "mmio" && std::sscanf(line, "%*s %lf", &sec) == 1) {
            std::map<std::tuple<uint32_t, uint32_t, bool, uint32_t>, unsigned> counts;
            char low[32] = {}, high[32] = {};
            mmioLow = 0, mmioHigh = 0xffffffff;
            if (std::sscanf(line, "%*s %*s %31s %31s", low, high) == 2) {
                mmioLow = uint32_t(std::strtoul(low, nullptr, 0));
                mmioHigh = uint32_t(std::strtoul(high, nullptr, 0));
            }
            mmioCounts = &counts;
            going = runUntil(now + uint64_t(sec * 1e9));
            mmioCounts = nullptr;
            std::vector<std::pair<unsigned, std::tuple<uint32_t, uint32_t, bool, uint32_t>>> top;
            for (const auto& [k, n] : counts) top.push_back({n, k});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 40; ++i)
                std::printf("mmio %7u pc %08x %c %08x %08x\n", top[i].first, std::get<0>(top[i].second), std::get<2>(top[i].second) ? 'W' : 'R',
                            std::get<1>(top[i].second), std::get<3>(top[i].second));
        } else if (c == "adc" && std::sscanf(line, "%*s %u %u", &x, &y) == 2 && x < 8 && y < 1024) {
            machine->adc.channel[x] = uint16_t(y);
        } else if (c == "power") {
            const auto leds = rx1950Leds(*machine);
            std::printf("power: %s, %" PRIu64 " sleeps, %" PRIu64 " wakes (last by %08x), GSTATUS2 %x, backlight %d/1000, LEDs green %d red %d blue %d blinking %d notification %d"
                        " (GPACON %07x GPADAT %07x GPBCON %06x GPBDAT %03x GPJDAT %04x TCON %06x TCNTB0 %u TCMPB0 %u GPFCON %04x GPFDAT %02x EXTINT0 %08x EINTMASK %06x SRCPND %08x INTMSK %08x)\n",
                        machine->asleep() ? "asleep" : "awake", machine->sleepCount(), machine->wakeCount(), machine->lastWakeSources(),
                        machine->gpio.gstatus[2], machine->backlightLevel(), leds.green, leds.red, leds.blue, leds.blinking,
                        rx1950NotificationLed(*machine),
                        machine->gpio.con[S3cGpio::a], machine->gpio.dat[S3cGpio::a], machine->gpio.con[S3cGpio::b],
                        machine->gpio.dat[S3cGpio::b], machine->gpio.dat[S3cGpio::j], machine->pwm.tcon,
                        machine->pwm.timers[0].buffer, machine->pwm.timers[0].compare,
                        machine->gpio.con[S3cGpio::f], machine->gpio.input[S3cGpio::f] & 0xff, machine->gpio.extint[0],
                        machine->gpio.eintmask, machine->intc.srcpnd, machine->intc.intmsk);
        } else if (c == "battery-state") {
            std::printf("battery %s\n", rx1950BatteryState(battery).c_str());
        } else if (c == "battery") {
            BatteryPower parsed;
            const char* spec = line + std::strspn(line, " \t") + 7;
            std::string text(spec);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (!BatteryPower::Parse(text, parsed)) {
                std::printf("error: battery is max, level N or host N (0-100), then charging or battery\n");
                continue;
            }
            battery = parsed;
            rx1950ApplyBattery(*machine, battery);
            std::printf("battery %s\n", rx1950BatteryState(battery).c_str());
        } else if (c == "intclog" && std::sscanf(line, "%*s %u", &x) == 1) {
            for (size_t i = intcEvents.size() > x ? intcEvents.size() - x : 0; i < intcEvents.size(); ++i) {
                const IntcEvent& e = intcEvents[i];
                if (e.address == 0xffffffff)
                    std::printf("intc %.6f pc %08x INT_UART0 %s", e.ns / 1e9, e.pc, e.write ? "latched" : "cleared");
                else
                    std::printf("intc %.6f pc %08x W %08x %08x", e.ns / 1e9, e.pc, e.address, e.value);
                std::printf("  SRCPND %08x SUBSRCPND %04x INTSUBMSK %04x INTMSK %08x UCON0 %04x", e.srcpnd, e.subsrcpnd, e.intsubmsk, e.intmsk,
                            e.ucon0);
                if (e.repeats) std::printf(" x%u", e.repeats + 1);
                std::printf("\n");
            }
        } else if (c == "fork") {
            // A checkpoint: the copy goes on with the commands and its quit comes back here, to the machine
            // as it was at the fork. The copy never writes the NAND store back.
            // The Fast engine's JIT goes first, as its compile thread wouldn't be in the copy, and each side
            // makes a new one.
            std::fflush(nullptr);
            const bool wasFast = machine->fastEngine();
            if (wasFast) {
                machine->setFastEngine(false);
                cpuArm920DropJit(machine->cpu());
            }
            const pid_t child = fork();
            if (wasFast && !machine->setFastEngine(true)) std::fprintf(stderr, "fork: Fast engine not back on\n");
            if (child < 0) { std::printf("error: fork failed\n"); continue; }
            if (child == 0) {
                nandPath = nullptr;
                std::printf("ok fork: in the copy at %.3f s\n", machine->nanoseconds() / 1e9);
                continue;
            }
            int status = 0;
            waitpid(child, &status, 0);
            std::printf("checkpoint resumed\n");
        } else if (c == "hard-reset") {
            // As SocS3C2442::HardReset: the user store erased from the MBR's first FAT block, then power on.
            const size_t mbr = size_t(21) * S3cNand::pagesPerBlock * S3cNand::pageSize;
            const uint32_t block = machine->nand.flash.size() > mbr ? rx1950UserStoreBlockFromMbr(&machine->nand.flash[mbr]) : rx1950UserStoreBlock;
            if (!machine->nand.eraseBlocks(block, S3cNand::blocks - block)) { std::printf("error: erase failed\n"); continue; }
            std::printf("hard-reset: erased blocks %u to %u\n", block, S3cNand::blocks - 1);
            machine->powerOnReset();
        } else if (c == "nand") {
            unsigned reread = 0;
            for (const auto& [row, n] : nandRows) reread += n > 1;
            std::printf("nand: %" PRIu64 " page reads, %zu rows, %u read more than once; %" PRIu64 " programs, %" PRIu64 " erases\n",
                        nandReads, nandRows.size(), reread, machine->nand.programs, machine->nand.erases);
            nandReads = 0;
            nandRows.clear();
        } else if (c == "stat") {
            const auto& adc = machine->adc;
            std::printf("stat: pc %08x pid %08x; ADC %" PRIu64 " conversions, %" PRIu64 " touch interrupts, ADCTSC %03x ADCCON %04x "
                        "stylus %d; SRCPND %08x INTMSK %08x SUBSRCPND %04x INTSUBMSK %04x; timer 3 TCON %08x TCNTB3 %u; idle %" PRIu64 " of %" PRIu64 " cycles\n",
                        machine->pc(), cpuGetPid(machine->cpu()), adc.conversions, adc.touchInterrupts, adc.adctsc, adc.adccon,
                        adc.stylusDown() ? 1 : 0, machine->intc.srcpnd, machine->intc.intmsk, machine->intc.subsrcpnd,
                        machine->intc.intsubmsk, machine->pwm.tcon, machine->pwm.timers[3].buffer, machine->idleCycles(), machine->cycles());
        } else if (c == "shot" && std::sscanf(line, "%*s %63s", word) == 1) {
            saveScreen(std::string(word) + ".ppm");
        } else {
            std::printf("error: %s", line);
            continue;
        }
        std::stable_sort(inputs.begin() + long(nextInput), inputs.end(), [](const Input& a, const Input& b) { return a.ns < b.ns; });
        std::printf("ok %s at %.3f s\n", cmd, machine->nanoseconds() / 1e9);
    }
    for (auto* f : logs) std::fclose(f);
    if (traceLimit) {
        FILE* trace = std::fopen((out + "/mmio.log").c_str(), "w");
        for (uint64_t i = tracedCount > traceLimit ? tracedCount - traceLimit : 0; i < tracedCount; ++i) {
            const Access& a = traced[i % traceLimit];
            std::fprintf(trace, "%08x %c%u %08x %08x\n", a.pc, a.write ? 'W' : 'R', a.size, a.address, a.value);
        }
        std::fclose(trace);
    }
    std::printf("%" PRIu64 " instructions, %" PRIu64 " cycles (%" PRIu64 " idle), %.3f s guest time, pc %08x\n",
                machine->retired(), machine->cycles(), machine->idleCycles(), machine->nanoseconds() / 1e9, machine->pc());
    {
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        std::printf("engine %s: %.1f s wall, %.1f M instructions a second", machine->fastEngine() ? "fast" : "exact", wall,
                    wall > 0 ? machine->retired() / wall / 1e6 : 0.0);
        if (machine->fastEngine())
        {
            uint64_t restarts = 0;
            const unsigned tags = cpuArm920JitPidTags(machine->cpu(), &restarts);
            std::printf("; translated %" PRIu64 ", lean %" PRIu64 ", interpreter %" PRIu64 ", delay skips %" PRIu64 " (%" PRIu64
                        "); PID tags %u, %" PRIu64 " restarts",
                        machine->jitRetired(), machine->leanRetired(), machine->stepRetired(), machine->delaySkips(),
                        machine->delaySkipRetired(), tags, restarts);
        }
        std::printf("\n");
    }
    std::printf("NAND: %" PRIu64 " page programs, %" PRIu64 " block erases; VFP probes %" PRIu64 ", breakpoints %" PRIu64 "\n",
                machine->nand.programs, machine->nand.erases, machine->vfpProbeCount(), machine->breakpointCount());
    if (nandPath && !machine->nand.flash.empty() && !rx1950NandSave(nandPath, machine->nand, error))
        std::fprintf(stderr, "%s\n", error.c_str());
    std::printf("FCLK %" PRIu64 " HCLK %" PRIu64 " PCLK %" PRIu64 " DVS %d\n", machine->clock.fclk(), machine->clock.hclk(),
                machine->clock.pclk(), machine->clock.dvs() ? 1 : 0);
    {
        const uint32_t* r = cpuGetRegisters(machine->cpu());
        std::printf("registers:");
        for (unsigned i = 0; i < 16; ++i) std::printf(" r%u=%08x", i, r[i]);
        std::printf(" cpsr=%08x spsr=%08x pid=%08x\n", cpuGetRegExternal(machine->cpu(), ARM_REG_NUM_CPSR),
                    cpuGetRegExternal(machine->cpu(), ARM_REG_NUM_SPSR), cpuGetPid(machine->cpu()));
    }
    if (digest) {
        // FNV-1a over SDRAM with the data cache cleaned to it, the NAND and the SFR blocks' state.
        auto fnv = [](const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
            return h;
        };
        cpuArm920LeanReset(machine->cpu());
        uint64_t sfr = fnv(&machine->intc, sizeof machine->intc);
        sfr = fnv(&machine->pwm, sizeof machine->pwm, sfr);
        sfr = fnv(&machine->rtc, sizeof machine->rtc, sfr);
        sfr = fnv(machine->steppingstone, sizeof machine->steppingstone, sfr);
        std::printf("digest: retired %" PRIu64 " cycles %" PRIu64 " idle %" PRIu64 " sdram %016" PRIx64 " nand %016" PRIx64
                    " sfr %016" PRIx64 "\n",
                    machine->retired(), machine->cycles(), machine->idleCycles(), fnv(machine->sdram(), 32u << 20),
                    fnv(machine->nand.flash.data(), machine->nand.flash.size()), sfr);
    }
    if (history) {
        std::printf("last pcs:");
        for (const uint32_t pc : machine->recentPcs()) std::printf(" %08x", pc);
        std::printf("\n");
    }
    for (const uint32_t va : peeks) {
        uint32_t word = 0;
        if (cpuInspectRead(machine->cpu(), va, &word, 4)) std::printf("peek %08x: %08x\n", va, word);
        else std::printf("peek %08x: not mapped\n", va);
    }
    {
        saveScreen("screen.ppm");
        std::printf("LCD: frame at %08x, LCDCON1 %08x, LCDCON5 %08x, LCDSADDR3 %08x\n", machine->lcd.frameAddress(),
                    machine->lcd.lcdcon[0], machine->lcd.lcdcon[4], machine->lcd.saddr[2]);
        std::printf("ADC: %" PRIu64 " conversions, channels %02x, first %d, %" PRIu64 " touch interrupts, ADCTSC %03x; SD commands %" PRIu64 "; Wi-Fi bus accesses %" PRIu64 "\n",
                    machine->adc.conversions, machine->adc.channelsRead, machine->adc.firstChannel,
                    machine->adc.touchInterrupts, machine->adc.adctsc, machine->sdi.commands, machine->wifiAccesses);
        for (const auto& [mux, seen] : adcStarts)
            std::printf("ADC start: channel %u at %.3f s pc %08x ADCCON %04x\n", mux, seen.ns / 1e9, seen.pc, seen.value);
        if (gpfReads)
            std::printf("GPFDAT: %" PRIu64 " reads, first at %.3f s pc %08x = %02x\n", gpfReads, firstGpf.ns / 1e9, firstGpf.pc, firstGpf.value);
        if (gpjReads)
            std::printf("GPJDAT: %" PRIu64 " reads, first at %.3f s pc %08x = %04x\n", gpjReads, firstGpj.ns / 1e9, firstGpj.pc, firstGpj.value);
        std::printf("EXTINT0 %08x EXTINT1 %08x EXTINT2 %08x EINTMASK %08x GPFCON %08x GPGCON %08x\n",
                    machine->gpio.extint[0], machine->gpio.extint[1], machine->gpio.extint[2], machine->gpio.eintmask,
                    machine->gpio.con[S3cGpio::f], machine->gpio.con[S3cGpio::g]);
    }
    if (!walks.empty()) {
        // The kernel's own mappings of SDRAM (OEMAddressTable): 0x8c000000 the first MB, 0x80000000 the rest.
        auto physical = [&](uint32_t pa, uint32_t* word) {
            if (pa >= 0x30000000 && pa < 0x30100000) return cpuInspectRead(machine->cpu(), 0x8c000000 + (pa - 0x30000000), word, 4);
            if (pa >= 0x30100000 && pa < 0x32000000) return cpuInspectRead(machine->cpu(), 0x80000000 + (pa - 0x30100000), word, 4);
            return false;
        };
        Arm920SavedState state;
        cpuArm920CaptureState(machine->cpu(), state);
        for (const uint32_t mva : walks) {
            uint32_t l1 = 0, l2 = 0;
            const uint32_t l1At = (state.ttb & ~0x3fffu) + (mva >> 20) * 4;
            if (!physical(l1At, &l1)) { std::printf("walk %08x: L1 at %08x not readable\n", mva, l1At); continue; }
            std::printf("walk %08x: ttb %08x L1[%08x] = %08x", mva, state.ttb, l1At, l1);
            const uint32_t l2At = (l1 & 3) == 1 ? (l1 & ~0x3ffu) + ((mva >> 12) & 0xff) * 4
                                : (l1 & 3) == 3 ? (l1 & ~0xfffu) + ((mva >> 10) & 0x3ff) * 4 : 0;
            if (l2At && physical(l2At, &l2)) std::printf(" L2[%08x] = %08x", l2At, l2);
            std::printf("\n");
        }
    }
    {
        // Text is only there once the kernel has mapped it; an unmapped ring leaves nk.log empty.
        uint32_t indices[2] = {0, 0};
        std::vector<uint8_t> ring(nkRingSize);
        FILE* f = std::fopen((out + "/nk.log").c_str(), "wb");
        const bool mapped = cpuInspectRead(machine->cpu(), nkRingIndices, indices, sizeof indices) &&
                            cpuInspectRead(machine->cpu(), nkRing, ring.data(), nkRingSize);
        if (mapped) std::printf("kernel log: %u bytes\n", ((indices[1] - indices[0]) & 0xffff));
        else std::printf("kernel log: not mapped\n");
        if (f && mapped)
            for (uint32_t i = indices[0] & 0xffff; i != (indices[1] & 0xffff); i = (i + 1) & 0xffff) std::fputc(ring[i], f);
        if (f) std::fclose(f);
    }
    std::printf("stop: %s\n", machine->stopText().c_str());
    return stop == S3c2442Stop::refused ? 3 : 0;
}
