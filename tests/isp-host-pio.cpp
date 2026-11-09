#include <cassert>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "EmRegsISPDana.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"
class Isp: public EmRegsISPDana {
 public:
 Isp(){SetSubBankHandlers();Reset(true);}
 uint64 CurrentCycle() const override{return 0;}
 uint64 ClockHz() const override{return 1000000;}
 void cmd(uint16 c){SetWord(0x1f000002,c);}
 void put(uint16 v){SetWord(0x1f000000,v);}
 uint16 get(){return GetWord(0x1f000000);}
 void reg(uint16 c,uint16 v){cmd(c);put(v);}
};
static std::vector<uint8> save(Isp& i){Savestate<ChunkType> s;assert(s.Save(i));auto* p=static_cast<uint8*>(s.GetBuffer());return {p,p+s.GetSize()};}
static bool load(Isp& i,std::vector<uint8> s){SavestateLoader<ChunkType> l;return l.Load(s.data(),s.size(),i);}
int main(){
 Isp i;i.reg(0xab,4096);i.reg(0xa2,11);i.cmd(0xc1);i.put(0x1234);
 auto partial=save(i);i.put(0x5678);assert(load(i,partial));i.put(0x5678);
 for(int n=0;n<4;++n)i.put(0xffff);
 i.cmd(0x24);assert(i.get()&4);i.cmd(0x2c);assert(i.get()==4);
 i.reg(0xa2,11);i.cmd(0x41);assert(i.get()==0x1234);assert(i.get()==0x5678);
 auto reading=save(i);assert(i.get()==0xffff);assert(load(i,reading));assert(i.get()==0xffff);
 i.get();i.get();assert(i.get()==255);
 bool fail=false;auto before=save(i);try{i.get();}catch(const std::runtime_error&){fail=true;}assert(fail);assert(save(i)==before);
 auto bad=before;uint32 invalid=4097; // appended host cursor follows all DC fields
 // The cursor is immediately before 2048 Do16 words (each occupies one serialized word).
 memcpy(bad.data()+bad.size()-2048*4-4,&invalid,4);
 assert(!load(i,bad));assert(save(i)==before);
 bad=before;bad.resize(bad.size()-4);assert(!load(i,bad));assert(save(i)==before);
 // Interrupted malformed guest headers remain saveable until list validation.
 i.reg(0xa2,8);i.cmd(0xc1);i.put(0xffff);auto prefix=save(i);
 assert(load(i,prefix));i.put(0xffff);i.put(0xffff);i.put(0xffff);
 auto malformed=save(i);assert(load(i,malformed));
 i.reg(0xa2,4096);i.cmd(0xc1);for(int n=0;n<2048;++n)i.put(n);
 i.reg(0xa2,0);fail=false;try{i.cmd(0xc1);}catch(...){fail=true;}assert(fail);
 i.reg(0xa9,0xf6);i.cmd(0x22);assert(i.get()==0);
 uint8 data[24]={0,0xf8,64,0,1,4,1,0,42,0,0,0,0,0xf8,64,8,0,8,1,0};
 EmRegsISPDana::HostPtd p;assert(EmRegsISPDana::ValidateHostList(data,20));
 assert(!EmRegsISPDana::ValidateHostList(data,19));
 assert(EmRegsISPDana::DecodeHostPtd(data,20,0,p)&&p.active&&p.total==1&&p.direction==1);
 auto unchanged=p;assert(!EmRegsISPDana::DecodeHostPtd(data,7,0,p));assert(p.total==unchanged.total);
 assert(!EmRegsISPDana::CompleteHostPtd(data,20,0,2,0,true));assert(data[1]==0xf8);
 assert(EmRegsISPDana::CompleteHostPtd(data,20,0,1,0,true));assert(data[1]==4);
 data[19]=1;assert(!EmRegsISPDana::ValidateHostList(data,20));
 return 0;
}
