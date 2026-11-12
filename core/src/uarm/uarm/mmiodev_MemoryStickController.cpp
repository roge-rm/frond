//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "mmiodev_MemoryStickController.h"
#include <string.h>
#include <stdlib.h>
#include "cputil.h"
#include "Tg50Chunks.h"
#include "Tg50Trace.h"
#include "savestate/savestateAll.h"


#define MS_CONTROLLER_SIZE 	0x400


struct LegacyMsRegisters {

	uint16_t systemReg;

	//not sure
	uint16_t reg_0, reg_40, reg_50, reg_2c0, reg_2d0, reg_2e0, reg_2f0;

	//some sort of flags. w1c i think
	uint16_t reg_188, reg_180;

	//seems to be command results (64 bits in 2 32-bit regs)
	uint32_t reply_lo_2f0, reply_hi_2f8;

};


#include "MemoryStickCard.h"
#include "MemoryStickImage.h"
#include "MemoryStickProCard.h"
#include "sdcard.h"
#include "pxa_GPIO.h"
#include "pxa_DMA.h"
#include <cstdio>
#include <memory>
#include <type_traits>

struct MemoryStickController : LegacyMsRegisters {
    struct State {
        uint32_t inserted{}, command{}, length{}, offset{}, status{}, control{}, dmaCount{}, config{}, irqPending{}, cardPending{}, credit{}, fifoPending{};
        std::array<uint8_t,1024> buffer{};
    } state;
    MemoryStickCard card;
    // A Memory Stick Pro, when one is attached instead of a classic stick.
    MemoryStickProCard pro;
    PxaGpio* gpio{};
    PxaDma* dma{};
    Reschedule reschedule{};
    std::vector<uint8_t> savedImage;
    uint32_t imageCrc{};
    FILE* trace{};
    uint32_t traceLines{},lastStatus{~0u};
    bool swapLoad{};  // the next load keeps the stick attached now, ejected
    // After such a load the detect line stays as saved until msCtrlrSwapEject, so the eject is an
    // edge outside the power manager's restore and can wake a sleeping guest.
    bool swapEject{};
};
struct MemoryStickMediaStateAccess {
    static MemoryStickMedia::Result Attach(MemoryStickMedia& media,
                                           std::vector<uint8_t>&& image, bool protect) {
        return media.Attach(std::move(image), protect);
    }
};

static bool msPresent(const MemoryStickController* m){return m->pro.Present()||m->card.media.Present();}
static uint8_t msInt(const MemoryStickController* m){return m->pro.Present()?m->pro.Interrupt():m->card.Interrupt();}
static void msCardReset(MemoryStickController* m){m->card.Reset();m->pro.Reset();}

void msCtrlrDiscardSaveImage(MemoryStickController* m) {
    if (m) std::vector<uint8_t>().swap(m->savedImage);
}

