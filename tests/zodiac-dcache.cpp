#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <utility>
#include "cpu_diagnostics.h"

#include "CPU.h"
#include "MMU.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
constexpr uint32_t bytes = 0x40000, table = 0x4000, data = 0x10000;
constexpr uint32_t ctl = 0x1000, ttb = 0x1020, dacr = 0x1040, tlb = 0x1060,
    ld = 0x1100, st = 0x1120, ldb = 0x1140, stb = 0x1160, ldh = 0x1180, sth = 0x11a0,
    clean = 0x1200, cleanInval = 0x1220, inval = 0x1240, cleanIndex = 0x1260,
    cleanInvalIndex = 0x1280, allD = 0x12a0, allI = 0x12c0, lineI = 0x12e0,
    drain = 0x1300, allID = 0x1320, swp = 0x1340, swpb = 0x1360;
struct Access { uint32_t pa, size, value; bool write; };
struct Machine {
    ArmCpu* cpu = nullptr;
    ArmMem* mem;
    std::vector<uint8_t> ram = std::vector<uint8_t>(bytes);
    std::vector<Access> accesses;
    uint32_t readLimit = bytes, writeLimit = bytes;
    bool deny = false, failedWalkRetry = false;
    explicit Machine(bool fast) {
        assert(cpuSetFastEngine(fast));
        mem = memInit();
        assert(memRegionAdd(mem, 0, bytes, access, this));
        auto* patches = initPatchDispatch();
        cpu = cpuInit(ctl, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                      patches, new PacePatch{}, createSystemState(), ArmCpuModel::arm920t, true);
        patchDispatchSetCpu(patches, cpu);
        cpuArm920SetMemoryCheck(cpu, check, this);
    }
    static bool check(void* ctx, uint32_t pa, uint32_t size, bool write) {
        auto& m = *static_cast<Machine*>(ctx);
        return !m.deny && size && uint64_t(pa) + size <= (write ? m.writeLimit : m.readLimit);
    }
    static bool access(void* ctx, uint32_t pa, uint_fast8_t size, bool write, void* buf) {
        auto& m = *static_cast<Machine*>(ctx);
        if (uint64_t(pa) + size > bytes || size > 4) return false;
        uint32_t value = 0;
        if (write) {
            std::memcpy(&value, buf, size);
            std::memcpy(m.ram.data() + pa, buf, size);
            if (m.cpu) cpuDropTranslatedCode(m.cpu);
        } else {
            std::memcpy(&value, m.ram.data() + pa, size);
            std::memcpy(buf, &value, size);
        }
        if (!memIsInstructionFetch(m.mem)) m.accesses.push_back({pa, uint32_t(size), value, write});
        return true;
    }
    void word(uint32_t pa, uint32_t value) { std::memcpy(ram.data() + pa, &value, 4); }
    uint32_t word(uint32_t pa) const { uint32_t v; std::memcpy(&v, ram.data() + pa, 4); return v; }
    void reset(uint32_t attr = 0xc0e) {
        std::memset(ram.data(), 0, ram.size());
        cpuReset(cpu, ctl);
        cpuArm920SetIcacheIndexCompatibility(cpu, false);
        cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xd3);
        for (unsigned i = 0; i < 15; ++i) cpuSetReg(cpu, i, 0);
        readLimit = writeLimit = bytes; deny = failedWalkRetry = false;
        word(ctl, 0xee010f10); word(ttb, 0xee020f10); word(dacr, 0xee030f10);
        word(tlb, 0xee080f17); word(ld, 0xe5902000); word(st, 0xe5801000);
        word(ldb, 0xe5d02000); word(stb, 0xe5c01000);
        word(ldh, 0xe1d020b0); word(sth, 0xe1c010b0);
        word(clean, 0xee070f3a); word(cleanInval, 0xee070f3e); word(inval, 0xee070f36);
        word(cleanIndex, 0xee070f5a); word(cleanInvalIndex, 0xee070f5e);
        word(allD, 0xee070f16); word(allI, 0xee070f15); word(lineI, 0xee070f35);
        word(drain, 0xee070f9a); word(allID, 0xee070f17);
        word(swp, 0xe1002091); word(swpb, 0xe1402091);
        word(0x1400, 0xee150f10); word(0x1420, 0xee160f10); word(0x1440, 0xee110f10);
        word(0x1460, 0xee050f10); word(0x1480, 0xee060f10);
        word(table, attr); word(table + 4, attr); word(table + 8, 0x44e);
        word(data, 0x11223344); word(data + 4, 0x55667788); word(data + 16, 0x99aabbcc);
        cp(ttb, table); cp(dacr, 1); accesses.clear();
    }
    void step(uint32_t pc) {
        cpuSetReg(cpu, 15, pc);
        assert((cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(cpu, 1) == 1));
    }
    void cp(uint32_t pc, uint32_t value) { cpuSetReg(cpu, 0, value); step(pc); }
    void enable(uint32_t value = 0x407d) { cp(ctl, value); accesses.clear(); }
    uint32_t load(uint32_t va = data, uint32_t insn = ld) {
        cpuSetReg(cpu, 0, va); step(insn); return cpuGetRegisters(cpu)[2];
    }
    void store(uint32_t va, uint32_t value, uint32_t insn = st) {
        cpuSetReg(cpu, 0, va); cpuSetReg(cpu, 1, value); step(insn);
    }
    std::vector<Access> dataAccesses() const {
        std::vector<Access> result;
        for (auto a : accesses) if (a.pa >= data) result.push_back(a);
        return result;
    }
    void noData() const { assert(dataAccesses().empty()); }
    void und() const {
        assert((cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_UND);
        assert(cpuGetRegisters(cpu)[15] == 4);
    }
};
void same(Machine& a, Machine& b) {
    assert(!std::memcmp(cpuGetRegisters(a.cpu), cpuGetRegisters(b.cpu), 16 * sizeof(uint32_t)));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_CPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_CPSR));
    assert(cpuGetRegExternal(a.cpu, ARM_REG_NUM_SPSR) == cpuGetRegExternal(b.cpu, ARM_REG_NUM_SPSR));
    assert(a.ram == b.ram);
    assert(a.failedWalkRetry == b.failedWalkRetry);
    // The directed fault case reads the invalid descriptor once in both engines: Fast takes the
    // JIT's data abort without walking the table again.
    for (auto* m : {&a, &b}) {
        unsigned failedReads = 0;
        for (auto access : m->accesses)
            if (m->failedWalkRetry && access.pa == table + 4 && access.size == 4 &&
                !access.write && !access.value) ++failedReads;
        assert(failedReads == (m->failedWalkRetry ? 1u : 0u));
    }
    const auto& ax = a.accesses;
    const auto& bx = b.accesses;
    assert(ax.size() == bx.size());
    for (unsigned i = 0; i < ax.size(); ++i) {
        const auto x = ax[i], y = bx[i];
        assert(x.pa == y.pa && x.size == y.size && x.value == y.value && x.write == y.write);
    }
    const auto x = cpuArm920DataGuard(a.cpu), y = cpuArm920DataGuard(b.cpu);
    assert(x.active == y.active && x.address == y.address && x.size == y.size && x.write == y.write);
}
}

