// dynarmic JIT for ordinary guest code (trial). Built as C++20 with FROND_DYNARMIC.
// MRS and MSR run in generated code through the fork's psr_coprocessor, and iWMMXt data
// processing and register transfers through the cp0 and cp1 slots, some p15 accesses
// through the p15 slot, and SWI through CallSVC. Each CPU has one JIT per privilege level
// (user, privileged), each with its own code cache and page table, so switching between
// them keeps both. Other privileged, coprocessor and Frond's special instructions end the
// block and go back to the interpreter, which also takes every other exception and IRQ
// between runs.
#include "jit_dynarmic.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "pxa270_WMMX.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/ir/terminal.h"

namespace {

using namespace Dynarmic;
using PageTable = std::array<std::uint8_t *, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

// MRC and MCR p15 c3 run in privileged mode from generated code, for jitDomainCalls.
std::atomic<uint64_t> domainCalls{0};

// Counters for FROND_JIT_STATS=1.
struct Stats {
    uint64_t runs = 0, ran = 0, interpret = 0, halt = 0, fault = 0, aborts = 0, invalAll = 0, invalRange = 0, clears = 0;
    uint64_t cold = 0;  // runs ended at code not compiled yet
    uint64_t queued = 0;  // of those, at code waiting for the compile thread
    uint64_t psr = 0;  // MSR CPSR and MOVS PC calls from generated code
    uint64_t cp15 = 0, cp15User = 0;  // MRC and MCR p15 c3 and c7 c5 1 calls, and those in user mode
    uint64_t icacheLine = 0;  // of those, MCR p15 c7 c5 1 (I-cache line invalidate)
    uint64_t privSwitches = 0;  // runs ended by a privilege change in generated code
    uint64_t swi = 0;  // SWIs taken from generated code
    uint64_t wmmx = 0;  // iWMMXt calls from generated code
    uint64_t wmmxXfer = 0, wmmxFaults = 0;  // of those, WLDR and WSTR, and those that failed
    uint64_t cparWrites = 0, cparChanges = 0;  // CPAR writes, and those that dropped the code
    // Interpreter exits at cp0 and cp1 words (not SWI) by class: CDP, MCR/MRC, MCRR/MRRC,
    // LDC/STC, and cond NV forms of any of them. Counted with FROND_JIT_STATS only.
    uint64_t interpWmmx[5] = {};
    // Code pages noted, dropped for a write, a changed mapping, a guest invalidation
    // (unwatched pages only) or lost fetch permission, and how often the mappings were checked.
    uint64_t codePages = 0, unwatched = 0, pagesWritten = 0, mapChecks = 0, mapChanged = 0, unwatchedDropped = 0;
    uint64_t fetchDropped = 0;
    std::unordered_map<uint32_t, uint64_t> interpPc;
    // Instruction at each interpreted PC (Thumb: the halfword), and the guest PC current
    // at each whole-cache invalidation with the word there.
    std::unordered_map<uint32_t, uint32_t> interpWord;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> invalAllPc;
    std::unordered_map<uint32_t, uint64_t> writtenPage;  // virtual page dropped for a write
    const bool on = [] {
        const char *v = getenv("FROND_JIT_STATS");
        return v && *v && strcmp(v, "0");
    }();
    uint64_t pageClears = 0, pageFills = 0, reads = 0, writes = 0, domainChecks = 0, pageDrops = 0, pageReadOnly = 0;
    // Pages mapped for direct stores, and pages sent back to the callbacks by
    // jitWriteProtect or a code flag.
    uint64_t writeMaps = 0, writeProtects = 0;
    // Compile thread counts: JITs still alive at exit, and those of JITs gone before.
    std::vector<const A32::Jit *> jits;  // under jitsMutex: user level JITs are made on emulation threads
    std::mutex jitsMutex;
    A32::Jit::CompileStats compiled;
    void AddCompiled(const A32::Jit &jit) {
        const A32::Jit::CompileStats c = jit.GetCompileStats();
        compiled.queued += c.queued;
        compiled.installed += c.installed;
        compiled.stale += c.stale;
        compiled.cancelled += c.cancelled;
        compiled.overflow += c.overflow;
        compiled.no_space += c.no_space;
        compiled.ahead_queued += c.ahead_queued;
        compiled.ahead_compiled += c.ahead_compiled;
        compiled.ahead_used += c.ahead_used;
        compiled.ahead_skipped += c.ahead_skipped;
    }
    ~Stats() {
        if (!on) return;
        for (const A32::Jit *j : jits) AddCompiled(*j);
        fprintf(stderr,
                "jit: compile thread queued %llu installed %llu stale %llu cancelled %llu overflow %llu "
                "noSpace %llu\n",
                (unsigned long long)compiled.queued, (unsigned long long)compiled.installed,
                (unsigned long long)compiled.stale, (unsigned long long)compiled.cancelled,
                (unsigned long long)compiled.overflow, (unsigned long long)compiled.no_space);
        fprintf(stderr, "jit: compile ahead queued %llu compiled %llu used %llu unused %llu skipped %llu\n",
                (unsigned long long)compiled.ahead_queued, (unsigned long long)compiled.ahead_compiled,
                (unsigned long long)compiled.ahead_used,
                (unsigned long long)(compiled.ahead_compiled - compiled.ahead_used),
                (unsigned long long)compiled.ahead_skipped);
        fprintf(stderr,
                "jit: runs %llu ran %llu interpret %llu cold %llu queued %llu halt %llu fault %llu aborts %llu invalAll %llu "
                "invalRange %llu clears %llu pageClears %llu domainChecks %llu pageDrops %llu pageReadOnly %llu pageFills %llu reads %llu writes %llu\n",
                (unsigned long long)runs, (unsigned long long)ran, (unsigned long long)interpret, (unsigned long long)cold,
                (unsigned long long)queued,
                (unsigned long long)halt, (unsigned long long)fault, (unsigned long long)aborts, (unsigned long long)invalAll,
                (unsigned long long)invalRange, (unsigned long long)clears, (unsigned long long)pageClears,
                (unsigned long long)domainChecks, (unsigned long long)pageDrops, (unsigned long long)pageReadOnly,
                (unsigned long long)pageFills, (unsigned long long)reads, (unsigned long long)writes);
        fprintf(stderr,
                "jit: codePages %llu unwatched %llu pagesWritten %llu mapChecks %llu mapChanged %llu "
                "unwatchedDropped %llu fetchDropped %llu msrCpsr %llu writeMaps %llu writeProtects %llu cp15 %llu "
                "cp15User %llu icacheLine %llu privSwitches %llu swi %llu\n",
                (unsigned long long)codePages, (unsigned long long)unwatched, (unsigned long long)pagesWritten,
                (unsigned long long)mapChecks, (unsigned long long)mapChanged, (unsigned long long)unwatchedDropped,
                (unsigned long long)fetchDropped,
                (unsigned long long)psr, (unsigned long long)writeMaps, (unsigned long long)writeProtects,
                (unsigned long long)cp15, (unsigned long long)cp15User, (unsigned long long)icacheLine,
                (unsigned long long)privSwitches, (unsigned long long)swi);
        fprintf(stderr,
                "jit: wmmx %llu ldc/stc %llu failed %llu cparWrites %llu cparChanges %llu interpret wmmx "
                "cdp %llu mcr/mrc %llu mcrr/mrrc %llu ldc/stc %llu nv %llu\n",
                (unsigned long long)wmmx, (unsigned long long)wmmxXfer, (unsigned long long)wmmxFaults,
                (unsigned long long)cparWrites, (unsigned long long)cparChanges,
                (unsigned long long)interpWmmx[0], (unsigned long long)interpWmmx[1],
                (unsigned long long)interpWmmx[2], (unsigned long long)interpWmmx[3],
                (unsigned long long)interpWmmx[4]);
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto &[pc, n] : interpPc) top.push_back({n, pc});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: interpret %08x %llu word %08x\n", top[i].second,
                    (unsigned long long)top[i].first, interpWord[top[i].second]);
        top.clear();
        for (auto &[pc, e] : invalAllPc) top.push_back({e.first, pc});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12; i++)
            fprintf(stderr, "jit: invalAll at %08x %llu word %08x\n", top[i].second,
                    (unsigned long long)top[i].first, invalAllPc[top[i].second].second);
        top.clear();
        for (auto &[page, n] : writtenPage) top.push_back({n, page});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; i++)
            fprintf(stderr, "jit: written page %08x %llu\n", top[i].second << 12, (unsigned long long)top[i].first);
    }
} stats;

