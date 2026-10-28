#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "pxa_MMC.h"
#include "savestate/savestateAll.h"

static ArmMemAccessF accessRegister;
static void* controller;
static uint8_t sector[512];
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF f,void* context){accessRegister=f;controller=context;return true;}
static bool icLevel;
void pxaIcInt(PxaIc*,uint_fast8_t,bool level){icLevel=level;}
void pxaDmaExternalReq(PxaDma*,uint_fast8_t,bool){}
extern "C" void uarmAbort(){abort();}
static bool readSector(uint32_t n,void* out){if(n)return false;memcpy(out,sector,512);return true;}
static bool writeSector(uint32_t n,const void* in){if(n)return false;memcpy(sector,in,512);return true;}
static void writeReg(unsigned offset,uint32_t value,unsigned size=4){assert(accessRegister(controller,0x41100000+offset,size,true,&value));}
static uint32_t readReg(unsigned offset,unsigned size=4){uint32_t v=0;assert(accessRegister(controller,0x41100000+offset,size,false,&v));return v;}
static uint32_t cardCommand(VSD* card,unsigned cmd,uint32_t arg=0){uint8_t reply[17]{};vsdCommand(card,cmd,arg,reply);return (uint32_t(reply[1])<<24)|(uint32_t(reply[2])<<16)|(uint32_t(reply[3])<<8)|reply[4];}
static void initCard(VSD* card){
    cardCommand(card,0);
    for(int i=0;i<32;i++){cardCommand(card,55);if(cardCommand(card,41,0x00300000)&0x80000000u)break;}
    cardCommand(card,2);cardCommand(card,3);cardCommand(card,7,3u<<16);
}
struct CardState {
    VSD* card;
    template<class T> void Save(T& out){vsdSave(card,out);}
    void Load(SavestateLoader<ChunkType>& in){vsdLoad(card,in);}
};
static void command(unsigned cmd,unsigned arg,unsigned length,bool writing){
    writeReg(0,1);writeReg(0x1c,length);writeReg(0x20,1);writeReg(0x30,cmd);
    writeReg(0x34,arg>>16);writeReg(0x38,arg&65535);writeReg(0x10,writing?13:5);writeReg(0,2);
}
int main(){
    auto* card=vsdInit(readSector,writeSector,65536);initCard(card);
    auto* mmc=pxaMmcInit(nullptr,nullptr,nullptr,true);pxaMmcInsert(mmc,card);
    writeReg(0x24,0);writeReg(0x24,1);assert(readReg(0x24)==0);
    for(unsigned i=0;i<512;i++)sector[i]=uint8_t(i);
    command(17,0,512,false);
    assert(readReg(0x40)==0x03020100);
    assert(readReg(0x40,2)==0x0504);assert(readReg(0x40,1)==6);
    for(unsigned i=7;i<512;i++)assert(readReg(0x40,1)==uint8_t(i));
    assert(readReg(4)&0x800);
    command(24,0,512,true);
    for(unsigned i=0;i<128;i++)writeReg(0x44,0x44332211);
    assert(sector[0]==0x11&&sector[1]==0x22&&sector[2]==0x33&&sector[3]==0x44&&sector[511]==0x44);
    assert(readReg(4)&0x800);
    CardState target{card}; Savestate<ChunkType> checkpoint;
    assert(checkpoint.Save(target)); // Allocate the layout before a larger SD status response exists.
    // ACMD42 succeeds, and SD status is a real 64-byte data response with current bus width.
    cardCommand(card,55,3u<<16);cardCommand(card,42,0);
    cardCommand(card,55,3u<<16);cardCommand(card,6,2);
    cardCommand(card,55,3u<<16);cardCommand(card,13);
    assert(checkpoint.Save(target));
    uint8_t status[64];assert(vsdDataXferBlockFromCard(card,status,64)==SdDataOk && status[0]==0x80);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(checkpoint.GetBuffer(),checkpoint.GetSize(),target));
    memset(status,0,sizeof(status));
    assert(vsdDataXferBlockFromCard(card,status,64)==SdDataOk && status[0]==0x80 && status[10]==0x10);
    // Data errors end the transfer with DATA_TRAN_DONE and never fault later FIFO accesses.
    command(24,512,512,true);  // sector 1 has no backing store
    for(unsigned i=0;i<128;i++)writeReg(0x44,0);
    assert((readReg(4)&0x1804)==0x1804);
    writeReg(0x44,0);writeReg(0x44,0,1);assert(readReg(0x40)==0);
    command(17,512,512,false);
    assert((readReg(4)&0x801)==0x801);assert(readReg(0x40)==0);
    // Ejecting mid-block: write CRC error and DATA_TRAN_DONE, then response timeouts.
    command(24,0,512,true);
    for(unsigned i=0;i<64;i++)writeReg(0x44,0x55555555);
    pxaMmcInsert(mmc,nullptr);
    assert((readReg(4)&0x1804)==0x1804);
    writeReg(0x44,0x66666666);assert(readReg(0x40)==0);
    assert(sector[0]==0x11);  // the partial block was never written
    command(13,3u<<16,0,false);
    writeReg(0x10,1);writeReg(0,2);
    assert((readReg(4)&0x2002)==0x2002);
    // A reinserted card is idle: block commands are illegal and get no response (SD 4.6.1).
    vsdReset(card,65536);pxaMmcInsert(mmc,card);
    command(24,0,512,true);
    assert((readReg(4)&0x2002)==0x2002);assert(!(readReg(4)&0x800));
    for(unsigned i=0;i<128;i++)writeReg(0x44,0x77777777);
    assert(sector[0]==0x11);
    uint8_t reply[17]{};assert(vsdCommand(card,24,0,reply)==SdReplyNone);
    assert(vsdCommand(card,55,3u<<16,reply)==SdReplyNone);  // not addressed: no response
    initCard(card);
    command(24,0,512,true);
    for(unsigned i=0;i<128;i++)writeReg(0x44,0x88888888);
    assert((readReg(4)&0x1804)==0x1800&&sector[0]==0x88);
    for(unsigned i=0;i<128;i++)writeReg(0x44,0x44332211);  // ignored, no transfer
    assert(sector[0]==0x88);
    for(unsigned i=0;i<512;i+=4)memcpy(sector+i,"\x11\x22\x33\x44",4);
    // CMD7 with RCA 0 or another card's RCA deselects without a response (SD 4.3.1).
    {
        initCard(card);
        writeReg(0,1);writeReg(0x30,7);writeReg(0x34,0);writeReg(0x38,0);writeReg(0x10,0);writeReg(0,2);
        assert((readReg(4)&0x2022)==0x2000);  // command done, no response timeout or CRC error
        uint8_t r[17]{};
        assert(vsdCommand(card,7,0x00050000,r)==SdReplyNone);
        assert(((cardCommand(card,13,3u<<16)>>9)&15)==3);  // stand-by
        assert(vsdCommand(card,7,3u<<16,r)==SdReply48bitsAndBusy);
        assert(((cardCommand(card,13,3u<<16)>>9)&15)==4);  // transfer
    }
    // An illegal command is latched and reported once, in the next response (SD 4.10.1).
    {
        uint8_t r[17]{};
        assert(vsdCommand(card,63,0,r)==SdReplyNone);
        assert(cardCommand(card,13,3u<<16)&(1u<<22));
        assert(!(cardCommand(card,13,3u<<16)&(1u<<22)));
        assert(vsdCommand(card,63,0,r)==SdReplyNone);
        cardCommand(card,0);  // reset clears it
        initCard(card);
        assert(!(cardCommand(card,13,3u<<16)&(1u<<22)));
    }
    // Responses without status (R2, R3, R7) also clear the latch, and valid commands the card does
    // not implement are not illegal.
    {
        uint8_t r[17]{};
        initCard(card);
        assert(vsdCommand(card,63,0,r)==SdReplyNone);
        assert(vsdCommand(card,55,7u<<16,r)==SdReplyNone);  // not for this card: latch kept
        cardCommand(card,7,0);  // deselect: valid, no response, clears it
        assert(!(cardCommand(card,13,3u<<16)&(1u<<22)));
        cardCommand(card,7,3u<<16);
        assert(vsdCommand(card,56,0,r)==SdReplyNone && vsdCommand(card,42,0,r)==SdReplyNone);
        assert(!(cardCommand(card,13,3u<<16)&(1u<<22)));
        for(unsigned c:{28u,29u,30u})assert(vsdCommand(card,c,0,r)==SdReply48bits);
        assert(!(cardCommand(card,13,3u<<16)&(1u<<22)));
        // Erase zeroes the range; CMD38 without a range is an erase sequence error.
        assert(cardCommand(card,38)&(1u<<28));
        cardCommand(card,32,0);cardCommand(card,33,0);
        assert(!(cardCommand(card,38)&(1u<<28))&&sector[0]==0&&sector[511]==0);
        memset(sector,0x5a,512);
        cardCommand(card,32,0);cardCommand(card,33,0);cardCommand(card,38);
        assert(sector[0]==0&&sector[100]==0);
        // R2 (CMD10 needs stand-by) and R6 (CMD3) after an illegal command carry no bit.
        cardCommand(card,0);
        assert(vsdCommand(card,63,0,r)==SdReplyNone);
        for(int i=0;i<32;i++){cardCommand(card,55);if(cardCommand(card,41,0x00300000)&0x80000000u)break;}
        assert(vsdCommand(card,2,0,r)==SdReply136bits);
        assert(vsdCommand(card,3,0,r)==SdReply48bits&&!(r[3]&0x40));  // R6 bit 14 clear
        initCard(card);
        for(unsigned i=0;i<512;i+=4)memcpy(sector+i,"\x11\x22\x33\x44",4);
    }
    // PXA27x I_REG: DAT_ERR (bit 8) and RES_ERR (bit 9), gated by I_MASK (Linux pxamci.h).
    {
        writeReg(0x28,0x3ff);assert(readReg(0x28)==0x37f);  // bit 7 (TINT) is not modelled
        command(17,512,512,false);  // sector 1 has no backing store
        assert((readReg(4)&0x801)==0x801);
        assert((readReg(0x2c)&0x300)==0x100);assert(!icLevel);
        writeReg(0x28,0x37f&~0x100u);assert(icLevel);
        writeReg(0x28,0x37f);assert(!icLevel);
        command(63,0,0,false);  // no response
        assert((readReg(4)&0x2002)==0x2002);
        assert((readReg(0x2c)&0x300)==0x200);
        writeReg(0x28,0x37f&~0x200u);assert(icLevel);
        writeReg(0x28,0x37f);
        writeReg(0,1);writeReg(0x30,13);writeReg(0x34,3);writeReg(0x38,0);writeReg(0x10,1);writeReg(0,2);
        assert((readReg(4)&0x2022)==0x2000);  // a clean command clears both
        assert(!(readReg(0x2c)&0x300));
    }
    // A guest retrying illegal commands logs a bounded amount, and card reset restarts the count.
    {
        fflush(stderr);int saved=dup(2);FILE* sink=tmpfile();assert(sink&&saved>=0);dup2(fileno(sink),2);
        vsdReset(card,65536);pxaMmcInsert(mmc,card);
        for(unsigned i=0;i<20000;i++){command(24,0,512,true);assert((readReg(4)&0x2002)==0x2002);}
        fflush(stderr);long bytes=ftell(sink);
        vsdReset(card,65536);pxaMmcInsert(mmc,card);command(24,0,512,true);fflush(stderr);
        long again=ftell(sink);
        dup2(saved,2);close(saved);fclose(sink);
        assert(bytes>0&&bytes<4096);assert(again>bytes);  // limited, then full lines again after reset
    }
    // PXA25x keeps its previous one-byte-per-word access behaviour.
    card=vsdInit(readSector,writeSector,65536);initCard(card);
    mmc=pxaMmcInit(nullptr,nullptr,nullptr);pxaMmcInsert(mmc,card);
    writeReg(0x28,0x3ff);assert(readReg(0x28)==0x7f);  // PXA25x I_MASK has 7 bits
    command(17,0,512,false);assert(readReg(0x40)==0x11);assert(readReg(0x40)==0x22);
    puts("PASS: PXA27x packed FIFO widths, block write, PRTBUF, SD status, error and removal signalling, PXA25x compatibility");
}
