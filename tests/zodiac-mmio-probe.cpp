#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "imx1_boot_probe.h"

namespace {
struct Trace {
    std::vector<Imx1ProbeTrace> instructions;
    std::vector<Imx1ProbeAccess> accesses;
};
void instruction(void* context, const Imx1ProbeTrace& trace) {
    static_cast<Trace*>(context)->instructions.push_back(trace);
}
void access(void* context, const Imx1ProbeAccess& trace) {
    static_cast<Trace*>(context)->accesses.push_back(trace);
}
[[maybe_unused]] void compare(const Imx1ProbeResult& a, const Imx1ProbeResult& b, const Trace& x, const Trace& y) {
    if (a.busAccesses != b.busAccesses || a.peripheralStatus != b.peripheralStatus) {
        std::fprintf(stderr, "bus mismatch: Exact=%llu Fast=%llu, statuses=%u/%u\n",
                     (unsigned long long)a.busAccesses, (unsigned long long)b.busAccesses, unsigned(a.peripheralStatus), unsigned(b.peripheralStatus));
        for (size_t i = 0; (i < x.accesses.size() || i < y.accesses.size()) && i < 100; ++i) {
            for (const auto* t : {&x, &y}) {
                if (i >= t->accesses.size()) continue;
                const auto& e = t->accesses[i];
                std::fprintf(stderr, "%s %zu step=%u pc=%08x %c %08x size=%u value=%08x status=%u\n",
                             t == &x ? "Exact" : "Fast", i, e.step, e.pc, e.write ? 'W' : 'R',
                             e.address, e.size, e.value, unsigned(e.status));
            }
        }
    }
    assert(a.stop == b.stop && a.address == b.address && a.size == b.size && a.value == b.value);
    assert(a.peripheralStatus == b.peripheralStatus && a.busAccesses == b.busAccesses);
    assert(a.cpsr == b.cpsr && a.spsr == b.spsr);
    assert(!std::memcmp(a.registers, b.registers, sizeof(a.registers)));
    assert(x.instructions.size() == y.instructions.size() && x.accesses.size() == y.accesses.size());
    for (size_t i = 0; i < x.instructions.size(); ++i) {
        const auto& l = x.instructions[i]; const auto& r = y.instructions[i];
        assert(l.step == r.step && l.pc == r.pc && l.instruction == r.instruction &&
               l.cpsr == r.cpsr && l.operand == r.operand && l.instructionSize == r.instructionSize);
    }
    for (size_t i = 0; i < x.accesses.size(); ++i) {
        const auto& l = x.accesses[i]; const auto& r = y.accesses[i];
        assert(l.step == r.step && l.pc == r.pc && l.address == r.address && l.size == r.size &&
               l.write == r.write && l.value == r.value && l.status == r.status);
    }
}
Imx1ProbeResult probe(const std::vector<uint8_t>& rom, Imx1ProbeEntry entry, bool fast,
                      Imx1ProbeOptions options, Trace& trace, uint32_t budget = 100000) {
    options.busObserver = access;
    options.busContext = &trace;
    return imx1BootProbe(rom.data(), rom.size(), entry, budget, instruction, &trace, fast, &options);
}
void privateRom(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    assert(input && input.tellg() == 0x7d0000);
    input.seekg(0);
    std::vector<uint8_t> rom(0x7d0000);
    assert(input.read(reinterpret_cast<char*>(rom.data()), rom.size()));
    const auto original = rom;
    for (auto entry : {Imx1ProbeEntry::physicalFlash, Imx1ProbeEntry::resetAlias, Imx1ProbeEntry::linkedAlias}) {
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            Imx1ProbeOptions options{};
            options.bootstrapPeripherals = true;
            options.gpioDInputKnown = scenario != 0;
            options.gpioDInput = scenario == 1 ? 0x600 : scenario == 2 ? 0x640 : 0;
            options.ramMiB = scenario == 1 ? 128 : scenario == 2 ? 32 : 0;
            Trace exact;
            const auto a = probe(rom, entry, false, options, exact);
#if FROND_TEST_FAST
            Trace fast;
            const auto b = probe(rom, entry, true, options, fast);
            compare(a, b, exact, fast);
            assert(b.jitBlocks && b.jitInstructions && !a.jitBlocks && !a.jitInstructions);
#endif
            assert(rom == original);
            std::printf("PASS: private MMIO entry=%u scenario=%u gpio-d=%08x ram-mib=%u instructions=%zu accesses=%zu stop=%s address=%08x value=%08x peripheral=%s\n",
                        unsigned(entry), scenario, options.gpioDInput, options.ramMiB,
                        exact.instructions.size(), exact.accesses.size(), imx1ProbeStopName(a.stop),
                        a.address, a.value, imx1BootstrapAccessName(a.peripheralStatus));
        }
    }
}

