#ifndef FROND_TWGLUE_LOG_H
#define FROND_TWGLUE_LOG_H

// FROND_TWGLUE_LOG=FILE: a text log of the calls a Tapwave Native Application makes through the TwGlue table
// and through the r9 module tables (DAL, Boot, UI). It rides the Zodiac machine's per-instruction observer, so
// the machine runs one instruction at a time on the interpreter while it is on. Only call data is logged.
#include "CPU.h"
#include "MMU.h"
#include "imx1_boot_probe.h"
#include "imx1_reference_time.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TwGlueLog {
public:
    static constexpr uint32_t slots = 612;
    static constexpr uint32_t call68kIndex = 1000;

    // Reads physical memory with no side effect: false where it isn't plain ROM or RAM.
    using PhysicalRead = bool (*)(void* context, uint32_t pa, void* data, uint32_t size);

    TwGlueLog(ArmCpu* cpu, const Imx1ReferenceTime* time, FILE* out, PhysicalRead physical, void* physicalContext)
        : cpu(cpu), time(time), out(out), physical(physical), physicalContext(physicalContext) {}
    ~TwGlueLog() { if (out) std::fclose(out); }

    static const char* name(uint32_t index) {
        static const char* const names[] = {
#include "twglue_names.inc"
        };
        if (index == call68kIndex) return "Call68KFunc";
        return index < slots ? names[index] : "?";
    }

    // The observer: called before each instruction runs.
    void step(const Imx1ProbeTrace& t) {
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = t.pc & ~1u;
        const bool arm = t.instructionSize == 4;
        const bool jumped = pc != expectedPc;
        expectedPc = pc + t.instructionSize;
        if (!frames.empty()) returns(pc, r);
        lastPc = pc;
        // The ROM's TwLoadModule (QuickRun) calls the TNA header with r0 = 0 and the TwGlue table (in RAM) in r1,
        // r2 = 0. The second call, PilotMain, has emulStateP in r0 and Call68KFunc in r2.
        if (jumped && arm && !(pc & 3) && (pc >> 24) != romRegion && r[0] == 0 && r[1] >= 0x1000 && entryHeader(pc)) {
            const bool table = looksLikeTable(r[1]);
            if (table && !(r[1] == tableBase && pc == lastEntry)) entry(pc, r);
        } else if (jumped && tableBase && pc == lastEntry && r[0]) {
            secondEntry(r);
        }
        if (!tableBase) return;
        // Only calls from game code: the ROM calls many of the same functions itself.
        if (pc >= lowest && pc <= highest && (r[14] >> 24) != romRegion) {
            const auto it = targets.find(pc);
            if (it != targets.end()) call(it->second, pc, r);
        }
        if (arm && (t.instruction & 0xfff0f000) == 0xe590f000 && ((r[14] >> 24) != romRegion || (pc >> 24) != romRegion))
            moduleCall(t.instruction, r);
    }

