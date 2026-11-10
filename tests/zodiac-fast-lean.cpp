// The Zodiac's Fast engine against Exact on one MMU guest: plain loops run on the lean path, a page table
// change only shows after a CP15 TLB flush, and a cacheable load of unbacked memory stops both engines at
// the same pc with the same address.
#define ZODIAC_STATE_MAIN stateMain
#include "zodiac-machine-state.cpp"

namespace {
// 200 rounds of: load [r2], add it to reg.
void sumLoop(std::vector<uint32_t>& words, unsigned reg) {
    words.push_back(0xe3a00000 | reg << 12);  // mov reg, #0
    words.push_back(0xe3a070c8);              // mov r7, #200
    const size_t top = words.size();
    words.push_back(0xe5925000);              // ldr r5, [r2]
    words.push_back(0xe0800005 | reg << 16 | reg << 12);
    words.push_back(0xe2577001);              // subs r7, r7, #1
    words.push_back(0x1a000000 | (uint32_t(int32_t(top) - int32_t(words.size()) - 2) & 0xffffff));
}

// The graphics guest's setup with programmed clocks gives the step quiet spans, which the lean path runs.
// The test code follows it, running from the linked alias at 0x20000000.
std::vector<uint32_t> guest(uint32_t& stopPc) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    // SDRAM bank 0: precharge, eight refreshes, mode, normal.
    load(words, 0, 0x00221000);
    load(words, 1, 0x91120200); words.push_back(0xe5801000);
    load(words, 2, 0x08200000); words.push_back(0xe5923000);
    load(words, 1, 0xa1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08000000);
    for (unsigned i = 0; i < 8; ++i) words.push_back(0xe5923000);
    load(words, 1, 0xb1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08111800); words.push_back(0xe5923000);
    load(words, 1, 0x81129200); words.push_back(0xe5801000);
    // The same offset in SRAM and SDRAM holds different words.
    load(words, 0, 0x00308100); store(words, 0, 0, 0x11111111);
    load(words, 0, 0x08008100); store(words, 0, 0, 0x22222222);
    // Sections: 2 peripherals, 3 SRAM cached, 4 SRAM uncached, 5 SRAM cached, 6 unbacked SDRAM cached,
    // 0x160 graphics, 0x200 ROM.
    load(words, 0, 0x00304000);
    store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c1e);
    store(words, 0, 0x10, 0x00300c12); store(words, 0, 0x14, 0x00300c1e); store(words, 0, 0x18, 0x0a000c1e);
    load(words, 0, 0x00304580); store(words, 0, 0, 0x16000c12);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                       // TTB
    load(words, 1, 1); words.push_back(0xee031f10);    // domain 0 client
    load(words, 1, 0x107d); words.push_back(0xee011f10);  // MMU, D- and I-cache on
    load(words, 2, 0x00508100);
    sumLoop(words, 8);                                 // r8 = 200 * SRAM word
    load(words, 6, 0x00404014);
    load(words, 1, 0x08000c1e); words.push_back(0xe5861000);  // section 5 now maps SDRAM, the TLB keeps the old one
    sumLoop(words, 9);                                 // r9 still sums the SRAM word
    words.push_back(0xe3a00000);                       // mov r0, #0: the operand is SBZ
    words.push_back(0xee070f16);                       // invalidate the D-cache, its lines are virtually tagged
    words.push_back(0xee080f17);                       // invalidate the TLBs
    sumLoop(words, 10);                                // r10 = 200 * SDRAM word
    load(words, 11, 0x00600040);
    stopPc = 0x20000000 + g.cursor + uint32_t(words.size()) * 4;
    words.push_back(0xe59bc000);                       // ldr r12, [r11]: cacheable and unbacked
    words.push_back(0xe7f000f0);
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    return g.words;
}

struct Outcome {
    Imx1MachineRun run;
    Imx1MachineCheapStatus cheap;
    uint64_t lean;
};

Outcome runGuest(const std::vector<uint32_t>& words, bool fast) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, nullptr, nullptr, &error);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    // Small slices, so lean spans end at slice edges as well as at the guest's own exits.
    for (unsigned n = 0; n < 100000 && imx1MachineGetStatus(m).status == Imx1MachineStatus::yielded; ++n)
        imx1MachineRunSlice(m, 997, 0);
    Outcome out{imx1MachineInspect(m), imx1MachineGetStatus(m), imx1MachineLeanRetired(m)};
    imx1MachineStop(m);
    return out;
}
}  // namespace

int main() {
    uint32_t stopPc = 0;
    const auto words = guest(stopPc);
    const auto exact = runGuest(words, false), fast = runGuest(words, true);
    for (const auto* o : {&exact, &fast})
        std::printf("%s: stop=%s pc=%08x address=%08x retired=%llu ticks=%llu lean=%llu r8=%08x r9=%08x r10=%08x\n",
                    o == &exact ? "exact" : "fast", imx1ProbeStopName(o->run.result.stop), o->run.result.trace.pc,
                    o->run.result.address, (unsigned long long)o->cheap.retired, (unsigned long long)o->cheap.time.ticks,
                    (unsigned long long)o->lean, o->run.result.registers[8], o->run.result.registers[9],
                    o->run.result.registers[10]);
    std::fflush(stdout);
    const auto& e = exact.run.result;
    const auto& f = fast.run.result;
    assert(exact.lean == 0 && fast.lean > 1000);
    assert(e.registers[8] == 200u * 0x11111111u && e.registers[9] == 200u * 0x11111111u &&
           e.registers[10] == 200u * 0x22222222u);
    assert(!std::memcmp(e.registers, f.registers, sizeof(e.registers)) && e.cpsr == f.cpsr);
    assert(e.stop == Imx1ProbeStop::unsupportedDataCache && f.stop == e.stop);
    assert(e.trace.pc == stopPc && f.trace.pc == e.trace.pc && f.address == e.address);
    assert(exact.cheap.retired == fast.cheap.retired && exact.cheap.status == fast.cheap.status);
    assert(exact.cheap.time.ticks == fast.cheap.time.ticks);
    std::puts("PASS: Fast lean loops, CP15 TLB flush, stop pc and unsupported access match Exact");
}
