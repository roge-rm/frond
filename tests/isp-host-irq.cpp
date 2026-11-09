#include <cassert>
#include <cstring>
#include <vector>
#include <stdexcept>
#include "EmRegsISPDana.h"
#include "EmRegsVZDana.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
struct Board:EmRegsVZDana {
 Board(){SetSubBankHandlers();StdWrite(0xfffff304,2,0xffff);StdWrite(0xfffff302,2,0);SetByte(0xfffff41b,0);}
};
struct Isp:EmRegsISPDana {
 Isp(){SetSubBankHandlers();Reset(true);}
 uint64 CurrentCycle()const override{return 0;}
 uint64 ClockHz()const override{return 1000000;}
 void reg(uint16 c,uint32 v){SetWord(0x1f000002,c);SetWord(0x1f000000,v);if(c<0xa0)SetWord(0x1f000000,v>>16);}
};
static std::vector<uint8> save(Isp&i){Savestate<ChunkType>s;assert(s.Save(i));auto*p=(uint8*)s.GetBuffer();return {p,p+s.GetSize()};}
static bool load(Isp&i,std::vector<uint8>s){SavestateLoader<ChunkType>l;return l.Load(s.data(),s.size(),i);}
int main(){
 Board b;Isp i;bool high=false;
 i.SetHostIrq([&](bool h){high=h;b.SetHcIrqPin(h);});assert(high);
 auto pending=[&](){return b.GetWord(0xfffff310)&4;};
 auto status=[&](){return b.GetWord(0xfffff30c)&4;};
 i.SetHostConnection(0,true);i.reg(0x94,0x10000);i.reg(0x84,0x80000040);i.reg(0xa5,0x10);
 assert(!pending());i.reg(0xa0,0x1009);assert(!high&&pending()&&!status());
 b.SetWord(0xfffff304,0xfffb);assert(status());b.SetWord(0xfffff30c,4);assert(status());
 b.SetByte(0xfffff41b,0x40);assert(!pending());b.SetByte(0xfffff41b,0);assert(pending());
 b.SetWord(0xfffff302,0x2000);assert(!pending());i.reg(0xa0,0x100d);assert(high&&pending());
 b.SetWord(0xfffff302,0);i.reg(0xa0,0x1009);assert(pending());
 i.reg(0xa5,0);assert(high&&!pending());i.reg(0xa5,0x10);assert(pending());
 b.SetByte(0xfffff300,0x40);assert(b.GetInterruptBase()==0x40);assert(b.GetInterruptLevel()==3);
 bool edge=false;try{b.SetWord(0xfffff302,0x200);}catch(const std::runtime_error&){edge=true;}assert(edge&&b.GetWord(0xfffff302)==0);
 auto before=save(i),bad=before;bad.resize(bad.size()-4);assert(!load(i,bad));assert(save(i)==before&&pending());
 bad=before;uint64 invalid=10001;memcpy(bad.data()+bad.size()-16,&invalid,8);assert(!load(i,bad));assert(save(i)==before&&pending());
 bad=before;unsigned matches=0;
 for(size_t n=16;n+4<=bad.size();n+=4){uint32 v;memcpy(&v,bad.data()+n,4);if(v==0x1009){v|=2;memcpy(bad.data()+n,&v,4);++matches;}}
 assert(matches==1);assert(!load(i,bad));assert(save(i)==before&&pending());
 i.Reset(false);assert(pending());
 i.reg(0x95,0x10000);i.reg(0x83,0x40);i.reg(0xa4,0x10);assert(high&&!pending());
 assert(load(i,before));assert(pending());
 i.Reset(true);assert(high&&!pending());
 i.reg(0xa0,1);i.SetHostConnection(0,true);i.reg(0x94,0x10000);i.reg(0x84,0x80000040);i.reg(0xa5,0x10);assert(pending());
 i.reg(0xa9,0xf6);assert(high&&!pending());
 bool refused=false;try{i.reg(0xa0,3);}catch(const std::runtime_error&){refused=true;}assert(refused);
 b.SetDcIrqPin(false);assert(b.GetWord(0xfffff310)&2);assert(!pending());b.SetHcIrqPin(false);assert((b.GetWord(0xfffff310)&6)==6);
 b.SetHcIrqPin(true);assert((b.GetWord(0xfffff310)&6)==2);
 i.SetHostIrq({});
}