void blockTransfer(bool internal) {
    std::vector<uint32_t> words;
    auto load = [&](unsigned reg, uint32_t value) {
        words.push_back(0xe59f0000 | (reg << 12));
        words.push_back(0xea000000);
        words.push_back(value);
        return words.size() - 1;
    };
    load(0, 0x00221000);
    load(1, 0x91120200); words.push_back(0xe5801000);
    load(2, 0x08200000); words.push_back(0xe5923000);
    load(1, 0xa1120200); words.push_back(0xe5801000);
    load(2, 0x08000000);
    for (unsigned i = 0; i < 8; ++i) words.push_back(0xe5923000);
    load(1, 0xb1120200); words.push_back(0xe5801000);
    load(2, 0x08111800); words.push_back(0xe5923000);
    load(1, 0x81129200); words.push_back(0xe5801000);
    const size_t sourceLiteral = load(0, 0);
    words.push_back(0xe8b0000e);  // LDMIA r0!, {r1,r2,r3}
    load(4, 0x08000000);
    words.push_back(0xe8a4000e);  // STMIA r4!, {r1,r2,r3}
    load(4, 0x08000000);
    words.push_back(0xe8b400e0);  // LDMIA r4!, {r5,r6,r7}
    words.push_back(0xe1a0c004);  // MOV r12, r4
    load(4, internal ? 0x00300100 : 0x08000100);
    load(1, 0xe3a08001); words.push_back(0xe5841000);  // MOV r8, #1 in SDRAM
    load(1, 0xe1a0f009); words.push_back(0xe5841004);  // MOV pc, r9 in SDRAM
    const size_t firstReturn = load(9, 0);
    words.push_back(0xe12fff14);  // BX r4
    words[firstReturn] = 0x10000000 + words.size() * 4;
    words.push_back(0xe1a0a008);  // MOV r10, r8
    load(1, 0xe3a08002); words.push_back(0xe5841000);  // Replace cached translation from flash.
    const size_t secondReturn = load(9, 0);
    words.push_back(0xe12fff14);
    words[secondReturn] = 0x10000000 + words.size() * 4;
    words.push_back(0xe1a0b008);  // MOV r11, r8
    const uint32_t stack = internal ? 0x00301000 : 0x08001000;
    load(13, stack);
    const size_t stackReturn = load(14, 0);
    words.push_back(0xe92d40e0);  // STMDB sp!, {r5,r6,r7,lr}
    words.push_back(0xe3a05000); words.push_back(0xe3a06000); words.push_back(0xe3a07000);
    words.push_back(0xe8bd80e0);  // LDMIA sp!, {r5,r6,r7,pc}
    words.push_back(0xe3a05000);  // Must be skipped by the stack return.
    words[stackReturn] = 0x10000000 + words.size() * 4;
    words.push_back(0xeafffffe);
    words[sourceLiteral] = 0x10000000 + words.size() * 4;
    for (uint32_t v : {0x12345678u, 0x9abcdef0u, 0x55aa55aau}) words.push_back(v);
    std::vector<uint8_t> rom(words.size() * 4);
    std::memcpy(rom.data(), words.data(), rom.size());
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    options.ramMiB = 32;
    Trace exact;
    const auto a = probe(rom, Imx1ProbeEntry::physicalFlash, false, options, exact, 100);
    assert(a.stop == Imx1ProbeStop::budget);
    assert(a.registers[5] == 0x12345678 && a.registers[6] == 0x9abcdef0 && a.registers[7] == 0x55aa55aa);
    assert(a.registers[12] == 0x0800000c && a.registers[0] == words[sourceLiteral] + 12);
    assert(a.registers[10] == 1 && a.registers[11] == 2);
    assert(a.registers[13] == stack);
#if FROND_TEST_FAST
    Trace fast;
    const auto b = probe(rom, Imx1ProbeEntry::physicalFlash, true, options, fast, 100);
    compare(a, b, exact, fast);
    assert(b.jitBlocks && b.jitInstructions);
#endif
    std::puts("PASS: ARMv4 block copy, writeback, RAM code execution and replacement with engine parity");
}