private:
    static constexpr uint32_t romRegion = 0x20;
    struct Frame { uint32_t lr, sp, index; };

    ArmCpu* cpu;
    const Imx1ReferenceTime* time;
    FILE* out;
    PhysicalRead physical;
    void* physicalContext;
    uint32_t lastEntry = 0, lastPc = 0, expectedPc = 0, tableBase = 0, lowest = ~0u, highest = 0, lines = 0, entries = 0;
    std::unordered_map<uint32_t, std::vector<uint32_t>> targets;
    std::vector<Frame> frames;

    uint64_t ms() const { return time->ticks * 1000 / 32768; }
    // The page tables are walked here through the physical reader: the Zodiac's memory is one bus callback, so
    // cpuInspectRead and the MMU's inspect walk refuse it.
    bool translate(uint32_t va, uint32_t& pa) {
        const uint32_t ttb = mmuGetTTP(cpuGetMMU(cpu));
        if (ttb == MMU_DISABLED_TTP) { pa = va; return true; }
        uint32_t first, second;
        if (!physical(physicalContext, (ttb & ~0x3fffu) + (va >> 20) * 4, &first, 4)) return false;
        switch (first & 3) {
        case 2: pa = (first & 0xfff00000u) | (va & 0xfffff); return true;
        case 1: if (!physical(physicalContext, (first & 0xfffffc00u) + ((va >> 12) & 0xff) * 4, &second, 4)) return false; break;
        case 3: if (!physical(physicalContext, (first & 0xfffff000u) + ((va >> 10) & 0x3ff) * 4, &second, 4)) return false; break;
        default: return false;
        }
        switch (second & 3) {
        case 1: pa = (second & 0xffff0000u) | (va & 0xffff); return true;
        case 2: pa = (second & 0xfffff000u) | (va & 0xfff); return true;
        case 3: pa = (second & 0xfffffc00u) | (va & 0x3ff); return true;
        default: return false;
        }
    }

    bool read(uint32_t address, void* data, uint32_t size) {
        for (uint32_t offset = 0; offset < size;) {
            const uint32_t va = address + offset, count = std::min<uint32_t>(0x400 - (va & 0x3ff), size - offset);
            uint32_t pa;
            if (va < address || !translate(va, pa) || !physical(physicalContext, pa, static_cast<uint8_t*>(data) + offset, count))
                return false;
            offset += count;
        }
        return true;
    }

    // The first words of the table are ROM function addresses (Thumb bit allowed).
    bool looksLikeTable(uint32_t address) {
        uint32_t w[4];
        if (!read(address, w, sizeof w)) return false;
        for (const uint32_t v : w)
            if ((v >> 23) != (0x20000000u >> 23)) return false;
        return true;
    }

    bool entryHeader(uint32_t pc) {
        uint32_t magic;
        return read(pc + 28, &magic, 4) && magic == 0x54574e41;  // "ANWT"
    }

    void flushSometimes() { if (!(++lines & 255)) std::fflush(out); }

    // Printable text a value points at, as ` "text"`, or nothing.
    std::string text(uint32_t value) {
        if (value < 0x1000) return {};
        char buf[40];
        if (!read(value, buf, 33)) return {};
        uint32_t n = 0;
        while (n < 33 && buf[n] && uint8_t(buf[n]) >= 32 && uint8_t(buf[n]) < 127) ++n;
        if (n < 3 || (n < 33 && buf[n])) return {};
        std::string s = " \"";
        s.append(buf, std::min<uint32_t>(n, 32));
        if (n > 32) s += "...";
        return s + "\"";
    }

    std::string indexNames(uint32_t pc) {
        std::string s;
        for (const uint32_t index : targets[pc]) {
            if (!s.empty()) s += "|";
            s += std::to_string(index);
        }
        return s;
    }

    void entry(uint32_t pc, const uint32_t* r) {
        std::fprintf(out, "%llu entry pc=%08x r0=%08x r1=%08x r2=%08x lr=%08x\n", (unsigned long long) ms(), pc, r[0], r[1], r[2], r[14]);
        std::fflush(out);
        if (r[1] < 0x1000) return;
        std::vector<uint32_t> table(slots);
        if (!read(r[1], table.data(), slots * 4)) {
            std::fprintf(out, "%llu table-unreadable %08x\n", (unsigned long long) ms(), r[1]);
            return;
        }
        tableBase = r[1];
        lastEntry = pc;
        targets.clear();
        lowest = ~0u;
        highest = 0;
        for (uint32_t i = 0; i < slots; ++i) add(table[i] & ~1u, i);
        if (r[2] >= 0x1000) add(r[2] & ~1u, call68kIndex);
        std::fprintf(out, "%llu table %08x slots=%u distinct=%zu call68k=%08x\n", (unsigned long long) ms(), tableBase, slots,
                     targets.size(), r[2]);
        std::fflush(out);
        ++entries;
    }

    void secondEntry(const uint32_t* r) {
        std::fprintf(out, "%llu entry2 r0=%08x r1=%08x r2=%08x lr=%08x\n", (unsigned long long) ms(), r[0], r[1], r[2], r[14]);
        if (r[2] >= 0x1000 && !targets.count(r[2] & ~1u)) add(r[2] & ~1u, call68kIndex);
        std::fflush(out);
    }

    void add(uint32_t target, uint32_t index) {
        if (target < 0x1000) return;
        targets[target].push_back(index);
        if (target < lowest) lowest = target;
        if (target > highest) highest = target;
    }

    void args(const uint32_t* r) {
        uint32_t s[4] = {};
        read(r[13], s, sizeof s);
        std::fprintf(out, " r0=%08x r1=%08x r2=%08x r3=%08x s0=%08x s1=%08x s2=%08x s3=%08x lr=%08x", r[0], r[1], r[2], r[3], s[0],
                     s[1], s[2], s[3], r[14]);
        const uint32_t v[8] = {r[0], r[1], r[2], r[3], s[0], s[1], s[2], s[3]};
        static const char* const label[8] = {"r0", "r1", "r2", "r3", "s0", "s1", "s2", "s3"};
        for (int i = 0; i < 8; ++i) {
            const std::string str = text(v[i]);
            if (!str.empty()) std::fprintf(out, " %s=%s", label[i], str.c_str() + 1);
        }
    }

    void call(const std::vector<uint32_t>& indexes, uint32_t pc, const uint32_t* r) {
        const std::string list = indexNames(pc);
        std::string names;
        for (const uint32_t index : indexes) names += (names.empty() ? "" : "|") + std::string(name(index));
        std::fprintf(out, "%llu call %s %s", (unsigned long long) ms(), list.c_str(), names.c_str());
        args(r);
        std::fputc('\n', out);
        push(r, indexes[0]);
        flushSometimes();
    }

    void push(const uint32_t* r, uint32_t index) {
        if (frames.size() >= 256) frames.erase(frames.begin());
        frames.push_back({r[14] & ~1u, r[13], index});
    }

    void moduleCall(uint32_t instruction, const uint32_t* r) {
        const uint32_t base = r[(instruction >> 16) & 15];
        if (base < 0x1000) return;
        static const char* const modules[3] = {"DAL", "Boot", "UI"};
        for (uint32_t m = 0; m < 3; ++m) {
            uint32_t table;
            if (!read(r[9] - 4 * (m + 1), &table, 4) || table != base) continue;
            const uint32_t index = (instruction & 0xfff) / 4;
            std::fprintf(out, "%llu call %s %u", (unsigned long long) ms(), modules[m], index);
            args(r);
            std::fputc('\n', out);
            push(r, 2000 + m * 1000 + index);
            flushSometimes();
            return;
        }
    }

    void returns(uint32_t pc, const uint32_t* r) {
        while (!frames.empty() && frames.back().lr == pc && frames.back().sp == r[13]) {
            const Frame f = frames.back();
            frames.pop_back();
            std::string what;
            if (f.index >= 2000) what = std::string(f.index < 3000 ? "DAL" : f.index < 4000 ? "Boot" : "UI") + " " + std::to_string(f.index % 1000);
            else what = std::to_string(f.index) + " " + name(f.index);
            std::fprintf(out, "%llu ret %s r0=%08x%s\n", (unsigned long long) ms(), what.c_str(), r[0], text(r[0]).c_str());
            flushSometimes();
        }
    }
};

// Set when FROND_TWGLUE_LOG names a file; the machine keeps it until the process ends.
inline std::unique_ptr<TwGlueLog>& twGlueLogInstance() {
    static std::unique_ptr<TwGlueLog> log;
    return log;
}

inline void twGlueLogObserve(void* context, const Imx1ProbeTrace& t) { static_cast<TwGlueLog*>(context)->step(t); }

// Returns the log for env FROND_TWGLUE_LOG, or null when it is unset or can't be opened.
inline TwGlueLog* twGlueLogCreate(ArmCpu* cpu, const Imx1ReferenceTime* time, TwGlueLog::PhysicalRead physical, void* context) {
    const char* path = std::getenv("FROND_TWGLUE_LOG");
    if (!path || !*path) return nullptr;
    FILE* out = std::fopen(path, "w");
    if (!out) {
        std::fprintf(stderr, "FROND_TWGLUE_LOG: can't open %s\n", path);
        return nullptr;
    }
    std::setvbuf(out, nullptr, _IOFBF, 1 << 16);
    twGlueLogInstance() = std::make_unique<TwGlueLog>(cpu, time, out, physical, context);
    return twGlueLogInstance().get();
}

#endif
