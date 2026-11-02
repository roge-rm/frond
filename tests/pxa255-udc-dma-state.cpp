// Pending UDC DMA through actual SoC state replacement, without firmware.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "soc_PXA.h"

static void write(SocPXA& soc,uint32_t address,uint32_t value){
    assert(cpuMemOpExternal(soc.GetCpu(),&value,address,4,true));
}
static uint32_t read(SocPXA& soc,uint32_t address){
    uint32_t value=0;assert(cpuMemOpExternal(soc.GetCpu(),&value,address,4,false));return value;
}
static std::vector<uint8_t> save(SocPXA& soc){
    assert(soc.Save());const auto b=soc.GetSavestate();
    return {static_cast<uint8_t*>(b.data),static_cast<uint8_t*>(b.data)+b.size};
}
static void run(SocPXA& soc){assert(soc.Run(100000,100000000)>=100000);}
int main(){
    std::vector<uint8_t> rom(4096);
    SocPXA soc(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    auto* ram=static_cast<uint8_t*>(soc.GetMemoryData().data);
    const uint32_t loop=0xeafffffe;std::memcpy(ram+0x3000,&loop,4);
    cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
    cpuSetReg(soc.GetCpu(),15,0xa0003000);
    for(unsigned i=0;i<256;++i)ram[0x1000+i]=uint8_t(i*53+17);
    write(soc,0x40600000,1);
    const uint8_t config[]={0,9,1,0,0,0,0,0};auto port=soc.GetUsbPort();
    assert(port.Setup(config)==Pxa270UdcHostResult::Ack);
    write(soc,0x40600010,0x81);write(soc,0x40600058,255);
    write(soc,0x40000178,0x8f);write(soc,0x4000003c,0x40000017);
    write(soc,0x400002f4,0xa0001000);write(soc,0x400002f8,0x40600600);
    write(soc,0x400002fc,0x90234100);write(soc,0x4000003c,0xc0000000);
    run(soc);assert((read(soc,0x400002fc)&0x1fff)==128 && !(read(soc,0x4000003c)&0x100));
    auto state=save(soc);const std::vector<uint8_t> memory(ram,ram+16*1024*1024);
    assert(!soc.Load(state.size()-4,state.data()));
    assert(save(soc)==state && std::memcmp(ram,memory.data(),memory.size())==0);
    // A fresh SoC adopts the saved peripheral state and its separate RAM image.
    SocPXA fresh(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    std::memcpy(fresh.GetMemoryData().data,memory.data(),memory.size());
    assert(fresh.Load(state.size(),state.data()));auto restored=fresh.GetUsbPort();
    std::vector<uint8_t> bytes;
    for(unsigned packet=0;packet<4;++packet){
        uint8_t data[64];unsigned size=0;
        assert(restored.EpIn(6,data,&size)==Pxa270UdcHostResult::Ack && size==64);
        bytes.insert(bytes.end(),data,data+size);run(fresh);
    }
    assert(bytes==std::vector<uint8_t>(memory.begin()+0x1000,memory.begin()+0x1100));
    assert(!(read(fresh,0x400002fc)&0x1fff) && !(read(fresh,0x4000003c)&0x80000000));
    puts("PASS: pending nonzero UDC/DMA actual SoC rejected-load RAM/state rollback and fresh exact 256-byte continuation");
}
