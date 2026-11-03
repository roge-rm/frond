// Memory Stick Pro card and controller: the TG50 driver's TPC and command sequences, the attribute
// area it parses, STOP and errors, and save/restore of controller chunk v2 (v1 still loads).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "MemoryStickImage.h"
#include "MsProImage.h"
#include "mmiodev_MemoryStickController.h"
#include "savestate/savestateAll.h"
#include "Tg50Chunks.h"
#include "pxa_DMA.h"
static ArmMemAccessF mmio;static void* context;
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF fn,void* c){mmio=fn;context=c;return true;}
void pxaGpioSetState(PxaGpio*,uint_fast8_t,bool){}
void pxaDmaExternalReq(PxaDma*,uint_fast8_t,bool){}
void pxaDmaSetPeripheral(PxaDma*,uint_fast8_t,const PxaDmaPeripheral*){}
extern "C" void uarmAbort(){std::abort();}
static uint32_t rd(unsigned offset,unsigned size=2){uint32_t n=0;assert(mmio(context,0x14000000+offset,size,false,&n));return n;}
static void wr(unsigned offset,uint32_t n,unsigned size=2){assert(mmio(context,0x14000000+offset,size,true,&n));}
struct State {
 MemoryStickController* m;
 template<typename T>void Save(T& t){msCtrlrSave(m,t);}
 void Load(SavestateLoader<ChunkType>& t){msCtrlrLoad(m,t);}
};
static std::vector<uint32_t> save(State& target){Savestate<ChunkType> s;assert(s.Save(target));auto* p=static_cast<uint32_t*>(s.GetBuffer());return {p,p+s.GetSize()/4};}
static bool load(State& target,std::vector<uint32_t> words){SavestateLoader<ChunkType> l;return l.Load(words.data(),words.size()*4,target);}

// Sparse sectors; unwritten ones read as zero.
struct Disk {
 std::map<uint32_t,std::array<uint8_t,512>> sectors;
 bool fail{};
 MsProStore Store(unsigned mib){
  MsProStore s;s.sectors=frondMsPro::sectorsOf(mib);s.attribute=frondMsPro::attribute(mib,"pro-test");
  s.read=[this](uint32_t n,uint8_t* out){if(fail)return false;auto it=sectors.find(n);if(it==sectors.end())std::memset(out,0,512);else std::memcpy(out,it->second.data(),512);return true;};
  s.write=[this](uint32_t n,const uint8_t* in){if(fail)return false;std::memcpy(sectors[n].data(),in,512);return true;};
  return s;
 }
};

static MemoryStickController* m;
static void tpc(uint8_t id,const std::vector<uint8_t>& bytes){
 wr(0xb0,0x100);wr(0x80,(uint16_t(id)<<12)|bytes.size());
 for(size_t i=0;i<((bytes.size()+7)&~size_t(7));i+=4){uint32_t word=0;for(unsigned b=0;b<4&&i+b<bytes.size();++b)word|=uint32_t(bytes[i+b])<<(8*b);wr(0x90,word,4);if((i&7)==4)msCtrlrPeriodic(m);}
}
static std::vector<uint8_t> rtpc(uint8_t id,size_t size){
 wr(0xb0,0);wr(0x80,(uint16_t(id)<<12)|(size&0x3ff));
 std::vector<uint8_t> out;
 for(size_t i=0;i<((size+7)&~size_t(7));i+=4){const auto word=rd(0x90,4);for(unsigned b=0;b<4;++b)out.push_back(uint8_t(word>>(8*b)));if((i&7)==4)msCtrlrPeriodic(m);}
 out.resize(size);return out;
}
static uint8_t intr(){return rtpc(7,1)[0];}
static void exCmd(uint8_t cmd,uint16_t count,uint32_t address){
 tpc(9,{cmd,uint8_t(count>>8),uint8_t(count),uint8_t(address>>24),uint8_t(address>>16),uint8_t(address>>8),uint8_t(address)});
}
static std::vector<uint8_t> sector(uint8_t v){return std::vector<uint8_t>(512,v);}

// What the Sony driver checks in the attribute area (3452Slot 2.10 Pro init).
static void AttributeArea(){
 for(unsigned mib:frondMsPro::cardMibs){
  const auto a=frondMsPro::attribute(mib,"id");assert(a.size()%512==0);
  assert(a[0]==0xa5&&a[1]==0xc3&&a[2]==1&&a[4]>=1&&a[4]<=12);
  uint32_t info=0,mbr=0,pbr=0;
  for(unsigned i=0;i<a[4];++i){
   const auto* e=a.data()+0x10+12*i;const uint32_t at=MemoryStickProCard::Be32(e),size=MemoryStickProCard::Be32(e+4);
   assert(at>=0x1a0&&at+size<=0x8000&&at+size<=a.size());
   if(e[8]==0x10){assert(size==0x60);info=at;}
   if(e[8]==0x20){assert(size==0x10);mbr=at;}
   if(e[8]==0x21){assert(size==0x40);pbr=at;}
  }
  assert(info&&mbr&&pbr&&a[info]==2&&!(a[info+0x2e]&0xc0));
  auto le32=[&](uint32_t at){return uint32_t(a[at])|(uint32_t(a[at+1])<<8)|(uint32_t(a[at+2])<<16)|(uint32_t(a[at+3])<<24);};
  // Capacity is the partition's start plus its length.
  assert(le32(mbr+8)+le32(mbr+12)==frondMsPro::sectorsOf(mib));
  assert(a[pbr+11]==0&&a[pbr+12]==2&&a[pbr+13]>=8);
  const auto layout=frondSd::layout(mib);
  assert(std::memcmp(a.data()+pbr,layout[1].bytes.data(),0x40)==0);
  // user blocks x block size is the whole card
  assert(uint32_t((a[info+6]<<8)|a[info+7])*uint32_t((a[info+2]<<8)|a[info+3])==frondMsPro::sectorsOf(mib));
 }
}

