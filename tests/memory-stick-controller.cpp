#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "MemoryStickCard.h"
#include "MemoryStickImage.h"
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
static uint32_t* chunk(std::vector<uint32_t>& s,ChunkType type){auto* p=s.data()+1+s[0]*2;for(unsigned i=0;i<s[0];++i){if(s[1+i*2]==uint32_t(type))return p;p+=s[2+i*2];}std::abort();}
int main(){
 auto image=frondMs::create();assert(frondMs::validBoot(image));
 {auto encoded=frondMs::encode(image);std::vector<uint8_t> decoded;assert(frondMs::decode(encoded.data(),encoded.size(),decoded));assert(decoded==image);encoded.back()^=1;assert(!frondMs::decode(encoded.data(),encoded.size(),decoded));assert(decoded==image);}
 MemoryStickCard card;assert(card.media.Attach(image,false)==MemoryStickMedia::Result::ok);card.Reset();
 const uint8_t idWindow[]={4,4,6,1};assert(card.TpcWrite(8,idWindow,4));assert(MemoryStickCard::Valid(card.state));
 uint8_t id[4]{};assert(card.TpcRead(4,id,4));assert(id[0]==0xff&&id[2]==0xff&&id[3]==0xff);
 const uint8_t category=0;assert(card.TpcWrite(0xb,&category,1));assert(card.TpcRead(4,id,4));assert(id[2]==0xff);
 card.Reset();
 for(auto value:{32u,0xfffffff0u,0xffffffffu})for(auto size:{16u,32u,0xffffffffu}){
  auto s=card.state;s.writeStart=value;s.writeSize=size;assert(!MemoryStickCard::Valid(s));
  s=card.state;s.readStart=value;s.readSize=size;assert(!MemoryStickCard::Valid(s));
 }
 card.state.reg[19]=2;card.state.reg[20]=0x40;card.state.reg[21]=0;
 std::fill_n(card.state.reg.begin()+22,9,0xf5);assert(card.Command(0x55));assert(card.Interrupt()==0x80);
 MemoryStickMedia::Page before;assert(card.media.Read(2,0,before)==MemoryStickMedia::Result::ok);
 card.state.reg[20]=0x80;std::fill_n(card.state.reg.begin()+22,9,0);card.state.reg[22]=0x70;assert(card.Command(0x55));
 MemoryStickMedia::Page after;card.media.Read(2,0,after);assert(after[512]==(before[512]&0x70));
 for(unsigned i=513;i<521;++i)assert(after[i]==before[i]);
 card.state.reg[20]=0xc0;assert(card.Command(0x55));assert(card.Interrupt()==0xc0);
 // The card data buffer survives a host read and a parameter change for internal page copy.
 card.state.reg[19]=2;card.state.reg[20]=0x20;card.state.reg[21]=0;assert(card.Command(0xaa));
 std::array<uint8_t,512> readPage{};assert(card.TpcRead(2,readPage.data(),readPage.size()));card.FinishRead(2);
 assert((card.state.bufferFlags&MemoryStickCard::Cached)&&!(card.state.bufferFlags&MemoryStickCard::WriteReady));
 assert(card.Command(0xaa));
 card.state.reg[19]=3;assert(card.Command(0x55));MemoryStickMedia::Page copied;assert(card.media.Read(3,0,copied)==MemoryStickMedia::Result::ok);
 assert(copied==after);
 // BLOCK_END preserves the current page while ending a multiple-page stream.
 card.state.reg[19]=2;card.state.reg[20]=0;card.state.reg[21]=0;assert(card.Command(0xaa));assert(card.state.operation==2);
 assert(card.Command(0x33));assert(card.state.operation==1);assert(card.TpcRead(2,readPage.data(),512));card.FinishRead(2);assert(card.state.operation==0);
 auto* m=msCtrlrInit(nullptr,0x14000000);assert(msCtrlrAttach(m,image));State state{m};
 // A partial register-window TPC must resume after a portable reload.
 wr(0xb0,0x100);wr(0x80,0x8004);wr(0x90,0x0f101e00,4);auto saved=save(state);
 wr(0x90,0,4);assert(rd(0xa0)&0x1000);
 SavestateLoader<ChunkType> loader;assert(loader.Load(saved.data(),saved.size()*4,state));wr(0x90,0,4);assert(rd(0xa0)&0x1000);
 auto canonical=save(state);
 for(unsigned field:{(1+7+256+8+128), (1+7+256+8+128+2)}){
  auto invalid=canonical;auto* data=chunk(invalid,ChunkType::sonyTg50MsCard);data[field]=0xfffffff0u;if(field==402)data[403]=16;
  assert(!loader.Load(invalid.data(),invalid.size()*4,state));assert(save(state)==canonical);
 }
 assert(msCtrlrInsert(m,false));assert(msCtrlrProtect(m,true));auto protectedState=save(state);
 assert(msCtrlrProtect(m,false));assert(loader.Load(protectedState.data(),protectedState.size()*4,state));assert(msCtrlrReadOnly(m));
 assert(msCtrlrProtect(m,false));assert(msCtrlrInsert(m,true));
 auto tpc=[&](uint8_t id,const std::vector<uint8_t>& bytes){
  wr(0xb0,0x100);wr(0x80,(uint16_t(id)<<12)|bytes.size());
  for(size_t i=0;i<((bytes.size()+7)&~size_t(7));i+=4){uint32_t word=0;for(unsigned b=0;b<4&&i+b<bytes.size();++b)word|=uint32_t(bytes[i+b])<<(8*b);wr(0x90,word,4);if((i&7)==4)msCtrlrPeriodic(m);}
 };
 tpc(8,{0,30,16,15});std::vector<uint8_t> params(15,0xff);params[0]=0x80;params[1]=params[2]=0;params[3]=2;params[4]=0x20;params[5]=0;
 tpc(0xb,params);tpc(0xe,{0x55});wr(0xb0,0x100);wr(0x80,0xd200);
 for(unsigned i=0;i<64;++i){wr(0x90,0xa5b6c7d8,4);if(i&1)msCtrlrPeriodic(m);}
 auto partialWrite=save(state); // Preserve the pending page and half-filled host FIFO.
 for(unsigned i=0;i<64;++i){wr(0x90,0xa5b6c7d8,4);if(i&1)msCtrlrPeriodic(m);}
 auto committed=save(state);std::vector<uint8_t> physical;assert(msCtrlrExport(m,physical));
 const auto index=size_t(2)*32*521;assert(physical[index]==0xd8&&physical[index+511]==0xa5);
 // Restoring the decoded image keeps the pending write and its extra bytes intact.
 assert(loader.Load(partialWrite.data(),partialWrite.size()*4,state));
 for(unsigned i=0;i<64;++i){wr(0x90,0xa5b6c7d8,4);if(i&1)msCtrlrPeriodic(m);}
 std::vector<uint8_t> continued;assert(msCtrlrExport(m,continued));assert(continued==physical);
 assert(save(state)==committed);
 auto* missing=msCtrlrInit(nullptr,0x14000000);State missingState{missing};
 assert(!loader.Load(committed.data(),committed.size()*4,missingState));
 msCtrlrDestroy(missing);context=m;
 auto* fresh=msCtrlrInit(nullptr,0x14000000);assert(msCtrlrAttach(fresh,physical));State freshState{fresh};
 assert(loader.Load(committed.data(),committed.size()*4,freshState));
 std::vector<uint8_t> restored;assert(msCtrlrExport(fresh,restored));assert(restored==physical);msCtrlrDestroy(fresh);context=m;
 {auto invalid=committed;size_t offset=1+invalid[0]*2;
  for(unsigned i=0;i<invalid[0];++i){auto words=invalid[2+i*2];if(invalid[1+i*2]==uint32_t(ChunkType::sonyTg50MsCard)){
    if(words){invalid[2+i*2]-=1;invalid.erase(invalid.begin()+offset+words-1);}break;}offset+=words;}
  assert(!loader.Load(invalid.data(),invalid.size()*4,state));assert(save(state)==committed);
 }
 // Guest flash damage is physical data, not a malformed portable state.
 params[3]=0;tpc(0xb,params);tpc(0xe,{0x99});params[3]=1;tpc(0xb,params);tpc(0xe,{0x99});
 std::vector<uint8_t> damagedImage;assert(msCtrlrExport(m,damagedImage));assert(!frondMs::validBoot(damagedImage));auto damaged=save(state);
 assert(loader.Load(damaged.data(),damaged.size()*4,state));std::vector<uint8_t> damagedRestored;assert(msCtrlrExport(m,damagedRestored));assert(damagedRestored==damagedImage);
 msCtrlrDestroy(m);std::puts("Memory Stick card/controller checks passed");
}
