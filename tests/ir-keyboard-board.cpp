// Exercise the production accessory wiring and savestate through each board's
// actual infrared UART MMIO. No firmware, serial endpoint or host timing is involved.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "pxa_UART.h"
#include "soc_PXA.h"

static uint32_t uartBase;
static void mmio(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(cpuMemOpExternal(soc.GetCpu(), &value, address, 4, true));
}
static void power(SocPXA& soc, bool enabled) {
    mmio(soc,0x08000020,1u<<10);
    mmio(soc,enabled?0x08000040:0x08000060,1u<<10);
    mmio(soc,0x40e0000c,1u<<27);
    mmio(soc,0x40e00024,1u<<27);
}
static void write(SocPXA& soc, unsigned reg, uint32_t value) {
    assert(cpuMemOpExternal(soc.GetCpu(), &value, uartBase+4*reg, 4, true));
}
static uint32_t read(SocPXA& soc, unsigned reg) {
    uint32_t value=0;
    assert(cpuMemOpExternal(soc.GetCpu(), &value, uartBase+4*reg, 4, false));
    return value;
}
static void run(SocPXA& soc, unsigned ms=1) {
    assert(soc.Run(uint64_t(ms)*100000,100000000)>=uint64_t(ms)*100000);
}
static std::vector<uint8_t> receive(SocPXA& soc, unsigned bytes) {
    std::vector<uint8_t> result;
    for(unsigned ms=0; ms<100 && result.size()<bytes; ++ms) {
        run(soc);
        while(result.size()<bytes) {
            const auto status=read(soc,5);
            assert(!(status&2)); // Check overrun before the status read clears it.
            if(!(status&1)) break;
            result.push_back(read(soc,0));
        }
    }
    assert(result.size()==bytes);
    return result;
}
static std::vector<uint8_t> saved(SocPXA& soc) {
    assert(soc.Save());const auto state=soc.GetSavestate();
    return {static_cast<uint8_t*>(state.data),static_cast<uint8_t*>(state.data)+state.size};
}
static std::vector<uint32_t> scheduler(const std::vector<uint8_t>& state) {
    const auto* words=reinterpret_cast<const uint32_t*>(state.data());
    size_t offset=1+2*words[0];
    for(unsigned i=0;i<words[0];++i) {
        const auto length=words[2+2*i];
        if(words[1+2*i]==uint32_t(ChunkType::scheduler)) return {words+offset,words+offset+length};
        offset+=length;
    }
    assert(false);return {};
}
int main() {
    for(auto device : {deviceTypeTX,deviceTypeE2,deviceTypeTG50}) {
        const bool tx=device==deviceTypeTX,tg=device==deviceTypeTG50;
        uartBase=tg?PXA_FFUART_BASE:PXA_STUART_BASE;
        std::vector<uint8_t> nor(tg?4096:(tx?16:8)*1024*1024),nand(tg?0:tx?132*1024*1024:34603008,0xff);
        SocPXA soc(device,(tg?16:32)*1024*1024,nor.data(),nor.size(),nand.data(),nand.size(),-1,tx?2:0);
        const uint32_t loop=0xeafffffe;
        std::memcpy(static_cast<uint8_t*>(soc.GetMemoryData().data)+0x3000,&loop,4);
        cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
        cpuSetReg(soc.GetCpu(),15,0xa0003000);
        assert(!soc.KeyboardAttached() && !soc.KeyboardKey(0x1a,true));
        soc.AttachKeyboard(true);assert(soc.KeyboardAttached());
        assert(!soc.KeyboardKey(0x1a,true)); // Receiver still disabled.
        write(soc,3,tg?0x83:0x87);write(soc,0,96);write(soc,1,0); // Original TG50 driver receives 8N1; accessory sends 8N2.
        write(soc,3,tg?3:7);write(soc,4,8);write(soc,1,0x41);write(soc,2,7);
        assert(!soc.KeyboardKey(0x1a,true)); // UART on, IR receive still disabled.
        write(soc,8,3);
        if(tg) {
            assert(!soc.KeyboardKey(0x1a,true));
            power(soc,true);
            mmio(soc,0x40e00018,1u<<27);
            assert(!soc.KeyboardKey(0x1a,true));
            mmio(soc,0x40e00024,1u<<27);
        }
        int owner;
        IrHostHooks hooks{&owner,[](void*)->uint_fast16_t{return UART_CHAR_NONE;},
            [](uint_fast16_t,void*){},[](void*){return false;},[](void*){},nullptr};
        assert(!soc.SetIrHostHooks(&hooks));
        if(tg) {
            soc.KeyDown(tg50Key(2,1));run(soc,50);mmio(soc,0x08000028,4);
            uint32_t matrix=0;
            assert(cpuMemOpExternal(soc.GetCpu(),&matrix,0x08000008,4,false));
            assert(!(matrix&0x100));soc.KeyUp(tg50Key(2,1));run(soc,50);
        }
        assert(soc.KeyboardKey(0x1a,true));
        if(tg) {
            power(soc,false);run(soc,20);assert(!(read(soc,5)&1));
            power(soc,true);mmio(soc,0x40e00018,1u<<27);
            run(soc,20);assert(!(read(soc,5)&1));
            mmio(soc,0x40e00024,1u<<27);

        }
        const auto first=receive(soc,2);
        assert((first==std::vector<uint8_t>{0xff,0xc0}));
        if(tg) {
            uint32_t status=0;
            assert(cpuMemOpExternal(soc.GetCpu(),&status,PXA_STUART_BASE+20,4,false));
            assert(!(status&1));
        }
        auto partial=saved(soc);
        const std::vector<uint8_t> tail{0x1a,0xe5,0x7d,0xc1};
        auto malformed=partial;
        auto* words=reinterpret_cast<uint32_t*>(malformed.data());
        size_t offset=1+2*words[0];bool found=false;
        for(unsigned i=0;i<words[0];++i) {
            if(words[1+2*i]==uint32_t(ChunkType::irKeyboard)) {
                words[offset+2]=IrKeyboard::capacity;found=true;break;
            }
            offset+=words[2+2*i];
        }
        if(tg) { power(soc,false);run(soc,20); }
        const auto beforeRefusal=scheduler(saved(soc));
        assert(found && !soc.Load(malformed.size(),malformed.data()));
        if(tg) {
            assert(scheduler(saved(soc))==beforeRefusal);
            power(soc,true);
        }
        assert(receive(soc,4)==tail);
        if(tg) { power(soc,false);run(soc,20); }
        assert(soc.Load(partial.size(),partial.data()));
        if(tg) {
            const auto after=scheduler(saved(soc)),before=scheduler(partial);
            // Normal load rebuilds PCM scheduling; the UART task's phase stays exact.
            assert(std::equal(before.begin()+41,before.begin()+48,after.begin()+41));
        }
        assert(soc.KeyboardAttached() && receive(soc,4)==tail);
        if(tg) {
            SocPXA fresh(deviceTypeTG50,16*1024*1024,nor.data(),nor.size(),nullptr,0,-1,0);
            assert(fresh.Load(partial.size(),partial.data()));
            assert(fresh.KeyboardAttached() && receive(fresh,4)==tail);
            fresh.ReleaseKeyboard();
            assert((receive(fresh,6)==std::vector<uint8_t>{0xff,0xc0,0x9a,0x65,0xfd,0xc1}));
        }
        // Focus loss/session startup use this public release API. Restored held
        // state must produce a real break packet, not inject a Palm key event.
        soc.ReleaseKeyboard();
        const std::vector<uint8_t> up{0xff,0xc0,0x9a,0x65,0xfd,0xc1};
        assert(receive(soc,6)==up);
        assert(soc.KeyboardKey(0x1a,true));
        assert((receive(soc,6)==std::vector<uint8_t>{0xff,0xc0,0x1a,0xe5,0x7d,0xc1}));
        soc.AttachKeyboard(false);
        assert(!soc.KeyboardAttached() && !soc.KeyboardKey(0x1a,true));
        assert(!soc.SetIrHostHooks(&hooks));
        assert(receive(soc,6)==up); // Detachment releases an accepted held key.
        assert(soc.SetIrHostHooks(&hooks));soc.AttachKeyboard(true);assert(!soc.KeyboardAttached());
        assert(soc.SetIrHostHooks(nullptr));
        auto detached=saved(soc);soc.AttachKeyboard(true);
        assert(soc.Load(detached.size(),detached.data()) && !soc.KeyboardAttached());
        run(soc,20);assert(!(read(soc,5)&1));
        printf("PASS: %s infrared UART gates, exact bytes, partial/fresh restore, malformed rollback, peer exclusion, release and detach persistence\n",tg?"TG50 FFUART":tx?"TX STUART":"E2 STUART");
    }
}