// FORMAT with a factory format: MBR to root directory rewritten, data sectors left alone.
static void FactoryFormat(){
 for(unsigned mib:frondMsPro::cardMibs){
  Disk disk;auto store=disk.Store(mib);const auto layout=frondSd::layout(mib);const uint32_t last=store.sectors-1;
  const auto write=store.write;store.format=[mib,write]{return frondMsPro::format(mib,write);};
  disk.sectors[100].fill(0x55);disk.sectors[last].fill(0x66);
  MemoryStickProCard card;card.Attach(store,true);
  assert(card.Command(MemoryStickProCard::Format)&&card.Interrupt()==0xc0&&disk.sectors.size()==2);
  card.Attach(store,false);
  assert(card.Command(MemoryStickProCard::Format)&&card.Interrupt()==0x80);
  for(const auto& s:layout)assert(disk.sectors.at(s.index)==s.bytes);
  assert((disk.sectors.at(100)==std::array<uint8_t,512>{})&&disk.sectors.at(last)[0]==0x66);
  assert(disk.sectors.size()==layout.back().index+32+1);
  disk.fail=true;assert(card.Command(MemoryStickProCard::Format)&&card.Interrupt()==0xc0);
 }
}

int main(){
 AttributeArea();
 FactoryFormat();
 Disk disk;
 m=msCtrlrInit(nullptr,0x14000000);State state{m};
 assert(!msCtrlrPro(m));
 assert(msCtrlrAttachPro(m,disk.Store(256)));
 assert(msCtrlrPresent(m)&&msCtrlrPro(m)&&msCtrlrInserted(m)&&!msCtrlrReadOnly(m)&&msCtrlrMegabytes(m)==256);
 std::vector<uint8_t> image;assert(!msCtrlrExport(m,image));
 // Identification: type 1, category 0, class 0.
 tpc(8,{0,8,0x10,0x10});
 auto regs=rtpc(4,8);assert(regs[4]==1&&regs[6]==0&&regs[7]==0&&!(regs[2]&1));
 // READ_ATRB, one sector, then CED.
 exCmd(0x24,1,0);assert(intr()==0x20);
 auto atrb=rtpc(2,512);assert(std::equal(atrb.begin(),atrb.end(),frondMsPro::attribute(256,"pro-test").begin()));
 assert(intr()==0x80);
 // WRITE_DATA of three sectors, a BREQ before each, CED after the last.
 for(uint8_t i=0;i<3;++i){assert(intr()==(i?0x20:0x80));if(!i){exCmd(0x21,3,100);assert(intr()==0x20);}tpc(0xd,sector(0x10+i));}
 assert(intr()==0x80);
 for(uint8_t i=0;i<3;++i)assert(disk.sectors.at(100+i)[0]==0x10+i);
 // READ_DATA back through SET_CMD with the count and address in the parameter registers.
 tpc(8,{0,8,0x11,6});tpc(0xb,{0,3,0,0,0,100});tpc(0xe,{0x20});
 for(uint8_t i=0;i<3;++i){assert(intr()==0x20);assert(rtpc(2,512)==sector(0x10+i));}
 assert(intr()==0x80);
 tpc(8,{0,8,0x10,0x10});
 // Count 0 runs until STOP; the sector already requested still moves.
 exCmd(0x20,0,100);assert(intr()==0x20&&rtpc(2,512)==sector(0x10));
 assert(intr()==0x20);tpc(0xe,{0x25});assert(intr()==0x20);
 assert(rtpc(2,512)==sector(0x11));assert(intr()==0x80);
 exCmd(0x21,0,200);assert(intr()==0x20);tpc(0xd,sector(0x55));tpc(0xe,{0x25});assert(intr()==0x20);tpc(0xd,sector(0x56));
 assert(intr()==0x80&&disk.sectors.at(200)[0]==0x55&&disk.sectors.at(201)[0]==0x56&&!disk.sectors.count(202));
 // STOP with nothing running is just CED.
 tpc(0xe,{0x25});assert(intr()==0x80);
 // Errors: past the end, a write-protected write, an unknown command, a failing store.
 const uint32_t last=frondMsPro::sectorsOf(256)-1;
 exCmd(0x20,2,last);assert(intr()==0xc0);
 exCmd(0x20,1,last);assert(intr()==0x20&&rtpc(2,512)==sector(0));assert(intr()==0x80);
 exCmd(0x24,1,2);assert(intr()==0xc0);
 exCmd(0x99,0,0);assert(intr()==0x81);
 disk.fail=true;exCmd(0x20,1,5);assert(intr()==0xc0);disk.fail=false;
 // Data TPCs out of turn are refused at the transport.
 tpc(0xd,sector(1));assert(rd(0xa0)&0x100);
 assert(msCtrlrInsert(m,false)&&msCtrlrProtect(m,true)&&msCtrlrInsert(m,true)&&msCtrlrReadOnly(m));
 regs=rtpc(4,8);assert(regs[2]&1);
 exCmd(0x21,1,7);assert(intr()==0xc0&&!disk.sectors.count(7));
 exCmd(0x10,0,0);assert(intr()==0xc0);
 assert(msCtrlrInsert(m,false)&&msCtrlrProtect(m,false)&&msCtrlrInsert(m,true));
 exCmd(0x10,0,0);assert(intr()==0x80);

 // Save in the middle of a read; a fresh controller on the same stick continues it.
 exCmd(0x20,3,100);assert(intr()==0x20);assert(rtpc(2,512)==sector(0x10));
 const auto mid=save(state);
 assert(intr()==0x20&&rtpc(2,512)==sector(0x11)&&intr()==0x20&&rtpc(2,512)==sector(0x12)&&intr()==0x80);
 const auto done=save(state);
 {
  auto* again=msCtrlrInit(nullptr,0x14000000);State againState{again};
  assert(msCtrlrAttachPro(again,disk.Store(256),false));
  assert(load(againState,mid)&&save(againState)==mid);
  // The MMIO callback goes to `context`, the last controller made.
  auto* first=m;m=again;
  assert(intr()==0x20&&rtpc(2,512)==sector(0x11)&&intr()==0x20&&rtpc(2,512)==sector(0x12)&&intr()==0x80);
  assert(save(againState)==done);
  msCtrlrDestroy(again);m=first;
 }
 // A Pro state needs the same Pro stick attached: none, a classic one or another size refuse.
 for(int variant=0;variant<3;++variant){
  auto* other=msCtrlrInit(nullptr,0x14000000);State otherState{other};
  if(variant==1)assert(msCtrlrAttach(other,frondMs::create()));
  if(variant==2)assert(msCtrlrAttachPro(other,disk.Store(512),false));
  const auto before=save(otherState);
  assert(!load(otherState,mid)&&save(otherState)==before);
  msCtrlrDestroy(other);
 }
 // A classic stick replaces the Pro and saves at v2; that state with its Pro words dropped and
 // version 1 is a v1 chunk, which still loads.
 {
  auto* classic=msCtrlrInit(nullptr,0x14000000);State classicState{classic};
  assert(msCtrlrAttachPro(classic,disk.Store(256),false));
  assert(msCtrlrAttach(classic,frondMs::create())&&!msCtrlrPro(classic)&&msCtrlrMegabytes(classic)==128);
  auto v2=save(classicState);
  size_t offset=1+v2[0]*2;
  for(unsigned i=0;i<v2[0];++i){
   const auto words=v2[2+i*2];
   if(v2[1+i*2]==uint32_t(ChunkType::sonyTg50MsCard)){
    assert(v2[offset]==2);
    auto v1=v2;const size_t pro=3+(32+512)/4+9;
    v1[offset]=1;v1.erase(v1.begin()+offset+words-pro,v1.begin()+offset+words);v1[2+i*2]-=pro;
    auto* fresh=msCtrlrInit(nullptr,0x14000000);State freshState{fresh};
    assert(msCtrlrAttach(fresh,frondMs::create()));
    assert(load(freshState,v1)&&save(freshState)==v2);
    msCtrlrDestroy(fresh);
    break;
   }
   offset+=words;
  }
  msCtrlrDestroy(classic);
 }
 msCtrlrDestroy(m);
 // A load that leaves the slot empty releases the card store behind the Pro stick, once.
 {
  auto* empty=msCtrlrInit(nullptr,0x14000000);State emptyState{empty};
  const auto none=save(emptyState);
  auto* held=msCtrlrInit(nullptr,0x14000000);State heldState{held};
  int released=0;auto store=disk.Store(256);store.release=[&released]{++released;};
  assert(msCtrlrAttachPro(held,std::move(store),false));
  assert(load(heldState,none)&&released==1&&!msCtrlrPresent(held));
  assert(load(heldState,none)&&released==1);
  msCtrlrDestroy(held);msCtrlrDestroy(empty);
 }
 // Detach leaves the slot empty.
 m=msCtrlrInit(nullptr,0x14000000);assert(msCtrlrAttachPro(m,disk.Store(1886)));assert(msCtrlrMegabytes(m)==1886);
 msCtrlrDetach(m);assert(!msCtrlrPresent(m)&&!msCtrlrInserted(m));
 msCtrlrDestroy(m);
 std::puts("Memory Stick Pro card/controller checks passed");
}
