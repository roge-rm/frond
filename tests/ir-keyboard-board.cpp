// Exercise the production accessory wiring and savestate through each board's
// actual STUART MMIO. No firmware, serial endpoint or host timing is involved.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "pxa_UART.h"
#include "soc_PXA.h"

static void write(SocPXA& soc, unsigned reg, uint32_t value) {
    assert(cpuMemOpExternal(soc.GetCpu(), &value, PXA_STUART_BASE+4*reg, 4, true));
}
static uint32_t read(SocPXA& soc, unsigned reg) {
    uint32_t value=0;
    assert(cpuMemOpExternal(soc.GetCpu(), &value, PXA_STUART_BASE+4*reg, 4, false));
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
int main() {
    for(auto device : {deviceTypeTX,deviceTypeE2}) {
        const bool tx=device==deviceTypeTX;
        std::vector<uint8_t> nor((tx?16:8)*1024*1024),nand(tx?132*1024*1024:34603008,0xff);
        SocPXA soc(device,32*1024*1024,nor.data(),nor.size(),nand.data(),nand.size(),-1,tx?2:0);
        const uint32_t loop=0xeafffffe;
        std::memcpy(static_cast<uint8_t*>(soc.GetMemoryData().data)+0x3000,&loop,4);
        cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
        cpuSetReg(soc.GetCpu(),15,0xa0003000);
        assert(!soc.KeyboardAttached() && !soc.KeyboardKey(0x1a,true));
        soc.AttachKeyboard(true);assert(soc.KeyboardAttached());
        assert(!soc.KeyboardKey(0x1a,true)); // Receiver still disabled.
        write(soc,3,0x87);write(soc,0,96);write(soc,1,0); // 9600 baud 8N2.
        write(soc,3,7);write(soc,4,8);write(soc,1,0x41);write(soc,2,7);
        assert(!soc.KeyboardKey(0x1a,true)); // UART on, IR receive still disabled.
        write(soc,8,3);
        assert(soc.KeyboardKey(0x1a,true));
        const auto first=receive(soc,2);
        assert((first==std::vector<uint8_t>{0xff,0xc0}));
        auto partial=saved(soc);
        const std::vector<uint8_t> tail{0x1a,0xe5,0x7d,0xc1};
        assert(receive(soc,4)==tail);
        assert(soc.Load(partial.size(),partial.data()));
        assert(soc.KeyboardAttached() && receive(soc,4)==tail);
        // Focus loss/session startup use this public release API. Restored held
        // state must produce a real break packet, not inject a Palm key event.
        soc.ReleaseKeyboard();
        const std::vector<uint8_t> up{0xff,0xc0,0x9a,0x65,0xfd,0xc1};
        assert(receive(soc,6)==up);
        assert(soc.KeyboardKey(0x1a,true));
        assert((receive(soc,6)==std::vector<uint8_t>{0xff,0xc0,0x1a,0xe5,0x7d,0xc1}));
        soc.AttachKeyboard(false);
        assert(!soc.KeyboardAttached() && !soc.KeyboardKey(0x1a,true));
        assert(receive(soc,6)==up); // Detachment releases an accepted held key.
        auto detached=saved(soc);soc.AttachKeyboard(true);
        assert(soc.Load(detached.size(),detached.data()) && !soc.KeyboardAttached());
        run(soc,20);assert(!(read(soc,5)&1));
        printf("PASS: %s STUART keyboard gates, exact bytes, partial restore, release and detach persistence\n",tx?"TX":"E2");
    }
}
