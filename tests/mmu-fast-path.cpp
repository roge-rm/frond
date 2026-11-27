#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MMU_internal.h"
#include "RAM.h"
#include "memory_buffer.h"

static std::vector<uint8_t> memory(0x10000);
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (uint64_t(addr)+size>memory.size()) return false;
    if (write) std::memcpy(memory.data()+addr,buf,size);
    else std::memcpy(buf,memory.data()+addr,size);
    return true;
}
static void put(uint32_t addr,uint32_t word) { std::memcpy(memory.data()+addr,&word,4); }
static void testArm920SubpageWrites() {
    constexpr uint32_t ttb = 0x4000, l2 = 0x8000, alternate = 0x8400;
    constexpr uint32_t mixed = 3u << 4 | 1u << 6 | 2u << 10 | 2u;
    auto* mem = memInit();
    MemoryBuffer ram{};
    assert(memoryBufferAllocate(&ram, 0x10000));
    ramSetNoFramebuffer(ramInit(mem, nullptr, 0, 0x10000, &ram, true));

    auto* exact = mmuInit(mem, false);
    auto* fast = mmuInit(mem, false);
    auto store = [&](uint32_t pa, uint32_t value) { assert((memAccess<4, true>(mem, pa, &value))); };
    auto compare = [&](uint32_t va, bool privileged) {
        const auto result = mmuTranslateCached<true, false, true>(fast, va, privileged);
        assert(result == mmuTranslate(exact, va, privileged, true));
        return result;
    };
    auto domain = [&](uint32_t value) {
        mmuSetDomainCfg(exact, value);
        mmuSetDomainCfg(fast, value);
    };
    for (auto* mmu : {exact, fast}) mmuSetTTP(mmu, ttb);

    store(ttb, l2 | 1);
    store(l2, 0x10000000 | mixed | 8);
    domain(1);
    for (int repeat = 0; repeat < 2; ++repeat)
        for (uint32_t offset : {0x28u, 0x424u, 0xa04u, 0xffcu})
            for (bool privileged : {true, false}) compare(offset, privileged);
    assert(fast->subpageWrites[0].valid && fast->subpageWrites[3].valid);
    assert(!mmuTlbLive(fast, 0));
    domain(0);
    assert(MMU_TRANSLATE_RESULT_FSR(compare(0x28, true)) == 0x0b);
    domain(3);
    assert(MMU_TRANSLATE_RESULT_PA(compare(0xa04, false)) == 0x10000a04);

    // Both descriptor words can change before a guest TLB flush.
    store(l2, 0x20000000 | mixed | 4);
    assert(compare(0x28, true) == (1ull << 61 | 0x20000028));
    store(ttb, alternate | 1);
    store(alternate, 0x30000000 | mixed);
    assert(MMU_TRANSLATE_RESULT_PA(compare(0x28, true)) == 0x30000028);
    store(alternate, 0);
    assert(MMU_TRANSLATE_RESULT_FSR(compare(0x28, true)) == 7);

    store(alternate, 0x30000000 | mixed);
    assert(MMU_TRANSLATE_RESULT_PA(compare(0x28, true)) == 0x30000028);
    assert(memHoldRam(mem, true));
    assert(MMU_TRANSLATE_RESULT_FSR(compare(0x28, true)) == 5);
    assert(memHoldRam(mem, false));
    assert(MMU_TRANSLATE_RESULT_PA(compare(0x28, true)) == 0x30000028);

    // An ordinary walk can fill the guest TLB while the old subpage memo still exists.
    store(alternate, 0x10000ff2);
    for (auto* mmu : {exact, fast})
        assert(MMU_TRANSLATE_RESULT_PA(mmuTranslate(mmu, 0x28, true, true)) == 0x10000028);
    assert(mmuTlbLive(fast, 0) && fast->subpageWrites[0].valid);
    store(alternate, 0x20000000 | mixed);
    assert(MMU_TRANSLATE_RESULT_PA(compare(0x28, true)) == 0x10000028);
    for (auto* mmu : {exact, fast}) mmuTlbFlush(mmu);
    assert(MMU_TRANSLATE_RESULT_PA(compare(0x28, true)) == 0x20000028);
    assert(!mmuTlbLive(fast, 0));
}

