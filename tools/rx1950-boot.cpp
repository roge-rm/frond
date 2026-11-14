// Boots the HP iPAQ rx1950's bootloader on the S3C2442 machine and keeps what its UARTs send.
//
//   rx1950-boot --cebl CEBL.nbf [--ceos CEOS.nbf] [--out DIR] [--instructions N] [--mmio-trace N]
//               [--history N] [--debug-messages] [--peek VA]...
//
// CEBL.nbf's eboot goes straight into SDRAM at 0x30080000 and runs from there with the MMU off and the
// PLL set, as the block 0 loader would leave it (that loader isn't in either dump). CEOS.nbf, if given, fills the NAND
// (s3c2442_nand.h) as rx1950NandImage lays it out. DIR gets uart0.log, uart1.log, uart2.log and, with
// --mmio-trace, the last N SFR accesses in mmio.log, and nk.log what the kernel printed. The last line
// printed says why the run stopped.
//
// The retail kernel only sends its debug output to a serial port on the debug board (a Super I/O chip on
// nGCS5 it probes for). Without one it keeps the text in a 64 KB ring in RAM, which nk.log is, when
// --debug-messages turns it on in the board settings page (rx1950_nand_image.h).

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "CPU.h"
#include "rx1950_nand_image.h"
#include "s3c2442_machine.h"

namespace {
    constexpr uint32_t nbfHeader = 0xa5a, ebootBytes = 256 * 1024, ebootPhysical = 0x30080000;
    // The kernel's debug text ring in the WM5 CEOS.nbf (kernel built Jun 1 2005): 64 KB of text and its
    // read and write indices, found in OEMWriteDebugByte.
    constexpr uint32_t nkRing = 0x802d2b40, nkRingIndices = 0x80292e28, nkRingSize = 0x10000;

    bool readFile(const char* path, std::vector<uint8_t>& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }
}

int main(int argc, char** argv) {
    const char* cebl = nullptr;
    const char* ceos = nullptr;
    std::string out = ".";
    uint64_t instructions = 2000000000ull;
    uint64_t traceLimit = 0;
    size_t history = 0;
    Rx1950NandOptions nandOptions;
    std::vector<uint32_t> peeks;  // virtual addresses whose words are printed when the run stops
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
        else if (a == "--peek") peeks.push_back(uint32_t(std::strtoul(next(), nullptr, 0)));
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (!cebl) {
        std::fprintf(stderr, "usage: rx1950-boot --cebl CEBL.nbf [--ceos CEOS.nbf] [--out DIR] [--instructions N] [--mmio-trace N] [--history N] [--debug-messages] [--peek VA]...\n");
        return 2;
    }
    std::vector<uint8_t> bl;
    if (!readFile(cebl, bl) || bl.size() != nbfHeader + ebootBytes) {
        std::fprintf(stderr, "%s: not the rx1950's CEBL.nbf (%zu bytes)\n", cebl, bl.size());
        return 1;
    }
    std::string error;
    S3c2442Options options;
    auto machine = S3c2442Machine::create(options, error);
    if (!machine) { std::fprintf(stderr, "machine: %s\n", error.c_str()); return 1; }
    machine->loadRam(ebootPhysical, bl.data() + nbfHeader, ebootBytes);
    // ASSUMPTION "rx1950 IPL clocks": the block 0 loader sets the PLL before eboot, which never does.
    // 296.35 MHz from the 16.9344 MHz crystal (MDIV 97, PDIV 1, SDIV 2), FCLK:HCLK:PCLK 1:3:6.
    machine->clock.write(0x04, 0x61012);
    machine->clock.write(0x14, 0x7);
    if (ceos) {
        std::vector<uint8_t> os;
        if (!readFile(ceos, os)) { std::fprintf(stderr, "%s: can't read\n", ceos); return 1; }
        std::vector<uint8_t> flash;
        if (!rx1950NandImage(bl.data() + nbfHeader, ebootBytes, os.data(), os.size(), flash, error, nandOptions)) {
            std::fprintf(stderr, "%s: %s\n", ceos, error.c_str());
            return 1;
        }
        machine->nand.setImage(std::move(flash));
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
    if (traceLimit)
        machine->mmioTrace = [&](uint32_t pc, uint32_t address, unsigned size, bool write, uint32_t value) {
            traced[tracedCount++ % traceLimit] = {pc, address, value, size, write};
        };

    if (history) machine->keepHistory(history);
    machine->start(ebootPhysical);
    const auto stop = machine->run(instructions);
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
    std::printf("FCLK %" PRIu64 " HCLK %" PRIu64 " PCLK %" PRIu64 " DVS %d\n", machine->clock.fclk(), machine->clock.hclk(),
                machine->clock.pclk(), machine->clock.dvs() ? 1 : 0);
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