constexpr HaltReason kInterpret = HaltReason::UserDefined2;
constexpr HaltReason kSlowPath = HaltReason::UserDefined3;
// A data access that faults ends the run on that instruction (check_halt_on_memory_access)
// with the fault in Env::dataAbort, and the CPU takes the abort there without running the
// instruction again (FROND_JIT_ABORT): the accesses before it in the same instruction (an
// LDM or STM's first words) are done once, as in Exact. An unaligned access is refused
// before it's tried, and the interpreter runs the instruction instead.
constexpr HaltReason kFault = HaltReason::MemoryAbort;
// The block at PC isn't compiled yet (ShouldCompile), for the interpreter.
constexpr HaltReason kCold = HaltReason::UserDefined4;
// MSR CPSR or an exception return changed the privilege: the run ends there and the next
// one goes to the other privilege level's JIT.
constexpr HaltReason kPrivilege = HaltReason::UserDefined5;
// A SWI from generated code: the run ends at the vector.
constexpr HaltReason kSwi = HaltReason::UserDefined6;

// The instructions the p15 slot runs (jitCp15SlotWord). Each ends its block, so a user
// mode one can be undone and handed to the interpreter, which raises the undefined
// instruction, and the budget is checked right after it, as after an interpreted one.
inline bool Cp15Slot(uint32_t w, bool arm920) { return arm920 ? jitCp15SlotWord920(w) : jitCp15SlotWord(w); }

// The p15 slot's instructions end their block (Cp15Slot).
// Translation and emission of a block run back to back on one thread (the guest thread or
// the compile thread), so the block's one p15 instruction is still current there when the
// coprocessor compiles it.
// Slot for the fork's psr_coprocessor, one XScale doesn't have. Real coprocessor
// instructions never reach dynarmic (cpuJitPlain), so the slot is only used for MRS and MSR (and the
// ARM920T's access pcs).
constexpr std::size_t kPsrCoprocessor = 8;

thread_local uint32_t cp15Pc = ~0u, cp15Rd = 0, cp15Word = 0;

// PreCodeReadHook's work: the instruction at pc goes to the JIT if plain(pc, word) says
// so (word gets the aligned word holding it); a p15 c3 access ends its block after it.
template <typename Plain>
bool CodeReadHook(bool thumb, bool arm920, A32::VAddr pc, A32::IREmitter &ir, Plain plain) {
    if (pc == A32::LocationDescriptor{ir.block.Location()}.PC()) {
        cp15Pc = ~0u;
    } else if (cp15Pc != ~0u && pc == cp15Pc + 4) {
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
        return false;
    }
    uint32_t w;
    if (plain(pc, w)) {
        if (!thumb && Cp15Slot(w, arm920)) {
            cp15Pc = pc;
            cp15Rd = (w >> 12) & 15;
            cp15Word = w;
        }
        return true;
    }
    ir.SetTerm(IR::Term::Interpret(ir.current_location));
    return false;
}

// Instructions after which dynarmic surely ends the block: branches and writes to PC. The
// snapshot stops there; if translation goes on anyway, it overflows and a whole one is taken.
bool EndsBlock(uint32_t w, uint32_t pc, bool thumb) {
    if (thumb) {
        const uint32_t h = (pc & 2) ? w >> 16 : w & 0xffff;
        if (h >= 0xe000 && h < 0xe800) return true;                        // B
        if ((h & 0xf000) == 0xd000 && (h & 0x0f00) < 0x0e00) return true;  // B<cond>
        if (h >= 0xf000) return true;                                      // BL, BLX pair
        if ((h & 0xff00) == 0xdf00) return true;                           // SWI
        if ((h & 0xff00) == 0x4700 || (h & 0xff00) == 0xbd00) return true;  // BX, BLX, POP {pc}
        return (h & 0xfd87) == 0x4487;                                     // ADD, MOV pc
    }
    if ((w & 0x0e000000) == 0x0a000000) return true;  // B, BL, BLX imm
    if ((w >> 28) == 15) return false;
    if ((w & 0x0f000000) == 0x0f000000) return true;  // SWI
    if ((w & 0x0ffffff0) == 0x012fff10 || (w & 0x0ffffff0) == 0x012fff30) return true;  // BX, BLX
    if ((w & 0x0e108000) == 0x08108000) return true;  // LDM with PC
    if ((w & 0x0c10f000) == 0x0410f000) return true;  // LDR PC
    return (w & 0x0c00f000) == 0x0000f000;            // data processing to PC
}

// The code one block may read, for the compile thread: the words from its start and
// whether each instruction may go to the JIT (cpuJitPlain), taken on the guest thread at
// the request, so translation there gives what it would have given in place then.
// Translation marks the words it reads; before the block is installed they are fetched
// again and compared (Env::CodeUnchanged).
struct Snapshot final : public A32::CodeSnapshot {
    uint32_t base = 0;  // the word holding the first instruction
    std::vector<uint32_t> words;
    std::vector<uint8_t> fetched, read;  // per word
    std::vector<std::pair<uint32_t, bool>> plain;  // each instruction in order, and its check
    std::vector<uint32_t> marked;  // access pcs among them
    size_t next = 0;
    bool arm920 = false;

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr va) override {
        const uint32_t i = (va - base) >> 2;
        if (va < base || i >= words.size()) {
            overflow = true;
            return std::nullopt;
        }
        read[i] = 1;
        if (!fetched[i]) return std::nullopt;
        return words[i];
    }

    bool PreCodeReadHook(bool thumb, A32::VAddr pc, A32::IREmitter &ir) override {
        const bool ok = CodeReadHook(thumb, arm920, pc, ir, [&](uint32_t at, uint32_t &w) {
            while (next < plain.size() && plain[next].first != at) next++;
            if (next == plain.size()) {
                overflow = true;
                return false;
            }
            const uint32_t i = (at - base) >> 2;
            w = words[i];
            return plain[next].second;
        });
        if (ok && std::find(marked.begin(), marked.end(), pc) != marked.end())
            ir.CoprocSendOneWord(kPsrCoprocessor, false, 3, A32::CoprocReg::C0, A32::CoprocReg::C0, 0, ir.Imm32(pc));
        return ok;
    }
};

struct Env final : public A32::UserCallbacks {
    ArmCpu *cpu = nullptr;
    bool arm920 = false;
    // ARM920T: lineBound for the probe's single-step diagnostic mode, blocks ended at each 32-byte line.
    // Otherwise (Fast blocks) code pages are watched by physical page through the lean path's stores
    // (jitCodeWrittenPa), so the guest's I-cache invalidations change nothing.
    bool lineBound = false;
    std::unordered_map<uint32_t, std::vector<uint32_t>> byPa;  // Fast blocks: virtual code pages by physical page
    uint64_t blocks = 0, instructions = 0;
    A32::Jit *jit = nullptr;
    PageTable *pages = nullptr;
    // Filled page table entries by their page's domain (MMU_DOMAIN_NONE with the MMU off,
    // MMU_DOMAIN_UNKNOWN when the TLB didn't hold the page), each with what its TLB entry
    // allows while the domain is a client (cpuJitPageClientAccess).
    struct Filled {
        uint32_t page;
        uint8_t client;
    };
    std::array<std::vector<Filled>, MMU_DOMAIN_UNKNOWN + 1> filled;
    // The privilege level this JIT runs at, and the other level's Env (null until that one
    // first runs). Everything here is only used while the CPU is at this level, apart from
    // what both get: invalidations, clears, write protection and code writes.
    bool privileged = true;
    Env *other = nullptr;

    // Page table entries with bit 0 set are read-only (the fork's page_table_write_protect_bit):
    // stores to them come to the callbacks. A RAM page is filled read-only and made writable
    // by its first store through Write, which marks it dirty (and the frame buffer, where it
    // overlaps) as if all of it was written. Whoever clears that dirty state write-protects the
    // page again (jitWriteProtect), and so does a code flag on any of its lines, so the
    // flag's check runs on stores. Writable entries by host page:
    std::unordered_map<uint8_t *, std::vector<uint32_t>> writable;

    static uint8_t *ReadOnly(uint8_t *host) { return (uint8_t *)(uintptr_t(host) | 1); }

    void ProtectHost(uint8_t *host) {
        const auto it = writable.find(host);
        if (it == writable.end()) return;
        for (uint32_t page : it->second)
            if ((*pages)[page] == host) (*pages)[page] = ReadOnly(host);
        stats.writeProtects++;
        writable.erase(it);
    }