static void msTrace(MemoryStickController* m,const char* what,uint32_t a,uint32_t v) {
    if(m->trace&&!std::getenv("FROND_MS_SUMMARY")&&m->traceLines++<200000){std::fprintf(m->trace,"%s %04x %08x\n",what,a,v);std::fflush(m->trace);}
}
void msCtrlrRefresh(MemoryStickController* m) {
    const bool inserted=m->state.inserted&&msPresent(m);
    // Ordinary TPC/card interrupts are separately latched and enabled.
    const bool irq=inserted&&((m->state.irqPending&1&&m->state.control&0x400)||(m->state.irqPending&2&&m->state.control&0x2000));
    if(m->gpio){if(!m->swapEject)pxaGpioSetState(m->gpio,7,!inserted);pxaGpioSetState(m->gpio,10,!irq);}
    if(m->dma){
        const bool active=inserted&&(m->state.dmaCount&0x8000)&&m->state.offset<m->state.length&&m->state.credit;
        pxaDmaExternalReq(m->dma,DMA_CMR_DREQ_0,active);
    }
}
static void msCardInt(MemoryStickController* m) {
    const auto i=msInt(m);
    m->state.status=(m->state.status&~0x200f)|0x2000|(i&1)|((i&0xe0)>>4);
}
static void msComplete(MemoryStickController* m,bool ok,bool cardEvent) {
    m->state.status|=0x1000|(ok?0:0x100);m->state.irqPending|=1;
    m->state.cardPending=cardEvent;
    if(cardEvent&&m->reschedule.rescheduleCb)m->reschedule.rescheduleCb(m->reschedule.ctx,RESCHEDULE_TASK_DEVICE_TIER0);
    msCtrlrRefresh(m);
}
bool msCtrlrTaskRequired(MemoryStickController* m){return m->state.cardPending||m->state.fifoPending||(m->state.inserted&&(m->state.dmaCount&0x8000));}
void msCtrlrPeriodic(MemoryStickController* m){
    if(m->state.fifoPending){m->state.fifoPending=0;m->state.credit=8;msCtrlrRefresh(m);}
    if(m->state.cardPending&&!m->state.irqPending){
        m->state.cardPending=0;msCardInt(m);m->state.irqPending=2;
    }
    msCtrlrRefresh(m);
}
static void msStart(MemoryStickController* m,uint16_t command) {
    msCtrlrDiscardSaveImage(m);
    auto& s=m->state;s.command=command;s.length=command&0x3ff;if(!s.length)s.length=1024;
    s.offset=0;s.status=0;s.irqPending=0;s.cardPending=0;s.credit=8;s.fifoPending=0;s.buffer.fill(0);
    if(!s.inserted||!msPresent(m)){s.status=0x1100;msCtrlrRefresh(m);return;}
    const uint8_t tpc=command>>12;
    if(!(s.control&0x100)) {
        if(!(m->pro.Present()?m->pro.TpcRead(tpc,s.buffer.data(),s.length):m->card.TpcRead(tpc,s.buffer.data(),s.length))){
            s.offset=(s.length+7)&~7u;s.credit=0;msComplete(m,false,false);
            if(m->trace&&std::getenv("FROND_MS_SUMMARY"))std::fprintf(m->trace,"FAIL TPC=%x len=%u op=%u buffer=%u block=%u page=%u\n",tpc,s.length,m->card.state.operation,m->card.state.bufferFlags,m->card.Block(),m->card.state.reg[21]);
        }
    }
    msCtrlrRefresh(m);
}
static bool msOrdinary(MemoryStickController* m,uint32_t pa,unsigned size,bool write,void* buf) {
    auto& s=m->state;
    if(pa==0x90&&(size==1||size==2||size==4)) {
        uint32_t value=0;if(write)std::memcpy(&value,buf,size);
        if(size>s.credit){s.status|=0x100;if(!write)std::memset(buf,0,size);return true;}
        if(s.offset<((s.length+7)&~7u)) {
            s.credit-=size;
            for(unsigned i=0;i<size;++i){
                if(s.offset<s.length){if(write)s.buffer[s.offset]=value>>(8*i);else value|=uint32_t(s.buffer[s.offset])<<(8*i);}
                ++s.offset;
            }
            if(s.offset>=((s.length+7)&~7u)) {
                bool ok=true;const auto tpc=s.command>>12;
                const auto beforeOp=m->card.state.operation,beforeBuffer=m->card.state.bufferFlags;
                const auto cacheCrc=frondMs::crc32(m->card.state.page.data(),512);
                const bool cardEvent=m->pro.Present()?MemoryStickProCard::Event(tpc):tpc==0xe||tpc==2||(tpc==0xd&&(m->card.state.operation==3||m->card.state.operation==4));
                if(write){msCtrlrDiscardSaveImage(m);ok=m->pro.Present()?m->pro.TpcWrite(tpc,s.buffer.data(),s.length):m->card.TpcWrite(tpc,s.buffer.data(),s.length);}
                else if(m->pro.Present())m->pro.FinishRead(tpc);
                else m->card.FinishRead(tpc);
                msComplete(m,ok,cardEvent);
                if(m->trace&&std::getenv("FROND_MS_SUMMARY")&&m->traceLines++<200000){
                    std::fprintf(m->trace,"TPC=%x len=%u arg=%02x block=%u page=%u mode=%02x extra=%02x%02x%02x%02x op=%u->%u buffer=%u->%u cache=%08x data=%08x int=%02x ok=%u\n",unsigned(tpc),s.length,s.buffer[0],m->card.Block(),m->card.state.reg[21],m->card.state.reg[20],m->card.state.reg[22],m->card.state.reg[23],m->card.state.reg[24],m->card.state.reg[25],beforeOp,m->card.state.operation,beforeBuffer,m->card.state.bufferFlags,cacheCrc,frondMs::crc32(s.buffer.data(),std::min(s.length,512u)),msInt(m),ok);std::fflush(m->trace);
                }
            }
            if(!s.credit&&s.offset<((s.length+7)&~7u)){
                s.fifoPending=1;if(m->reschedule.rescheduleCb)m->reschedule.rescheduleCb(m->reschedule.ctx,RESCHEDULE_TASK_DEVICE_TIER0);
            }
            if(s.dmaCount&0x8000){
                const auto left=((s.length+7)&~7u)-std::min(s.offset,((s.length+7)&~7u));
                s.dmaCount=left?(0x8000|(((left+7)/8)&0x1ff)):0;
            }
        }
        if(!write)std::memcpy(buf,&value,size);
        msTrace(m,write?"W":"R",pa,value);msCtrlrRefresh(m);return true;
    }
    if(size!=2)return false;
    uint16_t value=0;if(write)std::memcpy(&value,buf,2);
    switch(pa) {
     case 0x20:if(write)s.config=value;else value=s.config;break;
     case 0x10:if(write)return false;value=s.irqPending?1:0;break;
     case 0x48:
        if(write){s.dmaCount=value;if((value&0x8000)&&m->reschedule.rescheduleCb)m->reschedule.rescheduleCb(m->reschedule.ctx,RESCHEDULE_TASK_DEVICE_TIER0);}
        else value=s.dmaCount;
        break;
     case 0x80:if(write)msStart(m,value);else value=s.command;break;
     case 0xa0:
        if(write)return false;
        value=s.status|((s.control&0x100)?(!s.credit?0x10:0):((!s.credit||s.offset>=s.length)?0x20:0));break;
     case 0xb0:
        if(write){
            if(value&0x8000){s.command=s.length=s.offset=s.status=s.irqPending=s.cardPending=s.dmaCount=s.credit=s.fifoPending=0;}
            if(value&0x200){s.offset=s.length=s.credit=s.fifoPending=0;}
            // The original driver's context supplies acknowledgement bit0x0800.
            if(value&0x800)s.irqPending=0;
            s.control=value&~0x8a00u;
        }else value=s.control;
        break;
     default:return false;
    }
    if(!write)std::memcpy(buf,&value,2);
    if(write||pa!=0xa0||m->lastStatus!=value){msTrace(m,write?"W":"R",pa,value);if(pa==0xa0)m->lastStatus=value;}
    msCtrlrRefresh(m);return true;
}

