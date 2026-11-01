#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "mem.h"
#include "soc_PXA.h"

static uint32_t load(ArmMem* m, uint32_t a) { uint32_t v{}; assert(memAccess(m,a,4,false,&v));return v; }
static void store(ArmMem* m, uint32_t a,uint32_t v) {assert(memAccess(m,a,4,true,&v));}
constexpr uint32_t gpio=0x40e00000,cs=1u<<4,data=1u<<5,clk=1u<<12;
static void pin(ArmMem* m,uint32_t mask,bool high){store(m,gpio+(high?0x18:0x24),mask);}
static void edge(ArmMem* m,bool bit){pin(m,clk,false);pin(m,data,bit);pin(m,clk,true);}
static void bits(ArmMem* m,uint32_t value,unsigned count){for(unsigned n=count;n;--n)edge(m,(value>>(n-1))&1);}
static void begin(ArmMem* m,unsigned op,unsigned address){
    pin(m,cs|clk,false);store(m,gpio+0xc,cs|clk|data);pin(m,cs,true);bits(m,0x100|(op<<6)|address,9);
}
static uint16_t word(ArmMem* m){
    store(m,gpio+0xc,cs|clk);uint16_t value=0;
    for(unsigned n=0;n<16;++n){pin(m,clk,false);pin(m,clk,true);value=uint16_t((value<<1)|bool(load(m,gpio)&data));}
    return value;
}
static std::vector<uint8_t> save(SocPXA& soc){assert(soc.Save());auto s=soc.GetSavestate();return {static_cast<uint8_t*>(s.data),static_cast<uint8_t*>(s.data)+s.size};}
static size_t serialChunk(std::vector<uint8_t>& s){
    auto* w=reinterpret_cast<uint32_t*>(s.data());size_t offset=1+2*w[0];
    for(unsigned i=0;i<w[0];++i){if(w[1+2*i]==0x230)return offset;offset+=w[2+2*i];}
    assert(false);return 0;
}
static std::vector<uint8_t> legacy(std::vector<uint8_t> s){
    auto* w=reinterpret_cast<uint32_t*>(s.data());std::vector<uint32_t> toc,payload;size_t offset=1+2*w[0];
    for(unsigned i=0;i<w[0];++i){const auto size=w[2+2*i];if(w[1+2*i]!=0x230){toc.push_back(w[1+2*i]);toc.push_back(size);payload.insert(payload.end(),w+offset,w+offset+size);}offset+=size;}
    std::vector<uint32_t> result{uint32_t(toc.size()/2)};result.insert(result.end(),toc.begin(),toc.end());result.insert(result.end(),payload.begin(),payload.end());
    std::vector<uint8_t> bytes(result.size()*4);memcpy(bytes.data(),result.data(),bytes.size());return bytes;
}
int main(){
    std::vector<uint8_t> rom(0xc70000);const uint32_t branch=0xeafffffe;memcpy(rom.data(),&branch,4);
    SocPXA soc(deviceTypeTG50,16u<<20,rom.data(),rom.size(),nullptr,0,-1,0);auto* m=cpuGetMem(soc.GetCpu());
    store(m,0xa0000000,branch);cpuSetReg(soc.GetCpu(),15,0xa0000000);
    auto advance=[&]{soc.Run(4000000,20000000);};
    // Original bit order and shared-line turn-around, including dummy zero.
    begin(m,2,63);store(m,gpio+0xc,cs|clk);assert(!(load(m,gpio)&data));assert(word(m)==0xffff&&word(m)==0xffff);
    // Alternate-function clock pins cannot clock the serial device.
    begin(m,2,7);store(m,gpio+0xc,cs|clk);store(m,gpio+0x54,1u<<24);
    pin(m,clk,false);pin(m,clk,true);assert(!(load(m,gpio)&data));
    store(m,gpio+0x54,0);assert(load(m,gpio)&data);
    // A processor output latch still drives the line while GPDR5 is output.
    store(m,gpio+0xc,cs|clk|data);pin(m,data,false);assert(!(load(m,gpio)&data));store(m,gpio+0xc,cs|clk);assert(load(m,gpio)&data);
    // A partially received command resumes through the actual GPIO registers.
    pin(m,cs|clk,false);store(m,gpio+0xc,cs|clk|data);pin(m,cs,true);bits(m,0x187>>4,5);
    auto commandState=save(soc);bits(m,0,4);
    assert(soc.Load(commandState.size(),commandState.data()));bits(m,0x187,4);assert(word(m)==0xffff);
    begin(m,0,48);begin(m,1,7);bits(m,0x1234,16);pin(m,cs,false);
    auto busy=save(soc);advance();begin(m,2,7);assert(word(m)==0x1234);
    assert(soc.Load(busy.size(),busy.data()));advance();begin(m,2,7);assert(word(m)==0x1234);
    begin(m,1,63);bits(m,0x69,8);auto partial=save(soc);bits(m,0,8);pin(m,cs,false);advance();
    assert(soc.Load(partial.size(),partial.data()));bits(m,0xa5,8);pin(m,cs,false);advance();begin(m,2,63);assert(word(m)==0x69a5);
    // Read position and output level continue exactly in a fresh model.
    begin(m,2,63);store(m,gpio+0xc,cs|clk);pin(m,clk,false);pin(m,clk,true);auto reading=save(soc);
    SocPXA fresh(deviceTypeTG50,16u<<20,rom.data(),rom.size(),nullptr,0,-1,0);assert(fresh.Load(reading.size(),reading.data()));auto* fm=cpuGetMem(fresh.GetCpu());
    uint16_t a=load(m,gpio)&data?1:0,b=load(fm,gpio)&data?1:0;
    for(unsigned n=1;n<16;++n){pin(m,clk,false);pin(m,clk,true);pin(fm,clk,false);pin(fm,clk,true);a=uint16_t((a<<1)|bool(load(m,gpio)&data));b=uint16_t((b<<1)|bool(load(fm,gpio)&data));}assert(a==0x69a5&&a==b);
    // Invalid scalar and version states leave all live models unchanged.
    auto valid=save(soc);for(unsigned field:{0u,33u,36u,38u,41u,44u,49u}){
        auto invalid=valid;auto* w=reinterpret_cast<uint32_t*>(invalid.data());w[serialChunk(invalid)+field]=0xffffffff;
        assert(!soc.Load(invalid.size(),invalid.data()));assert(save(soc)==valid);
    }
    auto wrongPins=valid;reinterpret_cast<uint32_t*>(wrongPins.data())[serialChunk(wrongPins)+33]^=1;
    assert(!soc.Load(wrongPins.size(),wrongPins.data()));assert(save(soc)==valid);
    auto extended=valid;
    auto* ew=reinterpret_cast<uint32_t*>(extended.data());size_t end=0;
    for(unsigned i=0;i<ew[0];++i)if(ew[1+2*i]==0x230){end=serialChunk(extended)+ew[2+2*i];++ew[2+2*i];break;}
    extended.insert(extended.begin()+end*4,4,0);
    assert(!soc.Load(extended.size(),extended.data()));assert(save(soc)==valid);
    auto old=legacy(valid);assert(fresh.Load(old.size(),old.data()));begin(fm,2,7);assert(word(fm)==0xffff);
    // CPU reset retains EEPROM storage and releases incomplete serial input.
    begin(m,1,8);bits(m,0x2468,16);pin(m,cs,false);soc.Reset();advance();begin(m,2,8);assert(word(m)==0x2468);
    soc.HardReset();begin(m,2,7);assert(word(m)==0x1234);begin(m,2,63);assert(word(m)==0x69a5);
    std::puts("TG50 EEPROM GPIO, state continuation, reset retention and malformed rollback passed");
}