    void Protect(const uint8_t *host, uint32_t size) {
        for (auto it = writable.begin(); it != writable.end();) {
            if (it->first + 0x1000 > host && it->first < host + size) {
                for (uint32_t page : it->second)
                    if ((*pages)[page] == it->first) (*pages)[page] = ReadOnly(it->first);
                stats.writeProtects++;
                it = writable.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Each virtual page dynarmic has read code from, with the physical page it came from
    // and its host page. Each 64-byte RAM line code is read from is flagged, and the first
    // write to one comes back through CodeWritten and drops the code of every virtual page
    // on that host page. Lines keep code and data apart on pages that hold both, like the
    // vector page. ROM only changes from the host side, which
    // clears everything. Pages that are neither (host null) can't be watched, so the
    // guest's I-cache invalidations drop them instead.
    // Dynarmic keeps blocks by PC and CPSR state, not permissions, so each code page also
    // has its domain and AP (access). When the domains, S or R change, the code of every
    // page that can't be fetched now is dropped, and fetching it again takes the prefetch
    // abort. Each Env only runs at its own privilege, so that isn't part of the context.
    struct CodePage {
        uint32_t pa;  // physical page, or ~0u when the page tables had no mapping
        uint8_t *host;
        uint8_t *lines;  // RAM code flags for the page, null for ROM and unwatched pages
        uint32_t access;
    };
    std::unordered_map<uint32_t, uint32_t> accessPages;  // code pages by access
    uint64_t permContext = ~0ull;
    std::unordered_map<uint32_t, CodePage> code;
    std::unordered_map<uint8_t *, std::vector<uint32_t>> byHost;
    std::vector<uint32_t> unwatched;
    uint32_t mapGen = 0;

    uint32_t CodePa(uint32_t page, uint32_t &access) {
        uint32_t pa;
        return cpuJitCodePa(cpu, page << 12, &pa, &access) ? pa >> 12 : ~0u;
    }

    void NoteCode(uint32_t va) {
        const uint32_t page = va >> 12;
        auto it = code.find(page);
        if (it == code.end()) it = NotePage(page);
        uint8_t *lines = it->second.lines;
        if (lines && !lines[(va & 0xfffu) >> 6]) {
            // The flags are the CPU's, so both privilege levels' direct stores to the page
            // have to come back through the callbacks.
            lines[(va & 0xfffu) >> 6] = 1;
            ProtectHost(it->second.host);
            if (other) other->ProtectHost(it->second.host);
        }
    }

    std::unordered_map<uint32_t, CodePage>::iterator NotePage(uint32_t page) {
        uint32_t access;
        const uint32_t pa = CodePa(page, access);
        uint8_t *lines = nullptr;
        uint8_t *host = pa == ~0u ? nullptr : cpuJitWatchCode(cpu, pa << 12, &lines);
        const auto it = code.emplace(page, CodePage{pa, host, lines, access}).first;
        accessPages[access]++;
        stats.codePages++;
        if (arm920 && !lineBound && host) {
            // A newly watched RAM page may have dropped the lean TLB; empty the page table now, as code
            // can run straight on after this compile. The probe's single-step mode (lineBound) has no lean
            // path, so its pages are watched by host page like the other cores'.
            byPa[pa].push_back(page);
            CheckGen();
        } else if (host) {
            byHost[host].push_back(page);
        } else {
            stats.unwatched++;
            unwatched.push_back(page);
        }
        return it;
    }

    void Drop(uint32_t page) {
        jit->InvalidateCacheRange(page << 12, 4096);
        const auto it = code.find(page);
        if (it == code.end()) return;
        const auto n = accessPages.find(it->second.access);
        if (!--n->second) accessPages.erase(n);
        code.erase(it);
    }

    // Drops the code of pages that can't be fetched with the current domains, S and R. Only a
    // change of those can make a page unfetchable without a TLB flush.
    void CheckFetch() {
        const uint64_t context = cpuJitPermContext(cpu);
        if (context == permContext) return;
        permContext = context;
        std::vector<uint32_t> lost;
        for (const auto &[access, n] : accessPages)
            if (!cpuJitAccessReadable(cpu, access)) lost.push_back(access);
        if (lost.empty()) return;
        std::vector<uint32_t> drop;
        for (const auto &[page, c] : code)
            if (std::find(lost.begin(), lost.end(), c.access) != lost.end()) drop.push_back(page);
        for (uint32_t page : drop) {
            stats.fetchDropped++;
            Drop(page);
        }
    }

    void CodeWrittenPa(uint32_t pa) {
        const auto it = byPa.find(pa);
        if (it == byPa.end()) return;
        for (uint32_t page : it->second) {
            const auto c = code.find(page);
            if (c == code.end() || c->second.pa != pa) continue;
            stats.pagesWritten++;
            if (stats.on) stats.writtenPage[page]++;
            Drop(page);
        }
        byPa.erase(it);
    }

    void CodeWritten(uint8_t *host) {
        const auto it = byHost.find(host);
        if (it == byHost.end()) return;
        for (uint32_t page : it->second) {
            const auto c = code.find(page);
            if (c == code.end() || c->second.host != host) continue;
            stats.pagesWritten++;
            if (stats.on) stats.writtenPage[page]++;
            Drop(page);
        }
        byHost.erase(it);
    }

    // After a TLB flush, drop the code of every page whose mapping moved.
    void CheckMappings() {
        stats.mapChecks++;
        std::vector<uint32_t> moved;
        for (const auto &[page, c] : code) {
            uint32_t access;
            if (CodePa(page, access) != c.pa || access != c.access) moved.push_back(page);
        }
        for (uint32_t page : moved) {
            stats.mapChanged++;
            Drop(page);
        }
    }

    void DropUnwatched(uint32_t first, uint32_t last) {
        std::erase_if(unwatched, [&](uint32_t page) {
            if (page < first || page > last) return false;
            const auto c = code.find(page);
            if (c != code.end() && !c->second.host) {
                stats.unwatchedDropped++;
                Drop(page);
            }
            return true;
        });
    }

    // Host side clears (reset, session load, patching) also reset dynarmic's own state at
    // the next run (return stack buffer, halt flags, exclusive monitor, FP control), so a
    // load starts from the same state as a fresh process.
    void Clear() {
        stats.clears++;
        ClearCode();
        resetState = true;
    }

    // Drops all translated code but keeps dynarmic's state, for a CPAR change made by the
    // guest between runs.
    void ClearCode() {
        ClearPages();
        jit->ClearCache();
        std::fill(heat.begin(), heat.end(), Heat{});
        code.clear();
        accessPages.clear();
        permContext = cpuJitPermContext(cpu);
        byHost.clear();
        byPa.clear();
        unwatched.clear();
        cpuJitForgetCode(cpu);
        gen = ~0ull;
        mapGen = cpuJitMapGen(cpu);
    }

    bool resetState = false;

    uint64_t gen = ~0ull;
    uint64_t budget = 0, ran = 0;

    uint32_t domains = 0;  // DACR at the last CheckGen

    void ClearPages() {
        stats.pageClears++;
        for (auto &list : filled) {
            for (const Filled &f : list) (*pages)[f.page] = nullptr;
            list.clear();
        }
        writable.clear();
    }

    void NoteFilled(uint32_t page) {
        const int access = cpuJitPageClientAccess(cpu, page << 12, privileged);
        if (access < 0)
            filled[MMU_DOMAIN_UNKNOWN].push_back({page, 0});
        else
            filled[access >> 2].push_back({page, uint8_t(access & 3)});
    }

    // Only the DACR changed since the pages were filled, so each page has the mapping and
    // TLB entry it was filled with. Pages in domains that lost rights (and pages whose
    // domain wasn't known) are checked again: a writable entry the MMU now allows reads
    // only on goes read-only, and an entry it allows no reads on is dropped. The rest grant
    // no more than the MMU does now, so the work goes with the pages in those domains.
    void CheckDomains(uint32_t from, uint32_t to) {
        stats.domainChecks++;
        for (unsigned d = 0; d < 16; d++) {
            const uint32_t before = (from >> (2 * d)) & 3, now = (to >> (2 * d)) & 3;
            if (mmuDomainRank(now) < mmuDomainRank(before)) CheckDomain(filled[d], now);
        }
        CheckDomain(filled[MMU_DOMAIN_UNKNOWN], 0);
    }

    // The pages of one domain whose two DACR bits are now bits.
    void CheckDomain(std::vector<Filled> &list, uint32_t bits) {
        size_t kept = 0;
        for (const Filled f : list) {
            const unsigned access = bits == 3 ? 3 : bits == 1 ? f.client : 0;
            uint8_t *const entry = (*pages)[f.page];
            const bool writableEntry = entry && !(uintptr_t(entry) & 1);
            if (!(access & 1) || (writableEntry && !(access & 2))) {
                if (writableEntry) {
                    const auto it = writable.find(entry);
                    if (it != writable.end()) {
                        std::erase(it->second, f.page);
                        if (it->second.empty()) writable.erase(it);
                    }
                }
                if (access & 1) {
                    (*pages)[f.page] = ReadOnly(entry);
                    stats.pageReadOnly++;
                } else {
                    (*pages)[f.page] = nullptr;
                    stats.pageDrops++;
                    continue;
                }
            }
            list[kept++] = f;
        }
        list.resize(kept);
    }

    void CheckGen() {
        const uint64_t g = cpuJitPageGen(cpu);
        if (g != gen) GenChanged(g);
    }

    [[gnu::noinline]] void GenChanged(uint64_t g) {
        const uint32_t d = cpuJitGetDomain(cpu);
        if (gen == ~0ull || (g >> 32) != (gen >> 32))
            ClearPages();
        else if (d != domains)
            CheckDomains(domains, d);
        gen = g;
        domains = d;
        CheckFetch();
    }

    void Fault() { jit->HaltExecution(kFault); }

    // A failed access: a guest fault (fsr set) for the data abort, or a refused one for
    // the interpreter. The run leaves at the check right after the access, so it's the
    // instruction's last; the first fault is kept all the same.
    [[gnu::cold, gnu::noinline]] void AccessFailed(uint32_t va, unsigned size, bool write, uint_fast8_t fsr) {
        if (fsr && !dataAbort.set) {
            stats.aborts++;
            dataAbort = {true, {va, uint8_t(fsr), uint8_t(size), write}};
        }
        Fault();
    }

    // Interpret first: a block is compiled once its start has been reached `hot` times.
    // Before that the run ends there and the interpreter runs the code up to its next taken
    // branch, so code that runs once (an application starting) is never compiled. Counts are
    // kept by PC and Thumb state in a small direct-mapped table; another PC landing on the
    // same entry starts it again. Emptied with the cache.
    struct Heat {
        uint32_t key = 0, count = 0;
    };
    static constexpr unsigned kHeatBits = 14;
    std::vector<Heat> heat = std::vector<Heat>(size_t(1) << kHeatBits);
    uint32_t hot = 1;

    bool ShouldCompile(A32::VAddr pc, bool thumb) override {
        if (hot <= 1) return true;
        const uint32_t key = pc | uint32_t(thumb);  // PC is even
        Heat &h = heat[(key * 0x9e3779b1u) >> (32 - kHeatBits)];
        if (h.key != key) h = Heat{key, 0};
        if (++h.count >= hot) return true;
        stats.cold++;
        jit->HaltExecution(kCold);
        return false;
    }

    void AfterIo() {
        if (cpuJitSlowPath(cpu)) jit->HaltExecution(kSlowPath);
    }

    template <typename T>
    T Read(A32::VAddr va) {
        T v{};
        stats.reads++;
        uint_fast8_t fsr;
        if (!cpuJitRead(cpu, va, sizeof(T), &v, &fsr)) {
            AccessFailed(va, sizeof(T), false, fsr);
            return 0;
        }
        // Only aligned reads land here for pages the table could hold; fill it.
        CheckGen();
        const uint32_t page = va >> 12;
        if (!(*pages)[page]) {
            uint8_t *host = cpuJitReadPage(cpu, va);
            if (host && !(uintptr_t(host) & 1)) {
                stats.pageFills++;
                (*pages)[page] = ReadOnly(host);
                NoteFilled(page);
            } else {
                AfterIo();
            }
        }
        return v;
    }

    template <typename T>
    void Write(A32::VAddr va, T v) {
        stats.writes++;
        uint_fast8_t fsr;
        if (!cpuJitWrite(cpu, va, sizeof(T), &v, &fsr)) {
            AccessFailed(va, sizeof(T), true, fsr);
            return;
        }
        // A RAM page the guest may store to in this mode: map it writable. Unaligned stores
        // also land here for pages that already are.
        CheckGen();
        const uint32_t page = va >> 12;
        uint8_t *const entry = (*pages)[page];
        if (!entry || (uintptr_t(entry) & 1)) {
            uint8_t *host = cpuJitWritePage(cpu, va);
            if (host && !(uintptr_t(host) & 1)) {
                stats.writeMaps++;
                if (!entry) NoteFilled(page);
                (*pages)[page] = host;
                writable[host].push_back(page);
            }
        }
        AfterIo();
    }

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr va) override {
        NoteCode(va);
        uint32_t w;
        if (!cpuJitFetch(cpu, va, &w)) return std::nullopt;
        return w;
    }

    bool PreCodeReadHook(bool thumb, A32::VAddr pc, A32::IREmitter &ir) override {
        if (lineBound && (pc & ~31u) != (A32::LocationDescriptor{ir.block.Location()}.PC() & ~31u)) {
            ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        const bool ok = CodeReadHook(thumb, arm920, pc, ir, [&](uint32_t at, uint32_t &w) {
            const bool plain = cpuJitFetch(cpu, at & ~3u, &w) && cpuJitPlain(cpu, at, w, thumb);
            if (plain && pc == A32::LocationDescriptor{ir.block.Location()}.PC()) blocks++;
            return plain;
        });
        // An access pc: its value goes to cpuJitAccessPcSlot before the instruction (psr slot, opc1 3).
        if (ok && arm920 && cpuJitAccessPcMarked(cpu, pc))
            ir.CoprocSendOneWord(kPsrCoprocessor, false, 3, A32::CoprocReg::C0, A32::CoprocReg::C0, 0, ir.Imm32(pc));
        return ok;
    }

    // Compile thread. The snapshot runs from pc up to the first instruction that surely
    // ends the block (whole: up to the block length limit), stopping early at one the JIT
    // can't run, as the hook would.
    uint32_t maxInstructions = 0;

    std::unique_ptr<A32::CodeSnapshot> SnapshotCode(A32::VAddr pc, bool thumb, bool whole) override {
        auto s = std::make_unique<Snapshot>();
        s->arm920 = arm920;
        s->base = pc & ~3u;
        auto word = [&](uint32_t va) {
            const uint32_t i = (va - s->base) >> 2;
            while (s->words.size() <= i) {
                uint32_t w = 0;
                const bool ok = cpuJitFetch(cpu, s->base + uint32_t(s->words.size()) * 4, &w);
                s->words.push_back(w);
                s->fetched.push_back(ok);
                s->read.push_back(0);
            }
            return i;
        };
        const uint32_t limit = maxInstructions ? maxInstructions : 1024;
        uint32_t at = pc;
        for (uint32_t n = 0; n < limit; n++) {
            const uint32_t i = word(at);
            const uint32_t w = s->words[i];
            const bool plain = s->fetched[i] && cpuJitPlain(cpu, at, w, thumb);
            s->plain.push_back({at, plain});
            if (!plain) break;
            if (arm920 && cpuJitAccessPcMarked(cpu, at)) s->marked.push_back(at);
            uint32_t size = 4;
            if (thumb) {
                const uint32_t h = (at & 2) ? w >> 16 : w & 0xffff;
                size = h >= 0xe800 ? 4 : 2;
                if (size == 4) word(at + 2);
            }
            if (!whole && EndsBlock(w, at, thumb)) break;
            if (!thumb && Cp15Slot(w, arm920)) break;  // p15 slot instructions end their block
            at += size;
        }
        s->runnable = s->plain.front().second;
        return s;
    }

    // Ahead of need: as SnapshotCode, reading only RAM and ROM. Code that reaches anything
    // else isn't compiled ahead.
    std::unique_ptr<A32::CodeSnapshot> SnapshotCodeAhead(A32::VAddr pc, bool thumb, bool whole) override {
        cpuJitMemoryOnly(cpu, true);
        auto s = SnapshotCode(pc, thumb, whole);
        cpuJitMemoryOnly(cpu, false);
        if (cpuJitRefused(cpu)) return nullptr;
        return s;
    }

    // The words translation read are fetched again; if any changed (or no longer fetch),
    // the block is dropped. Otherwise they are noted as code, as MemoryReadCode does.
    bool CodeUnchanged(A32::CodeSnapshot &snapshot) override {
        auto &s = static_cast<Snapshot &>(snapshot);
        for (size_t i = 0; i < s.words.size(); i++) {
            if (!s.read[i]) continue;
            uint32_t w = 0;
            const bool ok = cpuJitFetch(cpu, s.base + uint32_t(i) * 4, &w);
            if (ok != bool(s.fetched[i]) || (ok && w != s.words[i])) return false;
        }
        for (size_t i = 0; i < s.words.size(); i++)
            if (s.read[i]) NoteCode(s.base + uint32_t(i) * 4);
        return true;
    }

    void CompileQueued(A32::VAddr, bool) override {
        stats.cold++;
        stats.queued++;
        jit->HaltExecution(kCold);
    }

    // A p15 access in user mode: leave the registers as they were and end the run with the
    // instruction next, for the interpreter.
    uint32_t rewindPc = ~0u;
    void Cp15User(uint32_t pc) {
        stats.cp15User++;
        rewindPc = pc;
        undoTick = true;
        jit->HaltExecution(kInterpret);
    }

    std::uint8_t MemoryRead8(A32::VAddr va) override { return Read<uint8_t>(va); }
    std::uint16_t MemoryRead16(A32::VAddr va) override { return Read<uint16_t>(va); }
    std::uint32_t MemoryRead32(A32::VAddr va) override { return Read<uint32_t>(va); }
    std::uint64_t MemoryRead64(A32::VAddr va) override { return Read<uint64_t>(va); }
    void MemoryWrite8(A32::VAddr va, std::uint8_t v) override { Write(va, v); }
    void MemoryWrite16(A32::VAddr va, std::uint16_t v) override { Write(va, v); }
    void MemoryWrite32(A32::VAddr va, std::uint32_t v) override { Write(va, v); }
    void MemoryWrite64(A32::VAddr va, std::uint64_t v) override { Write(va, v); }

    void InterpreterFallback(A32::VAddr pc, size_t) override {
        if (stats.on) {
            uint32_t w = 0;
            cpuJitFetch(cpu, pc & ~3u, &w);
            const bool thumb = jit->Cpsr() & 0x20;
            if (!stats.interpPc[pc]++) stats.interpWord[pc] = thumb ? ((pc & 2) ? w >> 16 : w & 0xffff) : w;
            if (!thumb && (w & 0x0c000000) == 0x0c000000 && (w & 0x0f000000) != 0x0f000000 && ((w >> 8) & 14) == 0) {
                if ((w >> 28) == 15) stats.interpWmmx[4]++;
                else if ((w & 0x0f000010) == 0x0e000000) stats.interpWmmx[0]++;
                else if ((w & 0x0f000000) == 0x0e000000) stats.interpWmmx[1]++;
                else if ((w & 0x0fe00000) == 0x0c400000) stats.interpWmmx[2]++;
                else stats.interpWmmx[3]++;
            }
        }
        jit->HaltExecution(kInterpret);
    }
    // SWI (not the semihosting ones, cpuJitPlain): PC is past it. The run ends at the
    // vector, so the next one starts there in the right privilege level's JIT with the
    // budget and slow path checked, as after an interpreted SWI. An ARM920T SWI from a
    // privileged mode with budget left goes on at the vector in this run: the block returns
    // to the dispatcher, which takes the new PC and ARM state, with what is left of the
    // budget (AddTicks has the cycles so far, GetTicksRemaining gives the rest).
    void CallSVC(std::uint32_t) override {
        stats.swi++;
        jit->SetCpsr(cpuJitSwi(cpu, jit->Regs().data(), jit->Cpsr()));
        if (cpuJitPrivileged(cpu) != privileged) stats.privSwitches++;
        else if (arm920 && ran < budget && (ClearSetChange(), !cpuJitSlowPath(cpu))) return;
        jit->HaltExecution(kSwi);
    }
    // The interpreter's ARM/Thumb switch flag: generated code follows the Thumb bit by
    // itself, so on the ARM920T a switch alone doesn't end the run.
    void ClearSetChange() {
        if (arm920) cpuClearSlowPath(cpu, SLOW_PATH_REASON_INSTRUCTION_SET_CHANGE);
    }

    // Undefined, unpredictable and no-execute: PC is past the instruction and its tick
    // is counted, so step back and let the interpreter run it.
    void ExceptionRaised(A32::VAddr pc, A32::Exception) override {
        jit->Regs()[15] = pc;
        undoTick = true;
        jit->HaltExecution(kInterpret);
    }

    // MSR CPSR with more than the flags. The fork ends the block after this call, so the
    // banked registers and CPSR can change under it. A set slow path (an unmasked pending
    // interrupt, a Thumb switch) ends the run there, and so does a privilege change, with
    // nothing run or checked at the new privilege in this JIT.
    static std::uint64_t SetCpsr(void *arg, std::uint32_t value, std::uint32_t mask) {
        Env &e = *static_cast<Env *>(arg);
        stats.psr++;
        const uint32_t cpsr = cpuJitSetCpsr(e.cpu, e.jit->Regs().data(), e.jit->Cpsr(), mask, value);
        e.jit->SetCpsr(cpsr);
        e.AfterPsr();
        return 0;
    }

    void AfterPsr() {
        if (cpuJitPrivileged(cpu) != privileged) {
            stats.privSwitches++;
            jit->HaltExecution(kPrivilege);
        } else {
            CheckGen();
        }
        ClearSetChange();
        AfterIo();
    }

    // MOVS PC or SUBS PC. The fork returns to the dispatcher after this call, which picks up
    // the new PC and Thumb state.
    static std::uint64_t ExceptionReturn(void *arg, std::uint32_t target, std::uint32_t) {
        Env &e = *static_cast<Env *>(arg);
        stats.psr++;
        const uint32_t cpsr = cpuJitExceptionReturn(e.cpu, e.jit->Regs().data(), e.jit->Cpsr(), target);
        e.jit->SetCpsr(cpsr);
        e.AfterPsr();
        return 0;
    }

    struct Cp15Op {
        Env *env;
        uint32_t pc, rd, word;
    };
    // By pc and word, kept for the process. Added to by whichever thread compiles; generated
    // code holds pointers to the entries, which don't move. An entry is written only when it is
    // added, since generated code on the guest thread may be reading it.
    std::unordered_map<uint64_t, Cp15Op> cp15Ops;
    std::mutex cp15Mutex;

    Cp15Op *Cp15Current() {
        std::lock_guard lock{cp15Mutex};
        return &cp15Ops.try_emplace((uint64_t(cp15Word) << 32) | cp15Pc, Cp15Op{this, cp15Pc, cp15Rd, cp15Word})
                    .first->second;
    }

    static std::uint64_t Cp15Read(void *arg, std::uint32_t, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu)) {
            e.Cp15User(op.pc);
            return e.jit->Regs()[op.rd];
        }
        domainCalls.fetch_add(1, std::memory_order_relaxed);
        return cpuJitGetDomain(e.cpu);
    }

    // A domain change flushes the host TLB; the page table drops the pages in changed domains.
    static std::uint64_t Cp15Write(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu)) {
            e.Cp15User(op.pc);
            return 0;
        }
        domainCalls.fetch_add(1, std::memory_order_relaxed);
        cpuJitSetDomain(e.cpu, value);
        e.CheckGen();
        e.AfterIo();
        return 0;
    }

    // The other p15 slot instructions, through the interpreter's coprocessor handler: the
    // result for an MRC, ignored for an MCR.
    static std::uint64_t Cp15Xfer(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        if (!cpuJitPrivileged(e.cpu) || !cpuJitCp15Xfer(e.cpu, op.word, &value)) {
            e.Cp15User(op.pc);
            return e.jit->Regs()[op.rd];
        }
        return value;
    }

    // MCR p15 c7 c5 1: the interpreter's I-cache line invalidate, which also drops the code
    // of unwatched pages in that line from both JITs (jitInvalidate). If that drops the
    // running block, dynarmic finishes it first; the block ends after this instruction.
    static std::uint64_t Cp15InvalLine(void *arg, std::uint32_t value, std::uint32_t) {
        const Cp15Op &op = *static_cast<Cp15Op *>(arg);
        Env &e = *op.env;
        stats.cp15++;
        // The ARM920T refuses an address with SBZ bits 4:0 set; the interpreter takes that one.
        if (!cpuJitPrivileged(e.cpu) || (e.arm920 && (value & 31))) {
            e.Cp15User(op.pc);
            return 0;
        }
        stats.icacheLine++;
        cpuJitIcacheInvalLine(e.cpu, value);
        return 0;
    }

    // iWMMXt instructions, by encoding without the condition and core registers: the
    // fields for pxa270_WMMX's cores. Kept for the process like cp15Ops.
    struct WmmxOp {
        Pxa270wmmx *w;
        bool cp1;
        uint8_t op1, a, b, c, op2;  // CDP: CRd, CRn, CRm; MCR/MRC: CRn, CRm; MCRR/MRRC: CRm
    };
    std::unordered_map<uint32_t, WmmxOp> wmmxOps;
    std::mutex wmmxMutex;

    WmmxOp *Wmmx(uint32_t key, WmmxOp op) {
        std::lock_guard lock{wmmxMutex};
        return &wmmxOps.try_emplace(key, op).first->second;
    }

    // None of these can fail: the plain check let only encodings in that
    // pxa270wmmxJitValid accepts, and a false return depends only on the encoding.
    static std::uint64_t WmmxCdp(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxCdp(op.w, op.cp1, op.op1, op.a, op.b, op.c, op.op2);
        return 0;
    }
    static std::uint64_t WmmxMcr(void *arg, std::uint32_t value, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxMcr(op.w, op.cp1, op.op1, op.a, op.b, op.op2, value);
        return 0;
    }
    static std::uint64_t WmmxMrc(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        uint32_t value = 0;
        pxa270wmmxMrc(op.w, op.cp1, op.op1, op.a, op.b, op.op2, &value);
        return value;
    }
    static std::uint64_t WmmxMcrr(void *arg, std::uint32_t lo, std::uint32_t hi) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        pxa270wmmxMcrr(op.w, op.cp1, op.op1, op.c, lo, hi);
        return 0;
    }
    static std::uint64_t WmmxMrrc(void *arg, std::uint32_t, std::uint32_t) {
        const WmmxOp &op = *static_cast<WmmxOp *>(arg);
        stats.wmmx++;
        uint64_t value = 0;
        pxa270wmmxMrrc(op.w, op.cp1, op.op1, op.c, &value);
        return value;
    }

    // WLDR and WSTR, by coprocessor, size (N), direction and wR; arg0 is the address
    // dynarmic worked out (cp0 offsets unscaled). The access is the interpreter's, so a
    // failed one ends the run on the instruction with the base not written back (the
    // dynarmic patch checks right after the call) and the fault kept in dataAbort: jitRun
    // reports FROND_JIT_ABORT and the CPU takes the data abort there as Exact does, without
    // running the instruction again, since a WLDRD or WSTRD whose second word failed has
    // already done the first word's access and a device would see it twice.
    struct Abort {
        bool set = false;
        FrondJitAbort fault{};
    } dataAbort;
    struct WmmxXfer {
        Env *env;
        bool cp1, N, store;
        uint8_t wR;
    };
    static std::uint64_t WmmxTransfer(void *arg, std::uint32_t ea, std::uint32_t) {
        const WmmxXfer &x = *static_cast<WmmxXfer *>(arg);
        Env &e = *x.env;
        stats.wmmx++;
        stats.wmmxXfer++;
        Pxa270wmmxFault f;
        if (!pxa270wmmxTransfer(cpuJitWmmx(e.cpu), e.cpu, x.cp1, x.N, x.store, x.wR, ea, &f)) {
            stats.wmmxFaults++;
            e.dataAbort = Abort{true, {f.addr, f.fsr, f.size, f.write}};
            e.Fault();
            return 0;
        }
        e.CheckGen();
        e.AfterIo();
        return 0;
    }

    void AddTicks(std::uint64_t ticks) override { ran += ticks; }
    // Called at the start of a run and again after a CallSVC that doesn't halt, once AddTicks has taken the cycles so
    // far: the rest of the run's budget, so a run that goes on past a SWI still ends on time.
    std::uint64_t GetTicksRemaining() override { return budget > ran ? budget - ran : 0; }

    bool undoTick = false;
};