static bool msCtrlrPrvMemAccessF(void* userData, uint32_t pa, uint_fast8_t size, bool write, void* buf)
{
	struct MemoryStickController *msc = (struct MemoryStickController*)userData;
	uint32_t val = 0;
	bool ret = true;

	pa %= MS_CONTROLLER_SIZE;
    if(pa==0x10||pa==0x20||pa==0x48||pa==0x80||pa==0x90||pa==0xa0||pa==0xb0) return msOrdinary(msc,pa,size,write,buf);

	if (size == 4 && !write && (pa == 0x2f0 || pa == 0x2f8)) {

		*(uint32_t*)buf = (pa  == 0x2f0) ? msc->reply_lo_2f0 : msc->reply_hi_2f8;
		TG50_TRACE( "MSC %c [%04lx] == 0x%08lx\n", (char)(write ? 'W' : 'R'), (unsigned long)pa, (unsigned long)*(uint32_t*)buf);
		return true;
	}

	if (size != 2) {
		TG50_TRACE( "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__, write ? "write" : "read", size, (unsigned long)pa);
		return false;
	}

	if (write)
		val = *(uint16_t*)buf;

	switch (pa) {

		case 0x00:
			if (write)
				msc->reg_0 = val;
			else
				val = msc->reg_0;
			break;

		case 0x08:	//system reg top 16 bits?
			if (write)
				msc->systemReg = (msc->systemReg & 0x8a00) | (val & 0x75ff);
				//XXX: writing 0x0800 clears XINT too
			else
				val = msc->systemReg;
			break;

		case 0x40:
			if (write)
				msc->reg_40 = val;
			else
				val = msc->reg_40;
			break;

		case 0x50:
			if (write)
				msc->reg_50 = val;
			else
				val = msc->reg_50;
			break;

		case 0x180:
			if (write)
				msc->reg_180 &=~ val;
			else
				val = msc->reg_180;
			break;

		case 0x188:
			if (write)
				msc->reg_188 &=~ val;
			else
				val = msc->reg_188;
			break;

		case 0x2c0:
			if (write)
				msc->reg_2c0 = val;
			else
				val = msc->reg_2c0;
			break;

		case 0x2d0:
			if (write)
				msc->reg_2d0 = val &~ 1;
			else
				val = msc->reg_2d0;
			break;

		case 0x2e0:	//some sort of command, assembled from 2 bytes in 3452MGLib@7f0
			if (write)
				msc->reg_2e0 = val;
			else
				val = msc->reg_2e0;
			break;

		case 0x2f0:
			if (write)
				msc->reg_2f0 = val;
			else
				val = msc->reg_2f0;
			break;

		default:
			TG50_TRACE( "unknown reg 0x%08lx\n", (unsigned long)pa);
			ret = false;
			break;
	}

	if (ret)
		TG50_TRACE( "MSC %c [%04lx] == 0x%04lx\n", (char)(write ? 'W' : 'R'), (unsigned long)pa, (unsigned long)val);
	else
		TG50_TRACE( "MSC %c [%04lx] FAILED\n", (char)(write ? 'W' : 'R'), (unsigned long)pa);

	if (!write)
		*(uint16_t*)buf = val;

	return ret;
}


static uint_fast8_t msDmaValid(void* context){
    auto* m=static_cast<MemoryStickController*>(context);
    if(!m->state.inserted||!(m->state.dmaCount&0x8000))return 0;
    const auto left=((m->state.length+7)&~7u)-std::min(m->state.offset,((m->state.length+7)&~7u));
    return std::min(m->state.credit,left);
}
struct MemoryStickController* msCtrlrInit(ArmMem *physMem,uint32_t baseAddr,PxaGpio* gpio,PxaDma* dma,Reschedule reschedule) {
    auto* m=new MemoryStickController{};m->gpio=gpio;m->dma=dma;m->reschedule=reschedule;
    if(const char* path=std::getenv("FROND_MS_TRACE"))m->trace=std::fopen(path,"w");
    msCtrlrReset(m);
    if(dma){const PxaDmaPeripheral fifo{msDmaValid,nullptr,nullptr,m,baseAddr+0x90};
        pxaDmaSetPeripheral(dma,DMA_CMR_DREQ_0,&fifo);}
    if(!memRegionAdd(physMem,baseAddr,MS_CONTROLLER_SIZE,msCtrlrPrvMemAccessF,m))ERR("cannot add MSC");
    return m;
}
void msCtrlrReset(MemoryStickController* m) {
    static_cast<LegacyMsRegisters&>(*m)={};m->systemReg=0x4455;
    const auto inserted=m->state.inserted;m->state={};m->state.inserted=inserted;
    msCtrlrDiscardSaveImage(m);msCardReset(m);msCtrlrRefresh(m);
}
void msCtrlrDestroy(MemoryStickController* m){if(m->dma){pxaDmaSetPeripheral(m->dma,DMA_CMR_DREQ_0,nullptr);}if(m->trace)std::fclose(m->trace);delete m;}
bool msCtrlrPresent(MemoryStickController* m){return m&&msPresent(m);}
bool msCtrlrPro(MemoryStickController* m){return m&&m->pro.Present();}
bool msCtrlrChanged(MemoryStickController* m){return m&&!m->pro.Present()&&m->card.media.Dirty();}
uint32_t msCtrlrMegabytes(MemoryStickController* m){
    if(!m||!msPresent(m))return 0;
    return m->pro.Present()?uint32_t((uint64_t(m->pro.Sectors())*512)>>20):m->card.media.Shape().megabytes;
}
bool msCtrlrInserted(MemoryStickController* m){return m&&m->state.inserted;}
bool msCtrlrReadOnly(MemoryStickController* m){return m&&(m->pro.Present()?m->pro.writeProtected:m->card.media.WriteProtected());}
bool msCtrlrProtect(MemoryStickController* m,bool protect){
    if(!msCtrlrPresent(m)||msCtrlrInserted(m))return false;
    if(m->pro.Present())m->pro.writeProtected=protect;else m->card.media.SetWriteProtected(protect);
    return true;
}
bool msCtrlrAttach(MemoryStickController* m,const std::vector<uint8_t>& physical) {
    if(!m||m->state.inserted||!frondMs::validBoot(physical))return false;
    return msCtrlrAttach(m,std::vector<uint8_t>(physical));
}
// Takes the image without copying it.
bool msCtrlrAttach(MemoryStickController* m,std::vector<uint8_t>&& physical) {
    if(!m||m->state.inserted||!frondMs::validBoot(physical))return false;
    if(m->card.media.Attach(std::move(physical),false)!=MemoryStickMedia::Result::ok)return false;
    m->pro.Detach();
    msCtrlrDiscardSaveImage(m);msCardReset(m);m->state={};m->state.inserted=1;msCtrlrRefresh(m);return true;
}
// The card store's classic stick, read and written in place. Once the store holds another card
// (its epoch moved on), every access fails and detaching leaves the store alone.
static MemoryStickMedia::Store msCardStore() {
    const auto at=sdCardEpoch();
    MemoryStickMedia::Store store;
    store.read=[at](uint64_t off,uint64_t n,void* dest){return sdCardEpoch()==at&&sdCardReadBytes(off,n,dest);};
    store.write=[at](uint64_t off,uint64_t n,const void* src){return sdCardEpoch()==at&&sdCardWriteBytes(off,n,src);};
    store.snapshot=[at]{return sdCardEpoch()==at?sdCardSnapshot():nullptr;};
    store.restore=[at](const std::shared_ptr<const CardSnapshot>& pin){return sdCardEpoch()==at&&sdCardRestore(pin);};
    store.release=[at]{if(sdCardEpoch()==at)sdCardReset();};
    return store;
}
static const frondMsGeometry::Geometry* msStoreGeometry() {
    if(!sdCardInitialized())return nullptr;
    const auto* layout=frondMsLayout::byStoredBytes(uint64_t(sdCardSectorCount())*SD_SECTOR_SIZE);
    return layout?frondMsGeometry::byShape(layout->blocks,layout->pagesPerBlock):nullptr;
}
bool msCtrlrAttachClassicStore(MemoryStickController* m,bool inserted,bool protect) {
    const auto* geometry=msStoreGeometry();
    if(!m||m->state.inserted||!geometry)return false;
    // The boot blocks are checked as for an image, before the stick replaces any other.
    MemoryStickMedia probe;
    MemoryStickMedia::Store peek=msCardStore();peek.release={};
    if(probe.AttachStore(*geometry,std::move(peek),protect)!=MemoryStickMedia::Result::ok)return false;
    std::vector<uint8_t> boot(size_t(2)*geometry->pagesPerBlock*MemoryStickMedia::recordBytes);
    for(uint32_t b=0;b<2;++b)for(uint32_t p=0;p<geometry->pagesPerBlock;++p){
        MemoryStickMedia::Page page;
        if(probe.Read(b,p,page)!=MemoryStickMedia::Result::ok)return false;
        std::memcpy(boot.data()+(size_t(b)*geometry->pagesPerBlock+p)*MemoryStickMedia::recordBytes,page.data(),page.size());
    }
    probe.Detach();
    if(!frondMs::validBootBlocks(boot.data(),*geometry))return false;
    m->pro.Detach();
    if(m->card.media.AttachStore(*geometry,msCardStore(),protect)!=MemoryStickMedia::Result::ok)return false;
    msCtrlrDiscardSaveImage(m);msCardReset(m);m->state={};m->state.inserted=inserted;msCtrlrRefresh(m);return true;
}
bool msCtrlrClassicOnStore(MemoryStickController* m){return m&&!m->pro.Present()&&m->card.media.OnStore();}
bool msCtrlrMoveClassicToStore(MemoryStickController* m,const char* id) {
    if(!m||m->pro.Present()||!m->card.media.Present())return false;
    if(m->card.media.OnStore())return true;
    if(sdCardInitialized())return false;
    std::vector<uint8_t> stored;
    if(!m->card.media.CopyStored(stored))return false;
    const auto geometry=m->card.media.Shape();
    const bool protect=m->card.media.WriteProtected();
    sdCardInitializeWithBytes(std::move(stored),id?id:"");
    // Same bytes, so the guest's transfer state carries on as it was.
    if(m->card.media.AttachStore(geometry,msCardStore(),protect)!=MemoryStickMedia::Result::ok){sdCardReset();return false;}
    return true;
}
bool msCtrlrAttachEjected(MemoryStickController* m,std::vector<uint8_t>&& physical,bool protect) {
    if(!m||m->state.inserted||!frondMs::validBoot(physical))return false;
    if(m->card.media.Attach(std::move(physical),protect)!=MemoryStickMedia::Result::ok)return false;
    m->pro.Detach();
    msCtrlrDiscardSaveImage(m);msCardReset(m);m->state={};msCtrlrRefresh(m);return true;
}
void msCtrlrSwapOnLoad(MemoryStickController* m,bool swap){if(m)m->swapLoad=swap;}
void msCtrlrSwapEject(MemoryStickController* m){if(m&&m->swapEject){m->swapEject=false;msCtrlrRefresh(m);}}
bool msCtrlrAttachPro(MemoryStickController* m,MsProStore store,bool inserted) {
    if(!m||m->state.inserted||!store.sectors||!store.read||!store.write||store.attribute.empty()||store.attribute.size()%512)return false;
    m->card.media.Detach();m->pro.Attach(std::move(store),false);
    msCtrlrDiscardSaveImage(m);msCardReset(m);m->state={};m->state.inserted=inserted;msCtrlrRefresh(m);return true;
}
void msCtrlrDetach(MemoryStickController* m) {
    if(!m)return;
    m->card.media.Detach();m->pro.Detach();
    msCtrlrDiscardSaveImage(m);msCardReset(m);m->state={};msCtrlrRefresh(m);
}
bool msCtrlrInsert(MemoryStickController* m,bool inserted){
    if(!msCtrlrPresent(m))return false;
    if(bool(m->state.inserted)==inserted)return false;
    m->state={};m->state.inserted=inserted;msCtrlrDiscardSaveImage(m);msCardReset(m);msCtrlrRefresh(m);return true;
}
bool msCtrlrExport(MemoryStickController* m,std::vector<uint8_t>& physical){return msCtrlrPresent(m)&&!m->pro.Present()&&m->card.media.CopyImage(physical)==MemoryStickMedia::Result::ok;}
bool msCtrlrSaveCheckpoint(MemoryStickController* m, std::shared_ptr<const CardSnapshot>& pin, bool& protect) {
    // A Pro stick's sectors are pinned with the card store that holds them.
    if (!msCtrlrPresent(m) || m->pro.Present()) { pin.reset(); protect = false; return false; }
    auto checkpoint = m->card.media.SaveCheckpoint();
    if (!checkpoint.pin) return false;
    pin = std::move(checkpoint.pin);
    protect = checkpoint.writeProtected;
    return true;
}
bool msCtrlrRestoreCheckpoint(MemoryStickController* m, const std::shared_ptr<const CardSnapshot>& pin, bool protect) {
    if (!m || !pin || m->pro.Present()) return false;
    MemoryStickMedia::Checkpoint checkpoint{pin, protect};
    return m->card.media.RestoreCheckpoint(checkpoint) == MemoryStickMedia::Result::ok;
}

template<typename H>static void msCtrlrState(LegacyMsRegisters& s,H& h){
    h.Do16(s.systemReg).Do16(s.reg_0).Do16(s.reg_40).Do16(s.reg_50).Do16(s.reg_2c0).Do16(s.reg_2d0).Do16(s.reg_2e0).Do16(s.reg_2f0).Do16(s.reg_188).Do16(s.reg_180).Do32(s.reply_lo_2f0).Do32(s.reply_hi_2f8);
}
template<typename H>static void msProtocol(MemoryStickController::State& s,MemoryStickCard::State& c,H& h){
    h.Do32(s.inserted).Do32(s.command).Do32(s.length).Do32(s.offset).Do32(s.status).Do32(s.control).Do32(s.dmaCount)
     .DoBuffer(s.buffer.data(),s.buffer.size()).DoBuffer(c.reg.data(),c.reg.size()).DoBuffer(c.page.data(),c.page.size())
     .Do32(c.readStart).Do32(c.readSize).Do32(c.writeStart).Do32(c.writeSize).Do32(c.operation).Do32(c.bufferFlags).Do32(s.config).Do32(s.irqPending).Do32(s.cardPending).Do32(s.credit).Do32(s.fifoPending);
}
template<typename H>static void msProProtocol(MemoryStickProCard::State& p,H& h){
    h.DoBuffer(p.reg.data(),p.reg.size()).DoBuffer(p.page.data(),p.page.size())
     .Do32(p.readStart).Do32(p.readSize).Do32(p.writeStart).Do32(p.writeSize)
     .Do32(p.operation).Do32(p.address).Do32(p.remaining).Do32(p.open).Do32(p.loaded);
}
template<typename T>void msCtrlrSave(MemoryStickController* m,T& savestate){
    msCtrlrDiscardSaveImage(m);
    auto* old=savestate.GetChunk(tg50MsChunk,0);if(!old){savestate.NotifyError();return;}
    SaveChunkHelper h(*old);msCtrlrState(*m,h);
    // Version 1 stores protocol state and a media reference, not a physical image. Version 2 adds
    // a Memory Stick Pro: its protocol state and the size of the stick it refers to.
    auto* chunk=savestate.GetChunk(ChunkType::sonyTg50MsCard,2);if(!chunk){savestate.NotifyError();return;}
    SaveChunkHelper card(*chunk);auto state=m->state;auto cs=m->card.state;auto ps=m->pro.state;
    msProtocol(state,cs,card);card.Do32(msPresent(m)).Do32(msCtrlrReadOnly(m));
    uint32_t pro=m->pro.Present(),sectors=m->pro.Sectors(),attribute=m->pro.AttributeSectors();
    card.Do32(pro).Do32(sectors).Do32(attribute);msProProtocol(ps,card);
}
template<typename T>bool msCtrlrLoad(MemoryStickController* m,T& loader,bool preserveInput){
    auto* old=loader.GetChunkOrFail(tg50MsChunk,0,"TG50 Memory Stick");if(!old)return false;
    LegacyMsRegisters legacy{};LoadChunkHelper h(*old);msCtrlrState(legacy,h);
    if(old->HasError()){loader.NotifyError();return false;}
    // Another stick, or none, since the save: the saved transfer state belonged to the old one.
    if(m->swapLoad){
        m->swapLoad=false;m->swapEject=true;
        msCtrlrDiscardSaveImage(m);static_cast<LegacyMsRegisters&>(*m)=legacy;m->state={};msCardReset(m);
        (void)preserveInput;return true;
    }
    MemoryStickController::State state{};MemoryStickCard::State cs{};
    uint32_t present=0,protect=0,pro=0;std::vector<uint8_t> physical;bool haveImage=false;
    MemoryStickProCard::State ps{};
    if(loader.GetChunk(ChunkType::sonyTg50MsCard)){
        uint32_t gotVersion=0;
        auto* chunk=loader.GetChunkOrFail(ChunkType::sonyTg50MsCard,2,"TG50 Memory Stick card",gotVersion);
        if(!chunk)return false;
        LoadChunkHelper card(*chunk);msProtocol(state,cs,card);card.Do32(present).Do32(protect);
        if(gotVersion>=2){
            uint32_t sectors=0,attribute=0;card.Do32(pro).Do32(sectors).Do32(attribute);msProProtocol(ps,card);
            // A Pro stick must be the one attached from the session's sidecar.
            if(chunk->HasError()||pro>1||(pro&&(!present||!m->pro.Present()||sectors!=m->pro.Sectors()||attribute!=m->pro.AttributeSectors()||
               !MemoryStickProCard::Valid(ps,sectors,attribute)))){loader.NotifyError();return false;}
        }
        if(present&&!pro&&m->pro.Present()){loader.NotifyError();return false;}
        if(chunk->HasError()||present>1||protect>1||state.inserted>1||(!present&&(state.inserted||protect))||
           state.command>0xffff||state.length>1024||state.offset>((state.length+7)&~7u)||state.control>0xffff||
           state.status>0xffff||state.dmaCount>0xffff||state.config>0xffff||state.irqPending>3||state.cardPending>1||state.credit>8||state.fifoPending>1||(state.fifoPending&&state.credit)){loader.NotifyError();return false;}
        // Version 1 refers to the stick attached from the sidecar; version 0 embeds a 128 MB one.
        const bool attached=present&&m->card.media.Present();
        if(!MemoryStickCard::Valid(cs,attached?m->card.media.Shape():frondMsGeometry::largest)){loader.NotifyError();return false;}
        if(gotVersion==0&&present){
            uint32_t size=0,crc=0;card.Do32(size).Do32(crc);
            if(chunk->HasError()||!size||size>mz_compressBound(MemoryStickMedia::maxImageBytes)){loader.NotifyError();return false;}
            std::vector<uint8_t> compressed(size);card.DoBuffer(compressed.data(),size);
            if(chunk->HasError()){loader.NotifyError();return false;}
            if(!m->card.media.Present()){
                physical.resize(MemoryStickMedia::maxImageBytes);mz_stream stream{};
                stream.next_in=compressed.data();stream.avail_in=compressed.size();
                stream.next_out=physical.data();stream.avail_out=physical.size();
                if(mz_inflateInit(&stream)!=MZ_OK){loader.NotifyError();return false;}
                const auto result=mz_inflate(&stream,MZ_FINISH);
                const bool valid=result==MZ_STREAM_END&&stream.total_in==compressed.size()&&stream.total_out==physical.size();
                mz_inflateEnd(&stream);
                if(!valid||
                   frondMs::crc32(physical.data(),physical.size())!=crc){loader.NotifyError();return false;}
                haveImage=true;
            }
        }else if(present&&!pro&&!m->card.media.Present()){loader.NotifyError();return false;}
    }else {MemoryStickCard card;card.Reset();cs=card.state;}
    // Parsing and allocation complete before replacing the card or its transfer state.
    if(haveImage){if(MemoryStickMediaStateAccess::Attach(m->card.media,std::move(physical),protect)!=MemoryStickMedia::Result::ok){loader.NotifyError();return false;}}
    else if(!present){m->card.media.Detach();auto release=std::move(m->pro.store.release);m->pro.Detach();if(release)release();}
    else if(pro)m->pro.writeProtected=protect;
    else m->card.media.SetWriteProtected(protect);
    msCtrlrDiscardSaveImage(m);static_cast<LegacyMsRegisters&>(*m)=legacy;m->state=state;m->card.state=cs;
    if(pro)m->pro.state=ps;else m->pro.Reset();
    (void)preserveInput;return true;
}
template void msCtrlrSave<Savestate<ChunkType>>(MemoryStickController*,Savestate<ChunkType>&);
template void msCtrlrSave<SavestateProbe<ChunkType>>(MemoryStickController*,SavestateProbe<ChunkType>&);
template bool msCtrlrLoad<SavestateLoader<ChunkType>>(MemoryStickController*,SavestateLoader<ChunkType>&,bool);
