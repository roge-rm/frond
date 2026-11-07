// Firmware-free E2/E3 transport and storage checks through the production board.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "CPU.h"
#include "audio_queue.h"
#include "pxa_UART.h"
#include "sdcard.h"
#include "soc_PXA.h"

static void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(cpuMemOpExternal(soc.GetCpu(), &value, address, 4, true));
}
static uint32_t read(SocPXA& soc, uint32_t address) {
    uint32_t value=0;
    assert(cpuMemOpExternal(soc.GetCpu(), &value, address, 4, false));
    return value;
}
static void run(SocPXA& soc, unsigned us) {
    assert(soc.Run(uint64_t(us)*100,100000000)>=uint64_t(us)*100);
}
static std::vector<uint8_t> saved(SocPXA& soc) {
    assert(soc.Save());const auto state=soc.GetSavestate();
    return {static_cast<uint8_t*>(state.data),static_cast<uint8_t*>(state.data)+state.size};
}
struct Peer {
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
};
static void serial(SocPXA& soc) {
    auto* uart=soc.GetSerialUart();assert(uart && uart!=soc.GetIrUart());
    Peer peer{{0,0x7f,0x80,0xff},{}};
    pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t {
        auto& q=static_cast<Peer*>(p)->rx;
        if(q.empty())return UART_CHAR_NONE;
        auto c=q.front();q.pop_front();return c;
    },[](uint_fast16_t c,void* p){static_cast<Peer*>(p)->tx.push_back(c);},
    [](void* p){return !static_cast<Peer*>(p)->rx.empty();},nullptr,&peer);
    constexpr uint32_t base=PXA_FFUART_BASE;
    write(soc,base+12,0x83);write(soc,base,96);write(soc,base+4,0);
    write(soc,base+12,3);write(soc,base+16,8);write(soc,base+8,7);write(soc,base+4,0x41);
    for(uint8_t c:{0,0x7f,0x80,0xff})write(soc,base,c);
    pxaUartKick(uart);
    std::vector<uint8_t> received;std::vector<unsigned> arrivals;
    for(unsigned us=100;us<=10000 && received.size()<4;us+=100) {
        run(soc,100);
        const auto status=read(soc,base+20);assert(!(status&2));
        if(status&1) {received.push_back(read(soc,base));arrivals.push_back(us);}
    }
    const std::vector<uint8_t> binary{0,0x7f,0x80,0xff};
    assert(received==binary);
    for(unsigned i=1;i<arrivals.size();++i)assert(arrivals[i]-arrivals[i-1]>=900 && arrivals[i]-arrivals[i-1]<=1200);
    run(soc,5000);assert(peer.tx==binary);
    pxaUartSetFuncs(uart,nullptr,nullptr,nullptr,nullptr,nullptr);
    constexpr uint32_t hotSync=1u<<4;
    assert(read(soc,0x40e00000)&hotSync);
    assert(soc.PressHotSync() && !soc.PressHotSync());run(soc,100000);
    assert(!(read(soc,0x40e00000)&hotSync));run(soc,500000);
    assert(read(soc,0x40e00000)&hotSync);
}
static uint32_t codecRead(SocPXA& soc, unsigned reg) {
    const uint32_t address=0x40500200+reg*2;
    read(soc,address); // AC97 returns the previous register transaction.
    return read(soc,address);
}
static void audio(SocPXA& soc) {
    auto* queue=audioQueueCreate(128);soc.SetAudioQueue(queue);soc.SetPcmOutputEnabled(true);
    constexpr uint32_t codec=0x40500200;
    write(soc,codec+0x26*2,0x410f);assert(codecRead(soc,0x26)==0x410e);
    write(soc,codec+0x26*2,0);
    const uint32_t samples[]={0x12345678,0xff800001,0x7fff8000,0};
    for(auto sample:samples)write(soc,0x40500040,sample);
    run(soc,1000);
    uint32_t output[4]{};
    assert(audioQueuePopChunk(queue,output,4)==4 && !memcmp(output,samples,sizeof(samples)));
    write(soc,codec+0x18*2,0x1234);auto state=saved(soc);
    write(soc,codec+0x18*2,0);audioQueuePush(queue,0xdeadbeef);
    assert(soc.Load(state.size(),state.data()));
    assert(codecRead(soc,0x18)==0x1234 && audioQueuePendingSamples(queue)==0);
    soc.SetAudioQueue(nullptr);soc.SetPcmOutputEnabled(false);
}
static void command(SocPXA& soc,unsigned cmd,uint32_t arg,unsigned attr=1) {
    constexpr uint32_t mmc=0x41100000;
    write(soc,mmc,1);
    if(attr&4){write(soc,mmc+0x1c,512);write(soc,mmc+0x20,1);}
    write(soc,mmc+0x30,cmd);write(soc,mmc+0x34,arg>>16);
    write(soc,mmc+0x38,arg&0xffff);write(soc,mmc+0x10,attr);write(soc,mmc,2);
}
static void initCard(SocPXA& soc) {
    command(soc,0,0,0);
    bool ready=false;
    for(unsigned i=0;i<32 && !ready;++i) {
        command(soc,55,0);command(soc,41,0x00300000,3);
        ready=(read(soc,0x4110003c)&0x80)!=0;
    }
    assert(ready);
    command(soc,2,0,2);command(soc,3,0);command(soc,7,3u<<16);
}
static void card(SocPXA& soc) {
    constexpr uint32_t detect=1u<<10,mmc=0x41100000;
    assert(!soc.SdInserted() && (read(soc,0x40e00000)&detect));
    sdCardInitialize(65536,"e2-family-card-a");soc.SdInsert();
    assert(soc.SdInserted() && !(read(soc,0x40e00000)&detect));initCard(soc);
    command(soc,24,512,13);
    uint8_t expected[512];for(unsigned i=0;i<512;++i){expected[i]=uint8_t(i*73);write(soc,mmc+0x44,expected[i]);}
    assert(read(soc,mmc+4)&0x800);
    uint8_t sector[512]{};assert(sdCardRead(1,sector) && !memcmp(sector,expected,512));
    auto state=saved(soc);soc.SdEject();
    assert(!soc.SdInserted() && (read(soc,0x40e00000)&detect));
    assert(soc.Load(state.size(),state.data()) && soc.SdInserted() && soc.SdRemount());
    initCard(soc);command(soc,17,512,5);
    for(auto byte:expected)assert(read(soc,mmc+0x40)==byte);
    // A different backing cannot be remounted as the card held by the saved controller.
    sdCardInitialize(65536,"e2-family-card-b");assert(!soc.SdRemount());
    assert(!soc.SdInserted() && (read(soc,0x40e00000)&detect));
    soc.SdInsert();initCard(soc);command(soc,17,512,5);
    for(unsigned i=0;i<512;++i)assert(read(soc,mmc+0x40)==0);
    soc.SdEject();sdCardReset();
}
int main() {
    for(auto type:{deviceTypeE2,deviceTypeFrankenE2}) {
        std::vector<uint8_t> rom(8u<<20),nand(34603008,0xff);
        SocPXA soc(type,32u<<20,rom.data(),rom.size(),nand.data(),nand.size(),-1,0);
        write(soc,0xa0003000,0xeafffffe);cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
        cpuSetReg(soc.GetCpu(),15,0xa0003000);
        serial(soc);audio(soc);card(soc);
        assert(soc.EnableFastAudio() && !soc.FastAudioActive());
        assert(!soc.EnableFastAudio() && !soc.FastAudioActive());
        printf("PASS: synthetic %s paced FFUART binary transport, HotSync GPIO pulse, AC97 PCM/restore and SD MMIO write/read/restore/backing swap\n",type==deviceTypeE2?"E2":"E3");
    }
}