void resetAlias() {
    // The zero bootstrap alias ends at 1 MiB, before the physical MMIO windows.
    std::vector<uint8_t> rom(0x0021c324 + 4);
    const uint32_t code[] = {0xe59f0000, 0xe5901000, 0x0021c320};
    std::memcpy(rom.data(), code, sizeof(code));
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    Trace trace;
    auto r = probe(rom, Imx1ProbeEntry::resetAlias, false, options, trace, 2);
    assert(r.stop == Imx1ProbeStop::budget && r.registers[1] == 0xffffffff);
    assert(trace.accesses.size() == 1 && trace.accesses[0].address == 0x0021c320);
    const uint32_t limit = 0x00100000;
    std::memcpy(rom.data() + 8, &limit, 4);
    trace = {};
    r = probe(rom, Imx1ProbeEntry::resetAlias, false, options, trace, 2);
    assert(r.stop == Imx1ProbeStop::unsupportedRead && r.address == limit);
    std::puts("PASS: reset alias ends before reserved memory and physical MMIO");
}

void speculativeContinuation() {
    std::vector<uint32_t> words;
    auto load = [&](unsigned reg, uint32_t value) {
        words.push_back(0xe59f0000 | (reg << 12));
        words.push_back(0xea000000);
        words.push_back(value);
    };
    load(0, 0x00304800); load(1, 0x00300c02); words.push_back(0xe5801000);
    load(0, 0x00300100); load(1, 0xe3a07042); words.push_back(0xe5801000);
    load(1, 0xeafffffe); words.push_back(0xe5801004);
    load(0, 0x00304000); words.push_back(0xee020f10);
    words.push_back(0xe3a00001); words.push_back(0xee030f10);
    load(1, 0x20000100); words.push_back(0xe3a00079);
    words.push_back(0xee010f10); words.push_back(0xe12fff11); // Last two ROM words.
    std::vector<uint8_t> rom(words.size() * 4);
    std::memcpy(rom.data(), words.data(), rom.size());
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    Trace exact;
    const auto a = probe(rom, Imx1ProbeEntry::physicalFlash, false, options, exact, 100);
    assert(a.stop == Imx1ProbeStop::budget && a.registers[7] == 0x42);
#if FROND_TEST_FAST
    Trace fast;
    const auto b = probe(rom, Imx1ProbeEntry::physicalFlash, true, options, fast, 100);
    compare(a, b, exact, fast);
    assert(b.jitBlocks && b.jitInstructions);
#endif
    std::puts("PASS: unreachable speculative fetch beyond ROM does not stop MMU handoff");
}
}  // namespace

int main(int argc, char** argv) {
    unsetenv("FROND_JIT");
    const uint32_t words[] = {0xe59f0014, 0xe3a01000, 0xe5801000, 0xe5902000,
                              0xe59f0008, 0xe5903000, 0xeafffffe, 0x00205034, 0x0021c324};
    std::vector<uint8_t> rom(sizeof(words));
    std::memcpy(rom.data(), words, sizeof(words));
    Imx1ProbeOptions options{};
    Trace baseline;
    auto r = probe(rom, Imx1ProbeEntry::physicalFlash, false, options, baseline, 10);
    assert(r.stop == Imx1ProbeStop::unsupportedWrite && r.address == 0x00205034);
    options.bootstrapPeripherals = true;
    Trace exact;
    r = probe(rom, Imx1ProbeEntry::physicalFlash, false, options, exact, 10);
    assert(r.stop == Imx1ProbeStop::unsupportedPeripheral && r.address == 0x0021c324);
    assert(r.peripheralStatus == Imx1BootstrapAccess::externalInput && exact.accesses.size() == 3);
    options.gpioDInputKnown = true;
    options.gpioDInput = 0x640;
    exact = {};
    r = probe(rom, Imx1ProbeEntry::physicalFlash, false, options, exact, 10);
    assert(r.stop == Imx1ProbeStop::budget && r.registers[3] == 0x640);
#if FROND_TEST_FAST
    Trace fast;
    const auto f = probe(rom, Imx1ProbeEntry::physicalFlash, true, options, fast, 10);
    compare(r, f, exact, fast);
    assert(f.jitBlocks && f.jitInstructions);
#endif
    std::puts("PASS: optional peripheral probe, baseline rejection and ordered engine parity");
    blockTransfer(false);
    blockTransfer(true);
    resetAlias();
    speculativeContinuation();
    if (argc > 1) privateRom(argv[1]);
}
