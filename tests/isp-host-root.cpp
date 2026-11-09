#include <cassert>
#include <cstring>
#include <vector>
#include "EmRegsISPDana.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
class Isp: public EmRegsISPDana {
 public:
 uint64 now=0;
 Isp(){SetSubBankHandlers();Reset(true);}
 uint64 CurrentCycle() const override{return now;}
 uint64 ClockHz() const override{return 1000000;}
 void reg(uint16 c,uint32 v){SetWord(0x1f000002,c);SetWord(0x1f000000,v);if(c<0xa0)SetWord(0x1f000000,v>>16);}
 uint32 get(uint16 c){SetWord(0x1f000002,c);uint32 v=GetWord(0x1f000000);if(c<0x20)v|=uint32(GetWord(0x1f000000))<<16;return v;}
};
static std::vector<uint8> save(Isp& i){Savestate<ChunkType>s;assert(s.Save(i));auto*p=static_cast<uint8*>(s.GetBuffer());return {p,p+s.GetSize()};}
static bool load(Isp&i,std::vector<uint8>s){SavestateLoader<ChunkType>l;return l.Load(s.data(),s.size(),i);}
int main(){
 Isp i;
 i.SetHostConnection(0,true);assert(!(i.get(0x15)&1));
 i.reg(0x94,0x10000);assert((i.get(0x15)&0x10101)==0x10101);
 assert(i.get(3)&0x40);assert(!i.HostInterruptPending());
 i.reg(0x84,0x80000040);i.reg(0xa5,0x10);assert(!i.HostInterruptPending());
 i.reg(0xa0,1);assert(i.HostInterruptPending());
 i.reg(0xa4,0x10);assert(i.HostInterruptPending());
 i.reg(0x95,0x10000);i.reg(0x83,0x40);i.reg(0xa4,0x10);assert(!i.HostInterruptPending());
 i.reg(0x95,0x10);assert(i.get(0x15)&0x10);
 i.now=9999;assert(i.get(0x15)&0x10);
 auto mid=save(i);i.now=10000;assert((i.get(0x15)&0x100012)==0x100002);
 assert(load(i,mid));assert(!(i.get(0x15)&0x217));
 i.SetHostConnection(0,true,true);i.reg(0x95,2);assert(i.get(0x15)&0x202);
 i.reg(0x95,0x10);i.SetHostConnection(0,false);i.now+=20000;
 assert(!(i.get(0x15)&0x217));assert(i.get(0x15)&0x20000);
 i.SetHostConnection(0,true);i.reg(0x95,0x10);
 auto before=save(i),bad=before;uint64 invalid=10001;memcpy(bad.data()+bad.size()-16,&invalid,8);
 assert(!load(i,bad));assert(save(i)==before);
 bad=before;bad.resize(bad.size()-4);assert(!load(i,bad));assert(save(i)==before);
 i.reg(0x94,1);assert(!(i.get(0x15)&0x317));
 i.reg(0x92,0x100);i.reg(0x93,0x20000);i.reg(0x95,0x100);assert(i.get(0x15)&0x101);
 i.reg(0x94,1);assert(i.get(0x15)&0x100);i.reg(0x95,0x200);assert(!(i.get(0x15)&0x317));
 i.reg(0xa9,0xf6);assert(!(i.get(0x15)&0x317));assert(!i.HostInterruptPending());
}