int main() {
    testArm920SubpageWrites();
    auto mem=memInit(); assert(memRegionAdd(mem,0,memory.size(),access,nullptr));
    auto reference=mmuInit(mem,true), fast=mmuInit(mem,true);
    auto compare=[&](uint32_t addr,bool privileged,bool write) {
        auto expected=mmuTranslate(reference,addr,privileged,write);
        auto actual=write ? mmuTranslateCached<true>(fast,addr,privileged)
                          : mmuTranslateCached<false>(fast,addr,privileged);
        assert(actual==expected);
    };
    auto flush=[&]() { mmuTlbFlush(reference); mmuTlbFlush(fast); };
    auto ttp=[&](uint32_t v) { mmuSetTTP(reference,v); mmuSetTTP(fast,v); };
    auto domain=[&](uint32_t v) { mmuSetDomainCfg(reference,v); mmuSetDomainCfg(fast,v); };
    // Disabled translation, including high addresses and writes.
    for (auto addr:{0u,0x80000000u,0xffffffffu}) for (bool w:{false,true}) compare(addr,false,w);
    ttp(0x4000);
    for (unsigned dom=0;dom<16;++dom) for (unsigned ap=0;ap<4;++ap) for (unsigned cache=0;cache<2;++cache) {
        put(0x4000,0x10000002 | dom<<5 | ap<<10 | cache<<3);
        for (unsigned dac=0;dac<4;++dac) {
            domain(dac<<(2*dom));
            flush();
            for (bool priv:{false,true}) {
                const auto result=mmuTranslateCached<true>(fast,0x1234,priv);
                const bool allowed=dac==3 || (dac==1 && (ap==3 || (priv && ap!=0)));
                assert(bool(MMU_TRANSLATE_RESULT_OK(result))==allowed);
                if (!allowed) assert(MMU_TRANSLATE_RESULT_FSR(result)==((dac==1 ? 0x0d : 0x09)|(dom<<4)));
            }
            for (int repeat=0;repeat<2;++repeat) for (bool priv:{false,true}) for (bool w:{false,true})
                for (auto addr:{0u,0x1234u,0xffffcu}) compare(addr,priv,w);
        }
        // Domain changes must take effect immediately on populated TLB entries.
        domain(3u<<(2*dom)); compare(0x4321,false,true);
        domain(0); compare(0x4321,false,true);
    }
    domain(3);
    // Small pages with differing subpage permissions are deliberately uncached;
    // XScale extended pages and large pages exercise the populated TLB path.
    put(0x4000,0x8001);
    for (auto desc:{0x10000ffau,0x10000e4au,0x1000003bu,0x10000ff9u,0u}) {
        put(0x8000,desc); flush();
        for (int repeat=0;repeat<2;++repeat) for (bool priv:{false,true}) for (bool w:{false,true})
            for (auto addr:{0u,0x3fcu,0x400u,0x800u,0xffcu}) compare(addr,priv,w);
    }
    // Fine table tiny pages, page faults, section faults, table read faults.
    put(0x4000,0x8003); put(0x8000,0x1000003b); flush();
    compare(0,false,false); compare(0x400,true,true);
    for (auto desc:{0u,0x20001u}) { put(0x4000,desc); flush(); compare(0,true,false); }
    ttp(0x4001); compare(0,true,false);
    ttp(0x20000); compare(0,false,true);
    ttp(0x4000);
    // Generation wrap, changing translations, then disabling and re-enabling.
    for (unsigned i=0;i<40;++i) {
        put(0x4000,((i+1)<<20)|0xc0a); flush(); compare(0x120,true,false); compare(0x120,true,true);
        assert(MMU_TRANSLATE_RESULT_PA(mmuTranslateCached<false>(fast,0x120,true))==((i+1)<<20)+0x120);
    }
    ttp(MMU_DISABLED_TTP); compare(0xabc,false,true);
    ttp(0x4000); compare(0xabc,false,false);
    std::puts("PASS: native MMU cached path matches page walks, permissions, faults and invalidation");
}
