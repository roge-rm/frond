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
 // ISR order: HC status W1C, port reads, then microprocessor OPR W1C.
 for(bool oprFirst : {false,true}) {
  Isp event;event.reg(0x92,0x20000202);event.SetHostConnection(0,true);
  event.reg(0x84,0x80000040);event.reg(0xa5,0x10);event.reg(0xa0,1);
  auto ack=[&] {
   if(oprFirst) {event.reg(0xa4,0x10);assert(event.HostInterruptPending());}
   event.reg(0x83,0x40);assert(!(event.get(3)&0x40));
   event.reg(0xa4,0x10);assert(!event.HostInterruptPending());
  };
  assert(event.HostInterruptPending());ack();
  for(int n=0;n<10;++n) {assert(event.get(0x15)==0x10101);assert(!(event.get(3)&0x40));}
  event.reg(0x95,0x10);ack();event.now=10000;
  assert(event.get(0x15)==0x110103);assert(event.HostInterruptPending());ack();
  assert(event.get(0x15)==0x110103);
  event.reg(0x85,0x80000000);event.SetHostConnection(1,true);
  assert(event.get(3)&0x40);assert(!event.HostInterruptPending());
  event.reg(0x84,0x80000000);assert(event.HostInterruptPending());ack();
  event.reg(0xa5,0);event.SetHostConnection(1,false);
  assert(event.get(3)&0x40);assert(!event.HostInterruptPending());
  event.reg(0xa5,0x10);assert(event.HostInterruptPending());ack();
  event.reg(0xa0,0);event.SetHostConnection(1,true);
  assert(event.get(3)&0x40);assert(!event.HostInterruptPending());
  event.reg(0xa0,1);assert(event.HostInterruptPending());ack();
  event.SetHostConnection(1,false);ack();
  event.reg(0x85,0x40);event.SetHostConnection(0,false);
  assert(event.get(3)&0x40);assert(!event.HostInterruptPending());
  event.reg(0x84,0x40);assert(event.HostInterruptPending());ack();
  event.SetHostConnection(0,true);assert(event.HostInterruptPending());ack();
  event.reg(0x95,0x10000);assert(event.get(0x15)&0x100000);
  assert(!(event.get(3)&0x40));
  event.SetHostConnection(1,true);assert(event.get(3)&0x40);
  event.reg(0x96,0x10000);assert(event.get(3)&0x40);ack();
  auto stable=save(event);assert(load(event,stable));
  assert(event.get(3)&0x40);ack(); // Loading detaches the live port.
  auto detached=save(event);assert(load(event,detached));
  assert(!(event.get(3)&0x40));
  for(uint32 version : {6u,7u}) {
   auto legacy=detached;legacy.resize(legacy.size()-4);
   if(version==6) legacy.resize(legacy.size()-16);
   uint32 words=(legacy.size()-12)/4;
   memcpy(legacy.data()+8,&words,4);memcpy(legacy.data()+12,&version,4);
   assert(load(event,legacy));assert(!(event.get(3)&0x40));
   event.SetHostConnection(0,true);assert(event.get(3)&0x40);ack();
  }
  event.reg(0xa9,0xf6);assert(!(event.get(3)&0x40));
 }
 // NPS overrides switching even when the target was attached without a reset.
 {
  Isp powered;powered.SetHostConnection(0,true);
  powered.reg(0x92,0x20000202);powered.reg(0x94,0x8001);
  assert((powered.get(0x15)&0x101)==0x101);
  assert((powered.get(0x16)&0x101)==0x100);
 }
 // HC startup uses always-powered ports after resetting the HC.
 for(unsigned port=0;port<2;++port) {
  Isp startup;unsigned cancels=0;
  startup.hostCancelled=[&]{++cancels;};
  startup.SetHostTarget(port,[](const auto&){return EmRegsISPDana::HostReply{};});
  startup.reg(0xa9,0xf6);
  assert(cancels && startup.HasHostTarget(port));
  assert(!(startup.get(0x15+port)&0x101));
  startup.reg(0x92,0x20000202);startup.reg(0x93,1);startup.reg(0x94,0x8001);
  startup.reg(0x95,8);startup.reg(0x96,8);
  assert((startup.get(0x15+port)&0x101)==0x101);
  assert((startup.get(0x16-port)&0x101)==0x100);
  startup.reg(0x95+port,0x200);assert(startup.get(0x15+port)&0x100);
  startup.reg(0x82,1);assert(startup.HasHostTarget(port));
  assert((startup.get(0x15+port)&0x101)==0x101);
  auto powered=save(startup);assert(load(startup,powered));
  assert(!startup.HasHostTarget(port));
  assert((startup.get(0x15+port)&0x101)==0x100);
  startup.SetHostTarget(port,[](const auto&){return EmRegsISPDana::HostReply{};});
  assert((startup.get(0x15+port)&0x101)==0x101);
  startup.SetHostTarget(port,{});assert(!(startup.get(0x15+port)&1));
  assert(startup.get(0x15+port)&0x100);
 }
 Isp i;
 // ClearSuspendStatus has no effect while the port is not suspended.
 for(auto command : {0x95,0x96}) {
  auto before=i.get(command&0x7f);i.reg(command,8);assert(i.get(command&0x7f)==before);
 }
 i.SetHostConnection(0,true);assert(!(i.get(0x15)&1));
 i.reg(0x94,0x10000);assert((i.get(0x15)&0x10101)==0x10101);
 auto active=i.get(0x15);i.reg(0x95,8);assert(i.get(0x15)==active);
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