struct PsrCoprocessor final : public A32::Coprocessor {
    Env *env;
    std::uint32_t ignored = 0;  // MSR SPSR's field mask
    explicit PsrCoprocessor(Env *e) : env(e) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg,
                                                     A32::CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned opc, A32::CoprocReg, A32::CoprocReg, unsigned) override {
        if (opc == 3) return cpuJitAccessPcSlot(env->cpu);
        return std::monostate{};
    }
    // opc 0: MSR CPSR, opc 1: MSR SPSR (written whole, mask ignored), opc 2: MOVS PC.
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned opc, A32::CoprocReg) override {
        if (opc == 0) return Callback{&Env::SetCpsr, env};
        if (opc == 2) return Callback{&Env::ExceptionReturn, env};
        return std::array<std::uint32_t *, 2>{cpuJitSpsr(env->cpu), &ignored};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, A32::CoprocReg, A32::CoprocReg, unsigned) override {
        return cpuJitSpsr(env->cpu);
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
};

// p15 slot: only the words jitCp15SlotWord takes get here (cpuJitPlain). c3 and the
// I-cache line invalidate have their own callbacks, the rest go through Cp15Xfer.
struct Cp15Coprocessor final : public A32::Coprocessor {
    Env *env;
    explicit Cp15Coprocessor(Env *e) : env(e) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg,
                                                     A32::CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg crm,
                                               unsigned opc2) override {
        if (two || opc1 || cp15Pc == ~0u) return std::monostate{};
        if (crn == A32::CoprocReg::C3) return Callback{&Env::Cp15Write, env->Cp15Current()};
        if (crn == A32::CoprocReg::C7 && crm == A32::CoprocReg::C5 && opc2 == 1)
            return Callback{&Env::Cp15InvalLine, env->Cp15Current()};
        if (crn == A32::CoprocReg::C7) return Callback{&Env::Cp15Xfer, env->Cp15Current()};
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg crn, A32::CoprocReg,
                                              unsigned) override {
        if (two || opc1 || cp15Pc == ~0u) return std::monostate{};
        if (crn == A32::CoprocReg::C3) return Callback{&Env::Cp15Read, env->Cp15Current()};
        return Callback{&Env::Cp15Xfer, env->Cp15Current()};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
};

