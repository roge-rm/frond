#include <cassert>
#include <vector>
#include "EmRegsISPDana.h"
#include "EmHAL.h"
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
 void ready(HostTarget t){SetHostTarget(0,t);reg(0x94,0x10000);reg(0x95,2);reg(0x81,0x80);reg(0xab,4096);}
 void submit(const std::vector<uint8>& b){reg(0xa2,b.size());SetWord(0x1f000002,0xc1);for(unsigned n=0;n<b.size();n+=2)SetWord(0x1f000000,b[n]|(n+1<b.size()?b[n+1]<<8:0));}
 void frame(){now+=1000;get(0x0f);}
 std::vector<uint8> read(unsigned size){reg(0xa2,size);SetWord(0x1f000002,0x41);std::vector<uint8>b;for(unsigned n=0;n<size;n+=2){auto w=GetWord(0x1f000000);b.push_back(w);if(n+1<size)b.push_back(w>>8);}return b;}
};
using R=EmRegsISPDana::PacketResult;
static std::vector<uint8> ptd(unsigned total=10,unsigned direction=1){std::vector<uint8>b(8+total);b[1]=8;b[2]=4;b[3]=8;b[4]=total;b[5]=direction<<2;for(unsigned n=0;n<total;++n)b[8+n]=n;return b;}
static std::vector<uint8> save(Isp&i){Savestate<ChunkType>s;assert(s.Save(i));auto*p=static_cast<uint8*>(s.GetBuffer());return {p,p+s.GetSize()};}
static bool load(Isp&i,std::vector<uint8>b){SavestateLoader<ChunkType>l;return l.Load(b.data(),b.size(),i);}
int main(){
 Isp i;unsigned calls=0,accepted=0;std::vector<bool>toggles;std::vector<unsigned>sizes;
 i.ready([&](const auto&p){++calls;accepted+=p.bytes.size();toggles.push_back(p.toggle);sizes.push_back(p.requested);return Isp::HostReply{R::ack,p.toggle,{}};});
 auto b=ptd();i.submit(b);auto checkpoint=save(i);i.Initialize();i.now+=1000;EmHAL::DispatchCycle(i.now,false);i.Dispose();assert(calls==3&&accepted==10);assert((sizes==std::vector<unsigned>{4,4,2}));assert((toggles==std::vector<bool>{false,true,false}));assert(i.get(0x2c)==36);assert(i.get(0x24)&2);auto out=i.read(b.size());assert(out[0]==10&&out[1]==4);i.frame();assert(calls==3);
 assert(load(i,checkpoint));i.SetHostTarget(0,[&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{}};});i.reg(0x94,0x10000);i.reg(0x95,2);i.frame();assert(calls==3);
 for(auto result:{R::nak,R::stall,R::noAnswer}){
 calls=0;i.ready([&](const auto&){++calls;return Isp::HostReply{result,false,{}};});i.submit(b);i.frame();out=i.read(b.size());assert(out[0]==0);if(result==R::nak){assert(calls==1&&out[1]==8);}else assert(calls==(result==R::stall?1:3)&&out[1]==(result==R::stall?0x40:0x50));
 }
 calls=0;i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{42,43}};});b=ptd(10,2);i.submit(b);i.frame();out=i.read(b.size());assert(calls==1&&out[0]==2&&out[1]==0x94&&out[8]==42&&out[9]==43);
 for(bool oversized:{false,true}){calls=0;i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,!p.toggle,std::vector<uint8>(oversized?5:1)};});i.submit(b);i.frame();out=i.read(b.size());assert(out[0]==0&&out[1]==(oversized?0x80:0x30)&&calls==(oversized?1:3));}
 // Reject the last malformed header before the first OUT can escape.
 calls=0;i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{}};});b=ptd(4);b[3]=0;auto tail=ptd(0);tail[7]=1;b.insert(b.end(),tail.begin(),tail.end());i.submit(b);bool threw=false;try{i.frame();}catch(...){threw=true;}assert(threw&&calls==0);i.reg(0xa9,0xf6);
 // Chained descriptors, inactive entries and per-frame interrupt transaction cap.
 i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{}};});b=ptd(4);b[3]=0;tail=ptd(10);tail[5]|=32;b.insert(b.end(),tail.begin(),tail.end());i.submit(b);i.frame();out=i.read(b.size());assert(out[0]==4&&out[12]==4&&(out[13]&8));
 // Work budget retains actual/toggle for explicit guest resubmission.
 b=ptd(255);b[2]=1;i.submit(b);calls=0;i.frame();out=i.read(b.size());assert(calls==32&&out[0]==32&&(out[1]&8));i.submit(out);i.frame();out=i.read(b.size());assert(out[0]==64);
 // A partial ACK followed by NAK retains the accepted prefix.
 calls=0;i.ready([&](const auto&p){++calls;return Isp::HostReply{calls==1?R::ack:R::nak,p.toggle,{}};});b=ptd();i.submit(b);i.frame();out=i.read(b.size());assert(calls==2&&out[0]==4&&out[1]==12);
 // The whole-list budget also bounds many short chained transfers.
 calls=0;i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{}};});b.clear();
 for(unsigned n=0;n<150;++n){auto z=ptd(0);if(n!=149)z[3]=0;b.insert(b.end(),z.begin(),z.end());}
 i.submit(b);i.frame();out=i.read(b.size());assert(calls==128&&out[128*8+1]==8);
 b=ptd(255);b[2]=1;i.submit(b);i.frame();out=i.read(b.size());assert(out[0]==32);
 // Detach and reset never undo previously accepted bytes.
 i.submit(out);i.SetHostConnection(0,false);calls=0;i.frame();assert(calls==0);out=i.read(b.size());assert(out[0]==32&&out[1]==0x50);
 i.submit(b);i.Reset(true);i.frame();assert(calls==0);
 // Invalid load preserves target and armed submission.
 i.ready([&](const auto&p){++calls;return Isp::HostReply{R::ack,p.toggle,{}};});b=ptd(4);i.submit(b);auto good=save(i),bad=good;bad.resize(bad.size()-4);assert(!load(i,bad));assert(save(i)==good);i.frame();assert(calls==1);
}
