#include <cassert>
#include <vector>
#include <cstring>
#include "EmRegsISPDana.h"
#include "EmHAL.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
class Isp: public EmRegsISPDana {
public:
 uint64 now=0; Isp(){SetSubBankHandlers();Reset(true);}
 uint64 CurrentCycle() const override{return now;}
 uint64 ClockHz() const override{return 24000000;}
 void reg(uint16 c,uint32 v){SetWord(0x1f000002,c);SetWord(0x1f000000,v);if(c<0xa0)SetWord(0x1f000000,v>>16);}
 uint32 get(uint16 c){SetWord(0x1f000002,c);uint32 v=GetWord(0x1f000000);if(c<0x20)v|=uint32(GetWord(0x1f000000))<<16;return v;}
};
static std::vector<uint8> save(Isp&i){Savestate<ChunkType>s;assert(s.Save(i));auto*p=(uint8*)s.GetBuffer();return {p,p+s.GetSize()};}
static bool load(Isp&i,std::vector<uint8>s){SavestateLoader<ChunkType>l;return l.Load(s.data(),s.size(),i);}
int main(){
 Isp periodic;periodic.Initialize();periodic.reg(0x81,0x80);
 bool irq=false;periodic.SetHostIrq([&](bool level){irq=!level;});
 periodic.reg(0x84,0x80000004);periodic.reg(0xa5,0x10);periodic.reg(0xa0,1);
 periodic.reg(0x83,4);periodic.reg(0xa4,0x10);assert(!irq);
 periodic.now=24000;EmHAL::DispatchCycle(24000,false);assert(irq);
 periodic.Dispose();irq=false;periodic.now+=24000;EmHAL::DispatchCycle(48000,false);assert(!irq);
 Isp zero;zero.reg(0x8d,5);zero.reg(0x81,0x80);zero.now=10;assert(zero.get(0xe)==0);
 zero.reg(0x8d,0x80000005);zero.now=12;assert(zero.get(0xe)==5);
 zero.now=22;assert(zero.get(0xe)==0x80000000);
 Isp legacySource;legacySource.reg(0x81,0x80);auto legacy=save(legacySource);
 // Version 8 ends before hostWritten (added in version 10): drop its word from the chunk.
 legacy.resize(legacy.size()-4);uint32 words=0;std::memcpy(&words,legacy.data()+8,4);--words;std::memcpy(legacy.data()+8,&words,4);
 uint32 version=8;std::memcpy(legacy.data()+12,&version,4);
 uint64 oldFraction=12000000;std::memcpy(legacy.data()+24,&oldFraction,8);
 Isp migrated;assert(load(migrated,legacy));assert(migrated.get(0xe)==5999);
 migrated.now=2;assert(migrated.get(0xe)==5998);
 auto unchanged=save(migrated);uint64 invalidFraction=24000000;
 std::memcpy(legacy.data()+24,&invalidFraction,8);assert(!load(migrated,legacy));assert(save(migrated)==unchanged);
 Isp i;assert(i.get(0x0e)==0);i.reg(0x81,0x80);assert(i.get(0xf)==1);assert(i.get(0xe)==11999);
 i.now=1;assert(i.get(0xe)==11999);auto partial=save(i);
 Isp fresh;fresh.now=1;assert(load(fresh,partial));fresh.now=2;assert(fresh.get(0xe)==11998);
 i.now=2;assert(i.get(0xe)==11998);i.reg(0x8d,0x80000005);assert(i.get(0xe)==11998);
 i.now=23998;assert(i.get(0xe)==0x80000000);assert(i.get(0xf)==1);
 i.now=24000;assert(i.get(0xe)==0x80000005);assert(i.get(0xf)==2);
 i.SetWord(0x1f000002,0xe);assert(i.GetWord(0x1f000000)==5);auto split=save(i);
 i.reg(0x8d,5);i.now+=12;assert(i.get(0xe)==5);
 Isp other;other.now=24000;assert(load(other,split));other.now+=12;assert(other.GetWord(0x1f000000)==0x8000);
 assert(other.get(0xe)==0x80000005);
 auto good=save(other);auto bad=good;bad.resize(bad.size()-1);assert(!load(other,bad));assert(save(other)==good);
 i.reg(0x81,0xc0);auto frozen=i.get(0xe);i.now+=1000;assert(i.get(0xe)==frozen);
 i.reg(0x81,0x80);assert(i.get(0xe)==5);i.reg(0x8d,0);i.now+=12;assert(i.get(0xe)==0);auto n=i.get(0xf);i.now+=2;assert(i.get(0xf)==((n+1)&0xffff));
 i.now+=2*65536;assert(i.get(0xf)==((n+1)&0xffff));assert(i.get(3)&0x20);
 i.reg(0x82,1);assert(i.get(0xe)==0);assert(i.get(0xf)==0);i.Reset(true);assert(i.get(0xe)==0);
 // A frame boundary the CPU ran through without sleeping doesn't hold back the wake for the next one.
 {struct Root: EmHALHandler { uint32 CyclesToNextInterrupt(uint64) override { return 1000000; } } root;
  Isp w;w.Initialize();w.reg(0x81,0x80);w.reg(0xa5,1);w.reg(0xa0,1);
  w.now=100;w.get(0x24);w.now=30000;w.get(0x24);
  assert(EmHAL::CyclesToNextInterrupt(30000)==18000);assert(EmHAL::CyclesToNextInterrupt(48000)==1000000);
  w.Dispose();}
}
