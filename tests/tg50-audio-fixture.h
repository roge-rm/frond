#ifndef FROND_TG50_AUDIO_FIXTURE_H
#define FROND_TG50_AUDIO_FIXTURE_H
#include <cassert>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "i2sdev_AK4534.h"
#include "mem.h"
struct AudioMachine {
    std::vector<uint8_t> rom=std::vector<uint8_t>(4096);
    SocPXA soc{deviceTypeTG50,16*1024*1024,rom.data(),uint32_t(rom.size()),nullptr,0,-1,0};
    void wr(uint32_t a,uint32_t v){assert(memAccess(cpuGetMem(soc.GetCpu()),a,4,true,&v));}
    void loop(){wr(0xa0003000,0xeafffffe);cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(soc.GetCpu(),15,0xa0003000);}
    void run(unsigned ms=10){soc.Run(ms*100000,100000000);}
    void reg(unsigned address,unsigned value){
        wr(0x40301688,0x20);wr(0x40301690,0x49);
        wr(0x40301688,address);wr(0x40301690,0x48);
        wr(0x40301688,value);wr(0x40301690,0x4a);
    }
    void configure(){loop();wr(0x41300004,0x100);reg(0,0x83);reg(1,0x60);reg(4,0x6a);reg(7,0x0c);wr(0x40400000,0x7701);}
    std::vector<uint8_t> save(){assert(soc.Save());auto d=soc.GetSavestate();return {(uint8_t*)d.data,(uint8_t*)d.data+d.size};}
    void sleep(){wr(0xa0003000,0xee070e10);cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),15,0xa0003000);cpuSetReg(soc.GetCpu(),0,3);cpuCyclePure(soc.GetCpu());assert(soc.PowerSleeping());}
};
#endif