int main() {
    unsetenv("FROND_JIT");
    Machine exact(false), fast(true);
    auto run = [&](uint32_t attr, auto body) {
        for (auto* m : {&exact, &fast}) { m->reset(attr); body(*m); }
        same(exact, fast);
    };
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data,0xdeadbeef);
        assert(m.word(data)==0x11223344);
        using Records=std::vector<std::pair<std::string,std::vector<uint32_t>>>;
        auto collect=[](void* ctx,const char* name,const uint32_t* words,uint32_t count) {
            static_cast<Records*>(ctx)->push_back({name,{words,words+count}});
        };
        Records first,second;
        const auto original=m.ram;
        const auto accessCount=m.accesses.size();
        const auto exceptions=cpuExceptionSequence(m.cpu);
        const auto flushes=mmuFlushCount(cpuGetMMU(m.cpu));
        cpuEmitArm920Diagnostic(m.cpu,{&first,collect,nullptr});
        cpuEmitArm920Diagnostic(m.cpu,{&second,collect,nullptr});
        assert(first==second && m.ram==original && m.accesses.size()==accessCount);
        assert(cpuExceptionSequence(m.cpu)==exceptions && mmuFlushCount(cpuGetMMU(m.cpu))==flushes);
        bool dirty=false,translation=false,buffer=false;
        for (const auto& record:first) {
            const auto& w=record.second;
            if (record.first=="dcache-line" && w[2]==data) {
                assert(w[3]==data && w[4]==1 && w[5]==1 && w[6]==0xdeadbeef);
                dirty=true;
            }
            if (record.first=="tlb" && w[0]==data) {
                assert(w[1]==data && w[2]==3 && w[3]==0 && w[5]==1 && w[6]==1);
                translation=true;
            }
            if (record.first=="pending-write-buffer-words") {assert(w[0]==0);buffer=true;}
        }
        assert(dirty && translation && buffer);
        m.accesses.clear(); assert(m.load()==0xdeadbeef); m.noData();
        m.cp(clean,data); assert(m.word(data)==0xdeadbeef);
    });
    // Ccr and descriptor C/B independently select bypass, WT and WB.
    for (uint32_t attr : {0xc02u, 0xc06u, 0xc0au, 0xc0eu}) {
        for (uint32_t control : {0x4079u, 0x407du}) run(attr, [&](Machine& m) {
            m.enable(control);
            assert(m.load() == 0x11223344);
            const auto reads = m.dataAccesses();
            const bool cached = (control & 4) && (attr & 8);
            assert(reads.size() == (cached ? 8 : 1));
            m.accesses.clear();
            m.word(data, 0xaabbccdd);
            assert(m.load() == (cached ? 0x11223344 : 0xaabbccdd));
            assert(m.dataAccesses().size() == (cached ? 0 : 1));
            m.accesses.clear();
            m.store(data, 0xdeadbeef);
            const bool wb = cached && (attr & 4);
            assert(m.word(data) == (wb ? 0xaabbccdd : 0xdeadbeef));
            assert(m.dataAccesses().size() == (wb ? 0 : 1));
            assert(m.load() == 0xdeadbeef);
        });
    }
    for (uint32_t attr : {0xc0au, 0xc0eu}) run(attr, [](Machine& m) {
        m.enable();
        m.store(data, 0x12345678); // A store miss never allocates.
        assert(m.dataAccesses().size() == 1);
        m.word(data, 0xabcdef01); m.accesses.clear();
        assert(m.load() == 0xabcdef01 && m.dataAccesses().size() == 8);
        m.accesses.clear();
        m.store(data + 1, 0x66, stb); m.store(data + 2, 0x7788, sth);
        assert(m.load() == 0x77886601);
        assert(m.load(data + 1, ldb) == 0x66 && m.load(data + 2, ldh) == 0x7788);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678);
        m.word(data + 16, 0xdecafbad); m.accesses.clear();
        m.cp(clean, data);
        const auto writes = m.dataAccesses(); assert(writes.size() == 4);
        for (unsigned i = 0; i < 4; ++i) assert(writes[i].write && writes[i].pa == data + i * 4 && writes[i].size == 4);
        assert(m.word(data) == 0x12345678 && m.word(data + 16) == 0xdecafbad);
        m.word(data, 0x87654321); m.accesses.clear();
        assert(m.load() == 0x12345678); m.cp(clean, data); m.noData();
        m.cp(inval, data); assert(m.load() == 0x87654321);
        m.store(data + 16, 0x22222222); m.accesses.clear(); m.cp(cleanInval, data);
        assert(m.dataAccesses().size() == 4 && m.word(data + 16) == 0x22222222);
        m.word(data, 0x33333333); m.accesses.clear(); assert(m.load() == 0x33333333);
    });
    for (uint32_t operation : {inval, allD, allID}) run(0xc0e, [&](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678); m.accesses.clear();
        m.cp(operation, operation == inval ? data : 0); m.noData();
        assert(m.word(data) == 0x11223344 && m.load() == 0x11223344);
    });
    for (uint32_t operation : {cleanIndex, cleanInvalIndex}) run(0xc0e, [&](Machine& m) {
        m.enable();
        // Every segment and all 64 indices contain dirty data when swept.
        for (unsigned index = 0; index < 64; ++index) for (unsigned segment = 0; segment < 8; ++segment) {
            const uint32_t va = data + index * 256 + segment * 32;
            m.load(va); m.store(va, va); m.store(va + 16, ~va);
        }
        m.accesses.clear();
        for (unsigned index = 0; index < 64; ++index) for (unsigned segment = 0; segment < 8; ++segment)
            m.cp(operation, (index << 26) | (segment << 5));
        assert(m.dataAccesses().size() == 64 * 8 * 8);
        for (unsigned index = 0; index < 64; ++index) for (unsigned segment = 0; segment < 8; ++segment) {
            const uint32_t va = data + index * 256 + segment * 32;
            assert(m.word(va) == va && m.word(va + 16) == ~va);
        }
        m.accesses.clear(); m.word(data, 0x99);
        assert(m.load() == (operation == cleanIndex ? data : 0x99));
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678);
        m.accesses.clear();
        for (unsigned i = 1; i <= 64; ++i) m.load(data + i * 256);
        assert(m.word(data) == 0x12345678);
        const auto a = m.dataAccesses();
        unsigned writes = 0; for (auto access : a) writes += access.write;
        assert(writes == 4 && a.size() == 64 * 8 + 4);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); assert(m.load() == 0x11223344); assert(m.load(data + 0x100000) == 0x11223344);
        m.store(data, 0x12345678); assert(m.load(data + 0x100000) == 0x11223344);
        m.cp(clean, data); assert(m.word(data) == 0x12345678);
        assert(m.load(data + 0x100000) == 0x11223344);
        m.cp(inval, data + 0x100000); assert(m.load(data + 0x100000) == 0x12345678);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(table); m.store(table + 4, 0);
        assert(m.word(table + 4) == 0xc0e);
        m.cp(tlb, 0); assert(m.load(data + 0x100000) == 0x11223344);
        m.cp(clean, table); assert(m.word(table + 4) == 0);
        m.cp(tlb, 0); m.accesses.clear(); m.failedWalkRetry = true; m.load(data + 0x100000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        m.noData(); m.step(0x1400); assert(cpuGetRegisters(m.cpu)[0] == 5);
        m.step(0x1420); assert(cpuGetRegisters(m.cpu)[0] == data + 0x100000);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(data + 0x100000); m.store(data + 0x100000, 0x12345678);
        m.word(table + 4, 0x90000c0e); m.cp(tlb, 0); m.accesses.clear();
        m.cp(clean, data + 0x100000);
        assert(m.word(data) == 0x12345678 && m.dataAccesses().size() == 4);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678);
        m.cp(ctl, 0x4079); m.accesses.clear();
        assert(m.load() == 0x11223344); m.store(data, 0x99999999);
        m.enable(); assert(m.load() == 0x12345678);
        m.cp(ctl, 0x4079); m.cp(clean, data); assert(m.word(data) == 0x12345678);
        m.enable(); m.store(data, 0x77777777); cpuReset(m.cpu, ctl);
        assert(!cpuArm920DataGuard(m.cpu).active && m.word(data) == 0x12345678);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(0x7d);
        for (unsigned i = 0; i < 80; ++i) { m.load(data + i * 256); m.store(data + i * 256, i + 1); }
        for (unsigned index = 0; index < 64; ++index) m.cp(cleanIndex, index << 26);
        for (unsigned i = 0; i < 80; ++i) assert(m.word(data + i * 256) == i + 1);
    });
    for (uint32_t operation : {clean, cleanInval, inval, allD, allI, allID}) run(0xc0e, [&](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678); m.accesses.clear();
        m.cp(operation, operation == clean || operation == cleanInval || operation == inval ? data + 1 : 1);
        m.und(); m.noData(); assert(!cpuArm920DataGuard(m.cpu).active && m.word(data) == 0x11223344);
        cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, 0xd3); m.cp(clean, data); assert(m.word(data) == 0x12345678);
    });
    for (uint32_t operand : {0u, 1u, 0xffffffffu, 0xc000127fu}) run(0xc0e, [&](Machine& m) {
        constexpr uint32_t code = 0x18000;
        m.word(code, 0xe3a03001); m.enable(0x507d); m.step(code);
        assert(cpuGetRegisters(m.cpu)[3] == 1);
        m.load(); m.store(data, 0x12345678); m.load(data + 0x100000);
        m.word(code, 0xe3a03002); m.word(table + 4, 0); m.accesses.clear();
        cpuSetReg(m.cpu, 0, operand);
        uint32_t regs[15]; std::memcpy(regs, cpuGetRegisters(m.cpu), sizeof(regs));
        const auto cpsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR);
        const auto spsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR);
        m.step(drain);
        assert(cpuGetRegisters(m.cpu)[15] == drain + 4);
        assert(!std::memcmp(regs, cpuGetRegisters(m.cpu), sizeof(regs)));
        assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == cpsr);
        assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR) == spsr);
        m.noData(); assert(m.word(data) == 0x11223344);
        assert(m.load() == 0x12345678 && m.load(data + 0x100000) == 0x11223344);
        m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1); m.noData();
        m.cp(clean, data); assert(m.word(data) == 0x12345678);
        m.cp(lineI, code); m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 2);
    });
    for (uint32_t operand : {0u, 1u, 0xffffffffu, 0x20420520u}) run(0xc0e, [&](Machine& m) {
        constexpr uint32_t code = 0x18000, compatibility = 0x1380;
        m.word(compatibility, 0xee070f55);
        m.word(code, 0xe3a03001); m.word(code + 0x100, 0xe3a04001);
        m.enable(0x507d); m.step(code); m.step(code + 0x100);
        m.load(); m.store(data, 0x12345678); m.load(data + 0x100000);
        m.word(code, 0xe3a03002); m.word(code + 0x100, 0xe3a04002);
        m.word(table + 4, 0); m.accesses.clear();
        cpuArm920SetIcacheIndexCompatibility(m.cpu, true);
        cpuSetReg(m.cpu, 0, operand);
        uint32_t regs[15]; std::memcpy(regs, cpuGetRegisters(m.cpu), sizeof(regs));
        const auto cpsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR);
        const auto spsr = cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR);
        m.step(compatibility);
        assert(cpuGetRegisters(m.cpu)[15] == compatibility + 4);
        assert(!std::memcmp(regs, cpuGetRegisters(m.cpu), sizeof(regs)));
        assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) == cpsr);
        assert(cpuGetRegExternal(m.cpu, ARM_REG_NUM_SPSR) == spsr);
        m.noData(); assert(m.word(data) == 0x11223344);
        assert(m.load() == 0x12345678 && m.load(data + 0x100000) == 0x11223344);
        m.step(code); m.step(code + 0x100);
        assert(cpuGetRegisters(m.cpu)[3] == 2 && cpuGetRegisters(m.cpu)[4] == 2);
        m.cp(clean, data); assert(m.word(data) == 0x12345678);
        cpuArm920SetIcacheIndexCompatibility(m.cpu, false);
        m.step(compatibility); m.und();
    });
    // A populated cache hit must still fail the current domain/permission checks.
    run(0xc0e, [](Machine& m) {
        m.enable(); m.cp(dacr, 0x11); m.load(0x210000); m.store(0x210000, 0x12345678);
        cpuSetReg(m.cpu, ARM_REG_NUM_CPSR, 0xd0); m.accesses.clear(); m.load(0x210000);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        assert(cpuGetRegisters(m.cpu)[15] == 0x10); m.noData();
        m.step(0x1400); assert(cpuGetRegisters(m.cpu)[0] == 0x2d);
        m.step(0x1420); assert(cpuGetRegisters(m.cpu)[0] == 0x210000);
    });
    run(0xc0e, [](Machine& m) {
        const uint32_t code = data;
        m.word(code, 0xe3a03001); m.enable(0x507d);
        m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1);
        m.load(code); m.store(code, 0xe3a03002);
        assert(m.word(code) == 0xe3a03001);
        m.cp(lineI, code); m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1);
        m.cp(clean, code); assert(m.word(code) == 0xe3a03002);
        m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 1);
        m.cp(drain, 0); m.cp(lineI, code); m.step(code); assert(cpuGetRegisters(m.cpu)[3] == 2);
    });
    // Unsupported cache/buffer targets stop before a backing access or architectural data fault.
    for (bool write : {false, true}) for (uint32_t attr : {0xc06u, 0xc0au, 0xc0eu}) run(attr, [&](Machine& m) {
        m.cp(0x1460, 0x55); m.cp(0x1480, 0x12345678);
        m.enable(); m.deny = true; m.accesses.clear();
        if (write) m.store(data, 0x12345678); else m.load();
        m.und(); m.noData();
        const auto guard = cpuArm920DataGuard(m.cpu);
        assert(guard.active && guard.address == data && guard.write == write);
        assert(m.word(data) == 0x11223344);
        m.step(0x1400); assert(cpuGetRegisters(m.cpu)[0] == 0x55);
        m.step(0x1420); assert(cpuGetRegisters(m.cpu)[0] == 0x12345678);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.readLimit = data + 4; m.load(); m.und(); m.noData();
        const auto guard = cpuArm920DataGuard(m.cpu); assert(guard.active && guard.size == 32);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.writeLimit = data; m.accesses.clear(); m.store(data, 0x12345678);
        m.und(); m.noData(); assert(m.word(data) == 0x11223344);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678);
        m.writeLimit = data; m.accesses.clear(); m.cp(clean, data);
        m.und(); m.noData(); assert(m.word(data) == 0x11223344);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678); m.store(data + 16, 0x99999999);
        m.writeLimit = data + 16; m.accesses.clear(); m.cp(clean, data);
        m.und(); m.noData(); // Both dirty halves are checked before either is written.
        assert(m.word(data) == 0x11223344 && m.word(data + 16) == 0x99aabbcc);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678); m.writeLimit = data;
        m.accesses.clear();
        for (unsigned i = 1; i <= 64 && !cpuArm920DataGuard(m.cpu).active; ++i) m.load(data + i * 256);
        m.und(); assert(m.word(data) == 0x11223344);
        const auto a = m.dataAccesses(); assert(a.size() == 63 * 8);
        for (auto access : a) assert(!access.write);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); m.load(); m.store(data, 0x12345678); m.accesses.clear();
        m.cp(drain, 0); m.noData(); assert(m.word(data) == 0x11223344);
        m.cp(clean, data); assert(m.word(data) == 0x12345678);
    });
    run(0xc0e, [](Machine& m) {
        m.enable(); assert(m.load(data + 1) == 0x44112233);
        m.cp(ctl, 0x407f); m.accesses.clear(); m.load(data + 1);
        assert((cpuGetRegExternal(m.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_ABT);
        m.noData(); m.step(0x1400); assert(cpuGetRegisters(m.cpu)[0] == 1);
    });
    for (uint32_t attr : {0xc02u, 0xc06u, 0xc0au}) run(0xc0e, [&](Machine& m) {
        m.enable(); m.load(data + 0x100000); m.store(data + 0x100000, 0x12345678);
        m.word(table + 4, attr); m.cp(tlb, 0); m.accesses.clear(); m.load(data + 0x100000);
        m.und(); m.noData(); assert(m.word(data) == 0x11223344);
    });
    for (uint32_t operation : {swp, swpb}) for (uint32_t attr : {0xc02u, 0xc06u, 0xc0au, 0xc0eu})
        for (uint32_t control : {0x4079u, 0x407du}) run(attr, [&](Machine& m) {
        m.enable(control); m.store(data, 0xabcdef01, operation);
        assert(cpuGetRegisters(m.cpu)[2] == (operation == swp ? 0x11223344 : 0x44));
        const uint32_t updated = operation == swp ? 0xabcdef01 : 0x11223301;
        const bool wb = (control & 4) && (attr & 8) && (attr & 4);
        assert(m.word(data) == (wb ? 0x11223344 : updated));
        m.cp(clean, data); assert(m.word(data) == updated);
    });
    for (uint32_t operation : {swp, swpb}) for (uint32_t attr : {0xc02u, 0xc06u, 0xc0au, 0xc0eu}) run(attr, [&](Machine& m) {
        m.enable(); m.writeLimit = data; m.accesses.clear(); m.store(data, 0xabcdef01, operation);
        m.und(); m.noData(); assert(m.word(data) == 0x11223344);
    });
    uint64_t blocks = 0, instructions = 0;
    assert(cpuJitExecutionCounts(fast.cpu, &blocks, &instructions) && blocks && instructions);
    std::puts("ARM920T data cache Exact/Fast directed checks passed");
}