// iWMMXt slots, coprocessors 0 and 1. Only encodings pxa270wmmxJitValid accepts get here
// (cpuJitPlain), so each Compile* rebuilds the word, with Rt r0 and Rt2 r1 standing for
// the core registers dynarmic already checked, and asks it again: an encoding that got
// here and isn't valid would be a bug in the plain check, and dynarmic asserts on it.
// For LDC and STC the word is the pre-indexed form with base r0; every addressing mode the
// plain check lets in is valid alike.
struct WmmxCoprocessor final : public A32::Coprocessor {
    Env *env;
    bool cp1;
    std::array<Env::WmmxXfer, 64> xfers;  // by N, store and wR; filled here, then read only
    WmmxCoprocessor(Env *e, bool c) : env(e), cp1(c) {
        for (unsigned i = 0; i < xfers.size(); i++)
            xfers[i] = Env::WmmxXfer{e, c, bool(i & 32), bool(i & 16), uint8_t(i & 15)};
    }

    std::optional<Callback> Transfer(bool store, bool two, bool N, A32::CoprocReg CRd,
                                     std::optional<std::uint8_t> option) {
        const uint32_t w = Word((store ? 0x0d800000 : 0x0d900000) | (N ? 1u << 22 : 0) | (R(CRd) << 12));
        if (two || option || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::nullopt;
        return Callback{&Env::WmmxTransfer, &xfers[(N ? 32 : 0) | (store ? 16 : 0) | R(CRd)]};
    }

    static uint8_t R(A32::CoprocReg r) { return uint8_t(r); }
    uint32_t Word(uint32_t bits) const { return 0xe0000000 | bits | (cp1 ? 0x100 : 0); }
    Env::WmmxOp *Op(uint32_t word, unsigned op1, unsigned a, unsigned b, unsigned c, unsigned op2) {
        return env->Wmmx(word, Env::WmmxOp{cpuJitWmmx(env->cpu), cp1, uint8_t(op1), uint8_t(a), uint8_t(b),
                                           uint8_t(c), uint8_t(op2)});
    }

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, A32::CoprocReg CRd, A32::CoprocReg CRn,
                                                     A32::CoprocReg CRm, unsigned opc2) override {
        const uint32_t w = Word(0x0e000000 | (opc1 << 20) | (R(CRn) << 16) | (R(CRd) << 12) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::nullopt;
        return Callback{&Env::WmmxCdp, Op(w, opc1, R(CRd), R(CRn), R(CRm), opc2)};
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override {
        const uint32_t w = Word(0x0e000010 | (opc1 << 21) | (R(CRn) << 16) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMcr, Op(w, opc1, R(CRn), R(CRm), 0, opc2)};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override {
        const uint32_t w = Word(0x0e100010 | (opc1 << 21) | (R(CRn) << 16) | (opc2 << 5) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMrc, Op(w, opc1, R(CRn), R(CRm), 0, opc2)};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        const uint32_t w = Word(0x0c410000 | (opc << 4) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMcrr, Op(w, opc, 0, 0, R(CRm), 0)};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, A32::CoprocReg CRm) override {
        const uint32_t w = Word(0x0c510000 | (opc << 4) | R(CRm));
        if (two || !cpuJitWmmx(env->cpu) || !pxa270wmmxJitValid(w)) return std::monostate{};
        return Callback{&Env::WmmxMrrc, Op(w, opc, 0, 0, R(CRm), 0)};
    }
    std::optional<Callback> CompileLoadWords(bool two, bool N, A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override {
        return Transfer(false, two, N, CRd, option);
    }
    std::optional<Callback> CompileStoreWords(bool two, bool N, A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override {
        return Transfer(true, two, N, CRd, option);
    }
};

}  // namespace

// One privilege level's JIT. The page table is calloc'd, so only the entries used take
// memory.
struct Level {
    Env env;
    PageTable *pages = nullptr;
    std::unique_ptr<A32::Jit> jit;
    std::shared_ptr<WmmxCoprocessor> wmmx[2];
    ~Level() {
        jit.reset();
        free(pages);
    }
};

struct FrondJit {
    ArmCpu *cpu = nullptr;
    // [0] user, [1] privileged. Both are made by jitCreate.
    std::unique_ptr<Level> level[2];
    uint32_t maxUnchecked = 0, hot = 1;
    bool singleStep = false;  // ARM920T probe diagnostic: one instruction per check, blocks end at each line
    A32::Jit::CompileThread mode{};
    unsigned ahead = 0;
    FrondJitAbort abort{};  // the last run's, for FROND_JIT_ABORT
    const Env *running = nullptr;  // the level jitRun is in, for jitRanNow
};

const FrondJitAbort &jitLastAbort(const FrondJit *j) { return j->abort; }

template <typename F>
void jitPrvEach(FrondJit *j, F f) {
    for (auto &l : j->level)
        if (l) f(l->env);
}

// Instructions between budget checks in generated code. It bounds how far a run can pass
// its budget, which audio admission reserves for. 64, 128 and 256 ran Bike and FrondBench
// equally fast, so the smallest. FROND_JIT_MAX_UNCHECKED overrides it for measuring, 0 for
// no limit.
static constexpr uint32_t kMaxUnchecked = 64;

// Times a block's start is reached before it's compiled. FROND_JIT_HOT overrides it for
// measuring and tests, 1 compiles everything at once.
static constexpr uint32_t kHot = 4;

static uint32_t jitPrvHot() {
    const char *v = getenv("FROND_JIT_HOT");
    if (v && *v) return std::max<uint32_t>(1, uint32_t(strtoul(v, nullptr, 10)));
    return kHot;
}

// Compile thread: on, unless FROND_JIT_THREAD=0 (compiles in place, as before). Sync mode
// (FROND_JIT_SYNC=1 or jitSetSync) waits for each compile where it was asked for, so runs
// stop at the same places every time; tests and replays use it.
static bool jitPrvSync = false;

void jitSetSync(bool sync) { jitPrvSync = sync; }

static A32::Jit::CompileThread jitPrvCompileThread() {
    const char *t = getenv("FROND_JIT_THREAD");
    if (t && *t && !strcmp(t, "0")) return A32::Jit::CompileThread::Off;
    const char *v = getenv("FROND_JIT_SYNC");
    if (v && *v) return strcmp(v, "0") ? A32::Jit::CompileThread::Sync : A32::Jit::CompileThread::On;
    return jitPrvSync ? A32::Jit::CompileThread::Sync : A32::Jit::CompileThread::On;
}

// Compiling ahead on the compile thread: blocks reached but not hot yet, and blocks up to
// kAhead links on from one asked for (FROND_JIT_AHEAD sets it, 0 is off). Off by default:
// about half of what it compiles is never used, and the guest thread pays for the snapshots,
// so it costs more CPU than it saves (TG50 and E2 on arm64). Off in sync mode unless set; it
// gives the same results there.
static constexpr unsigned kAhead = 0;

static unsigned jitPrvAhead(A32::Jit::CompileThread mode) {
    const char *v = getenv("FROND_JIT_AHEAD");
    if (v && *v) return unsigned(strtoul(v, nullptr, 10));
    return mode == A32::Jit::CompileThread::On ? kAhead : 0;
}

static uint32_t jitPrvMaxUnchecked() {
    const char *v = getenv("FROND_JIT_MAX_UNCHECKED");
    if (v && *v) return uint32_t(strtoul(v, nullptr, 10));
    return kMaxUnchecked;
}

// Code cache per JIT, address space reserved up front and used as code is written. The user
// level's is smaller on Android.
static constexpr size_t kCodeCache = 64 << 20;
#ifdef __ANDROID__
static constexpr size_t kUserCodeCache = 16 << 20;
#else
static constexpr size_t kUserCodeCache = kCodeCache;
#endif

static size_t jitPrvCodeCache = 0;

void jitSetCodeCacheSize(size_t bytes) { jitPrvCodeCache = bytes; }

static Level *jitPrvMakeLevel(FrondJit *j, bool privileged) {
    auto l = std::make_unique<Level>();
    l->pages = static_cast<PageTable *>(calloc(1, sizeof(PageTable)));
    if (!l->pages) abort();
    Env &e = l->env;
    e.cpu = j->cpu;
    e.pages = l->pages;
    e.privileged = privileged;
    e.hot = j->hot;
    A32::UserConfig conf;
    conf.callbacks = &e;
    e.arm920 = cpuGetModel(j->cpu) == ArmCpuModel::arm920t;
    e.lineBound = e.arm920 && j->singleStep;
    conf.arch_version = e.arm920 ? A32::ArchVersion::v4T : A32::ArchVersion::v5TE;
    if (e.lineBound)
        conf.optimizations &= ~(OptimizationFlag::BlockLinking | OptimizationFlag::ReturnStackBuffer |
                                OptimizationFlag::FastDispatch);
    conf.page_table = l->pages;
    conf.page_table_writes_via_callbacks = false;
    conf.page_table_write_protect_bit = true;
    conf.page_table_pointer_mask_bits = 1;
    conf.check_halt_on_link = true;
    conf.check_halt_on_memory_access = true;
    conf.max_unchecked_instructions = j->maxUnchecked;
    // Unaligned accesses go through the callbacks, which fault on them.
    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64;
    conf.only_detect_misalignment_via_page_table_on_page_boundary = false;
    conf.always_little_endian = true;
    conf.code_cache_size = jitPrvCodeCache ? jitPrvCodeCache : privileged ? kCodeCache : kUserCodeCache;
    conf.coprocessors[kPsrCoprocessor] = std::make_shared<PsrCoprocessor>(&e);
    conf.psr_coprocessor = kPsrCoprocessor;
    conf.coprocessors[15] = std::make_shared<Cp15Coprocessor>(&e);
    l->wmmx[0] = std::make_shared<WmmxCoprocessor>(&e, false);
    l->wmmx[1] = std::make_shared<WmmxCoprocessor>(&e, true);
    conf.coprocessors[0] = l->wmmx[0];
    conf.coprocessors[1] = l->wmmx[1];
    conf.coprocessor_unscaled_offsets = 1;  // iWMMXt WLDRB/H and WSTRB/H offsets are in bytes
    l->jit = std::make_unique<A32::Jit>(conf);
    e.jit = l->jit.get();
    e.maxInstructions = j->maxUnchecked;
    l->jit->SetCompileThread(e.lineBound ? A32::Jit::CompileThread::Off : j->mode);
    l->jit->SetCompileAhead(e.lineBound ? 0 : j->ahead);
    if (Level *o = j->level[!privileged].get()) {
        e.other = &o->env;
        o->env.other = &e;
    }
    if (stats.on) {
        std::lock_guard<std::mutex> lock(stats.jitsMutex);
        stats.jits.push_back(l->jit.get());
    }
    j->level[privileged] = std::move(l);
    return j->level[privileged].get();
}

FrondJit *jitCreate(ArmCpu *cpu, bool singleStep) {
    auto *j = new FrondJit;
    j->cpu = cpu;
    j->singleStep = singleStep;
    j->maxUnchecked = singleStep ? 1 : jitPrvMaxUnchecked();
    j->hot = cpuGetModel(cpu) == ArmCpuModel::arm920t && singleStep ? 1 : jitPrvHot();
    j->mode = jitPrvCompileThread();
    j->ahead = jitPrvAhead(j->mode);
    jitPrvMakeLevel(j, true);
    // The user level too, though some guests never run in user mode: making a JIT takes
    // 5-10 ms (its prelude and memory fallbacks), which here falls in session loading
    // rather than in the slice that first enters user mode.
    jitPrvMakeLevel(j, false);
    return j;
}

void jitDestroy(FrondJit *j) {
    if (stats.on) {
        std::lock_guard<std::mutex> lock(stats.jitsMutex);
        for (auto &l : j->level) {
            if (!l) continue;
            stats.AddCompiled(*l->jit);
            std::erase(stats.jits, l->jit.get());
        }
    }
    delete j;
}

uint32_t jitMaxUnchecked(const FrondJit *j) { return j->maxUnchecked; }

uint64_t jitDomainCalls() { return domainCalls.load(std::memory_order_relaxed); }

uint32_t jitRanNow(const FrondJit *j) {
    const Env *e = j->running;
    if (!e) return 0;
    const int64_t ran = int64_t(e->budget) - e->jit->TicksRemainingNow();
    return ran <= 0 ? 0 : uint32_t(std::min<int64_t>(ran, UINT32_MAX));
}

void jitExecutionCounts(const FrondJit *j, uint64_t *blocks, uint64_t *instructions) {
    *blocks = *instructions = 0;
    for (const auto &l : j->level) {
        if (!l) continue;
        *blocks += l->env.blocks;
        *instructions += l->env.instructions;
    }
}

// The counts go to the privileged level, the user level's start again.
void jitSetExecutionCounts(FrondJit *j, uint64_t blocks, uint64_t instructions) {
    jitPrvEach(j, [](Env &e) { e.blocks = e.instructions = 0; });
    j->level[1]->env.blocks = blocks;
    j->level[1]->env.instructions = instructions;
}

void jitCompileCounts(const FrondJit *j, uint64_t *stale, uint64_t *aheadCompiled, uint64_t *aheadUsed) {
    *stale = *aheadCompiled = *aheadUsed = 0;
    for (const auto &l : j->level) {
        if (!l) continue;
        const A32::Jit::CompileStats c = l->jit->GetCompileStats();
        *stale += c.stale;
        *aheadCompiled += c.ahead_compiled;
        *aheadUsed += c.ahead_used;
    }
}

uint64_t jitCacheClears(const FrondJit *j) {
    uint64_t clears = 0;
    for (const auto &l : j->level) {
        if (l) clears += l->jit->GetCompileStats().cache_clears;
    }
    return clears;
}

void jitInvalidate(FrondJit *j, uint32_t va, uint32_t size) {
    stats.invalRange++;
    jitPrvEach(j, [&](Env &e) {
        if (e.lineBound) e.jit->InvalidateCacheRange(va, size);
        else if (!e.arm920 && !e.unwatched.empty())
            e.DropUnwatched(va >> 12, uint32_t((uint64_t(va) + size - 1) >> 12));
    });
}

void jitGuestInvalidateAll(FrondJit *j, uint32_t pc) {
    stats.invalAll++;
    auto &st = stats.invalAllPc[pc];
    if (!st.first++ && stats.on) cpuJitFetch(j->cpu, pc & ~3u, &st.second);
    jitPrvEach(j, [](Env &e) {
        if (e.lineBound) e.jit->ClearCache();
        else if (!e.arm920 && !e.unwatched.empty()) e.DropUnwatched(0, ~0u);
    });
}

// Both levels clear together: a clear forgets the CPU's RAM code flags, which the other
// level's code relies on too.
void jitClear(FrondJit *j) {
    jitPrvEach(j, [](Env &e) { e.Clear(); });
}

void jitCparWritten(FrondJit *j, bool changed) {
    stats.cparWrites++;
    if (!changed) return;
    stats.cparChanges++;
    jitPrvEach(j, [](Env &e) { e.ClearCode(); });
}

// Fields as dynarmic's decoder takes them from an ARM word with a condition other than NV.
bool jitWmmxCompiles(FrondJit *j, uint32_t w) {
    const unsigned cp = (w >> 8) & 15;
    if (cp > 1 || (w >> 28) == 15) return false;
    WmmxCoprocessor &c = *j->level[1]->wmmx[cp];
    const auto reg = [&](unsigned shift) { return A32::CoprocReg((w >> shift) & 15); };
    const auto isCallback = [](const auto &v) { return std::holds_alternative<A32::Coprocessor::Callback>(v); };
    if ((w & 0x0f000010) == 0x0e000000)
        return c.CompileInternalOperation(false, (w >> 20) & 15, reg(12), reg(16), reg(0), (w >> 5) & 7).has_value();
    if ((w & 0x0f100010) == 0x0e000010)
        return isCallback(c.CompileSendOneWord(false, (w >> 21) & 7, reg(16), reg(0), (w >> 5) & 7));
    if ((w & 0x0f100010) == 0x0e100010)
        return isCallback(c.CompileGetOneWord(false, (w >> 21) & 7, reg(16), reg(0), (w >> 5) & 7));
    if ((w & 0x0ff00000) == 0x0c400000) return isCallback(c.CompileSendTwoWords(false, (w >> 4) & 15, reg(0)));
    if ((w & 0x0ff00000) == 0x0c500000) return isCallback(c.CompileGetTwoWords(false, (w >> 4) & 15, reg(0)));
    if ((w & 0x0e000000) == 0x0c000000) {  // LDC, STC; P = U = D = W = 0 is undefined
        const bool p = w & (1 << 24), u = w & (1 << 23), d = w & (1 << 22), wb = w & (1 << 21);
        if (!p && !u && !d && !wb) return false;
        std::optional<std::uint8_t> option;
        if (!p && !wb && u) option = uint8_t(w);
        return (w & (1 << 20)) ? c.CompileLoadWords(false, d, reg(12), option).has_value()
                               : c.CompileStoreWords(false, d, reg(12), option).has_value();
    }
    return false;
}

void jitCodeWritten(FrondJit *j, uint8_t *hostPage) {
    jitPrvEach(j, [&](Env &e) { e.CodeWritten(hostPage); });
}

void jitCodeWrittenPa(FrondJit *j, uint32_t pa) {
    jitPrvEach(j, [&](Env &e) { e.CodeWrittenPa(pa >> 12); });
}

void jitWriteProtect(FrondJit *j, const uint8_t *host, uint32_t size) {
    jitPrvEach(j, [&](Env &e) {
        if (!e.writable.empty()) e.Protect(host, size);
    });
}

void jitWriteProtectPage(FrondJit *j, uint8_t *hostPage) {
    jitPrvEach(j, [&](Env &e) { e.ProtectHost(hostPage); });
}

// Runs the JIT for the CPU's current privilege level. A privilege change in generated code
// ends the run as if the budget ran out, and the next run goes to the other level.
uint32_t jitRun(FrondJit *j, uint32_t *regs, uint32_t &cpsr, uint32_t cycles, int &stop) {
    const bool privileged = cpuJitPrivileged(j->cpu);
    Level *l = j->level[privileged].get();
    if (!l) l = jitPrvMakeLevel(j, privileged);
    Env &e = l->env;
    A32::Jit &jit = *l->jit;
    if (e.resetState) {
        e.resetState = false;
        jit.Reset();
        jit.ClearCache();  // Reset drops the pending cache clear's halt flag
    }
    e.CheckGen();
    if (const uint32_t g = cpuJitMapGen(e.cpu); g != e.mapGen) {
        e.mapGen = g;
        if (!e.code.empty()) e.CheckMappings();
    }
    std::memcpy(jit.Regs().data(), regs, 16 * sizeof(uint32_t));
    jit.SetCpsr(cpsr);
    e.budget = cycles;
    e.ran = 0;
    e.undoTick = false;
    e.dataAbort.set = false;
    j->running = &e;
    const HaltReason hr = jit.Run();
    j->running = nullptr;
    jit.ClearHalt(kInterpret | kSlowPath | kFault | kCold | kPrivilege | kSwi);
    if (e.rewindPc != ~0u) {
        jit.Regs()[15] = e.rewindPc;
        e.rewindPc = ~0u;
    }
    std::memcpy(regs, jit.Regs().data(), 16 * sizeof(uint32_t));
    cpsr = jit.Cpsr();
    uint64_t ran = e.ran;
    if (e.undoTick && ran) ran--;
    e.instructions += ran;
    stats.runs++;
    stats.ran += ran;
    if (Has(hr, kFault)) stats.fault++;
    if (Has(hr, kInterpret)) stats.interpret++;
    if (Has(hr, kSlowPath)) stats.halt++;
    if (e.dataAbort.set) {
        j->abort = e.dataAbort.fault;
        e.dataAbort.set = false;
        stop = FROND_JIT_ABORT;
    } else if (Has(hr, kFault) || Has(hr, kInterpret))
        stop = FROND_JIT_INTERPRET;
    else if (Has(hr, kSlowPath))
        stop = FROND_JIT_HALT;
    else if (Has(hr, kCold))
        stop = FROND_JIT_COLD;
    else
        stop = FROND_JIT_BUDGET;
    return uint32_t(ran);
}
