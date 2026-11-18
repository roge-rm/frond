// Boots the HP iPAQ rx1950's bootloader on the S3C2442 machine and keeps what its UARTs send.
//
//   rx1950-boot --cebl CEBL.nbf [--ceos CEOS.nbf] [--nand FILE] [--out DIR] [--instructions N]
//               [--mmio-trace N] [--history N] [--debug-messages] [--any-firmware] [--peek VA]... [--walk MVA]... [--watch PA]...
//               [--seconds S] [--tap T,X,Y[,MS]]... [--key T,NAME[,MS]]... [--shot T]... [--interactive]
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
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "CPU.h"
#include "arm920_state.h"
#include "ZodiacRom.h"
#include "rx1950_board.h"
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
    auto machine = S3c2442Machine::create(options, error);
    if (!machine) { std::fprintf(stderr, "machine: %s\n", error.c_str()); return 1; }
    machine->loadRam(ebootPhysical, bl.data() + nbfHeader, ebootBytes);
    // ASSUMPTION "rx1950 IPL clocks": the block 0 loader sets the PLL before eboot, which never does.
    // 296.35 MHz from the 16.9344 MHz crystal (MDIV 97, PDIV 1, SDIV 2), FCLK:HCLK:PCLK 1:3:6.
    machine->clock.write(0x04, 0x61012);
    machine->clock.write(0x14, 0x7);
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
    auto observe = [&](uint32_t pc, uint32_t address, unsigned, bool write, uint32_t value) {
        if (write && address == 0x58000000 && (value & 1) && adcStarts.size() < 16 && !(machine->adc.adctsc & 7))
            adcStarts.push_back({(value >> 3) & 7, {pc, machine->nanoseconds(), value}});
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
    // "key NAME [MS]", "shot NAME" (NAME.ppm), "peek VA", "stat", "quit". Each answers with a line starting "ok" or "error".
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
