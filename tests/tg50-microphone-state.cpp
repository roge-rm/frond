// Firmware-free source ownership through actual SoC checkpoints/load/reset/sleep.
#include <cassert>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "i2sdev_AK4534.h"
#include "mem.h"
struct Source {
    unsigned pulls{},disarms{},fences{};
    bool fenced{};
    AudioInputSource Hooks(){return {this,[](void *p,AudioInputRoute,uint32_t,uint32_t)->int16_t{
        auto &s=*static_cast<Source*>(p);assert(!s.fenced);++s.pulls;return (s.pulls&1)?1000:-1000;
    },[](void*p,bool f){auto &s=*static_cast<Source*>(p);s.fenced=f;++s.fences;},
    [](void*p){++static_cast<Source*>(p)->disarms;}};}
};
struct Machine {
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
int main(){
    Machine m;m.configure();Source s;auto h=s.Hooks();m.soc.SetMicrophoneSource(&h);m.run(100);
    assert(s.pulls>=4000 && m.soc.MicrophoneDemand().active);
    auto saved=m.save();auto count=s.pulls;
    assert(!m.soc.Load(saved.size()-4,saved.data()));assert(s.pulls==count && s.disarms==0);
    assert(m.save()==saved && !s.fenced);
    auto bad=saved;auto *words=reinterpret_cast<uint32_t *>(bad.data());
    unsigned offset=1+2*words[0];bool changed=false;
    for(unsigned i=0;i<words[0];++i){
        if(words[1+2*i]==0xb0)words[offset+36]^=0x10203040; // Earlier controller state.
        if(words[1+2*i]==0x1d0){words[offset+37]=72;changed=true;}
        offset+=words[2+2*i];
    }
    assert(changed);auto memory=m.soc.GetMemoryData();
    std::vector<uint8_t> ram((uint8_t*)memory.data,(uint8_t*)memory.data+memory.size);
    assert(!m.soc.Load(bad.size(),bad.data()));
    assert(s.pulls==count && s.disarms==0 && !s.fenced && m.save()==saved);
    assert(!std::memcmp(memory.data,ram.data(),ram.size()));
    {
        SoC::InputCheckpoint checkpoint(m.soc);assert(s.fenced && m.soc.MicrophoneDemand().fenced);
        m.run(100);assert(s.pulls==count);
        assert(checkpoint.Restore(saved.size(),saved.data()));
    }
    assert(!s.fenced && !s.disarms && m.save()==saved);
    {
        SoC::InputCheckpoint checkpoint(m.soc);m.soc.Reset();assert(!s.disarms);
        assert(checkpoint.Restore(saved.size(),saved.data()));
    }
    assert(!s.disarms && !s.fenced);
    {
        SoC::InputCheckpoint outer(m.soc);
        { SoC::InputCheckpoint inner(m.soc);m.soc.Reset();inner.Finish(); }
        assert(!s.disarms && s.fenced && s.pulls==count);
        assert(outer.Restore(saved.size(),saved.data()));
    }
    assert(!s.disarms && !s.fenced && m.save()==saved);
    assert(!std::memcmp(memory.data,ram.data(),ram.size()));
    m.run();assert(s.pulls>count);
    saved=m.save();assert(m.soc.Load(saved.size(),saved.data()));assert(s.disarms==1);
    count=s.pulls;m.run();assert(s.pulls==count);
    m.soc.SetMicrophoneSource(&h);m.soc.Reset();assert(s.disarms==2);
    m.configure();m.soc.SetMicrophoneSource(&h);m.sleep();assert(s.disarms==3);
    m.soc.SetMicrophoneSource(&h);assert(s.disarms==4); // No arming an asleep guest.
    assert(m.soc.Load(saved.size(),saved.data()));m.soc.SetMicrophoneSource(&h);
    {
        SoC::InputCheckpoint checkpoint(m.soc);m.sleep();assert(s.disarms==4);
        assert(checkpoint.Restore(saved.size(),saved.data()));
    }
    assert(s.disarms==4 && !s.fenced && !m.soc.PowerSleeping());
    {
        SoC::InputCheckpoint checkpoint(m.soc);m.sleep();checkpoint.Finish();
    }
    assert(s.disarms==5);
    assert(m.soc.Load(saved.size(),saved.data()));m.soc.SetMicrophoneSource(&h);
    { SoC::InputCheckpoint checkpoint(m.soc);m.soc.Reset();checkpoint.Finish(); }
    assert(s.disarms==6);
}
