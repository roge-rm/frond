#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "audio_queue.h"
#include "MMU.h"
#include "rom_info5.h"

static void exercise(DeviceType5 device,bool denvfs=false) {
    const bool e2=deviceProfileIsE2Board5(device);
    std::vector<uint8_t> rom((denvfs?6:e2?8:16)*1024*1024), nand(e2?34603008:132*1024*1024,0xff);
    auto norWrite=[&](uint32_t a,uint32_t v,unsigned n=4){assert(a+n<=rom.size());std::memcpy(rom.data()+a,&v,n);};
    if(denvfs) {
        norWrite(0x20008,0xfeedbeef);norWrite(0x2006c,rom.size()-0x20000);
        norWrite(0x20080,deviceCompanyPalm5);norWrite(0x20084,0x68737072);
        std::memcpy(rom.data()+0x20010,"rePalm",7);std::memcpy(rom.data()+0x20030,"Dmitry.GR",10);
        assert(RomInfo5(rom.data(),rom.size()).GetFirmwareVariant()==&deviceFirmwareVariants5[0]);
    }
    const bool arm=denvfs || device==deviceTypeFrankenE2;
    const uint32_t start=denvfs?0x205bc180:device==deviceTypeFrankenE2?0x207a2e60:e2?0x20065de4:0x2004ae3c;
    const uint32_t controlCall=denvfs?0x205bc200:device==deviceTypeFrankenE2?0x207a2ee0:e2?0x20065e30:0x2004ae88;
    if(arm) {
        for(auto [a,v]:{std::pair<uint32_t,uint32_t>{start,0xe92d43f0},{start+4,0xe5999000},
            {controlCall-12,0xe5940028},{controlCall-8,0xe28d200c},{controlCall-4,0xe3a01005},
            {controlCall,denvfs?0xeb00543b:0xeb00542b},{controlCall+4,0xe3500000}})norWrite(a-0x20000000,v);
    } else {
        norWrite(start-0x20000000,0xb089b5f0);norWrite(start+4-0x20000000,0x1c04464d);
        norWrite(controlCall-4-0x20000000,0x21056aa0);
        norWrite(controlCall-0x20000000,e2?0xfa0ef020:0xfda0f021);norWrite(controlCall+4-0x20000000,0x2800,2);
    }
    SocPXA soc(device,(e2?16:32)*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,e2?0:2);
    auto cpu=soc.GetCpu();auto queue=audioQueueCreate(8192);soc.SetAudioQueue(queue);
    auto write=[&](uint32_t a,uint32_t v,unsigned n=4){assert(cpuMemOpExternal(cpu,&v,a,n,true));};
    const uint32_t entry=0xa0012000,table=0xa0010000,globals=0xa0011004,object=0xa0016000,pcm=0xa0018000;
    write(globals-4,table);write(globals,globals);write(table+0x320,entry);
    write(entry,0xe92d4200);write(entry+4,0xe5999000);write(entry+8,0xe5999000);
    write(entry+32,0xe59fe100);write(entry+36,0xe0800100);write(entry+40,0xe08ee009);
    write(entry+44,0xe08e0100);write(entry+48,0xe5900004);
    write(entry-0x5f8,0xe4920004);write(entry-0x5f4,0xe1a005c0);write(entry-0x5f0,0xe250cc7f);
    write(entry+40+0x100,0x3ffc);write(0xa0015004,object);
    write(object,0x12);write(object+4,44100);write(object+8,2,2);
    write(object+40,1024,2);write(object+42,1024,2);
    for(unsigned i=0;i<4096;i+=4)write(pcm+i,0xfc1803e8);
    if(e2) {
        const uint32_t code=0xa001c000;
        write(entry,0xe59fc004);write(entry+4,0xe08cc00f);write(entry+8,0xe12fff1c);
        write(entry+12,code+1-entry-12);
        for(auto [offset,value]:{std::pair<unsigned,uint16_t>{0,0xb570},{2,0x1c1c},{4,0x2800},
            {10,0x2810},{14,0x2314},{16,0x4358},{18,0x464e},{20,0x6836},{22,0x6836},
            {24,0x4b20},{26,0x199b},{28,0x331c},{30,0x18c0},{32,0x6840}})write(code+offset,value,2);
        write(code+28+128,0x3ffc-28);
        for(auto [offset,value]:{std::pair<int,uint16_t>{-0x420,0x6810},{-0x41e,0x6851},
            {-0x418,0x12c0},{-0x416,0x12c9},{0x1d7c,0x8981},{0x1d82,0x8941},
            {0x1d84,0x8541},{0x1d86,0x8501}})write(code+offset,value,2);
    }
    write(0xa0000000,0xe519c004); // LDR ip,[r9,#-4]
    write(0xa0000004,0xe59cf320); // LDR pc,[ip,#0x320]
    auto call=[&](){
        write(0xa0000000,0xe519c004);write(0xa0000004,0xe59cf320);
        cpuInvalidateCodeCache(cpu);cpuSetReg(cpu,ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC);
        cpuSetReg(cpu,0,0);cpuSetReg(cpu,1,pcm);cpuSetReg(cpu,2,4096);
        cpuSetReg(cpu,9,globals);cpuSetReg(cpu,15,0xa0000000);
        cpuCyclePure(cpu);cpuCyclePure(cpu);
        assert(cpuGetRegisters(cpu)[15]==entry);
    };
    call();assert(!soc.FastAudioActive() && audioQueuePendingSamples(queue)==0);
    assert(soc.EnableFastAudio());soc.SetPcmOutputEnabled(true);
    assert(!soc.EnableFastAudio());
    // The same E2 device selects an ARM start only when its full variant header matches.
    const uint32_t pageTable=0xa0020000,control=0xa001f000;
    write(pageTable+4*0xa00,0xa0000c02);
    write(pageTable+4*(start>>20),(start&0x0ff00000)|0xc02);
    mmuSetDomainCfg(cpuGetMMU(cpu),3);mmuSetTTP(cpuGetMMU(cpu),pageTable);
    write(table+0x324,control);write(control,0xe3a00000);write(control+4,0xe12fff1e);
    write(0xa0000004,0xe59cf324);cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu,0,0);cpuSetReg(cpu,1,5);cpuSetReg(cpu,9,globals);
    cpuSetReg(cpu,14,controlCall+(arm?4:5));cpuSetReg(cpu,15,0xa0000000);
    cpuCyclePure(cpu);cpuCyclePure(cpu);
    const auto state=soc.AudioHostState();
    assert(state.controls.size()==1 && state.controls[0].qualifiedStart);
    cpuCyclePure(cpu);cpuCyclePure(cpu);cpuCyclePure(cpu);
    assert(soc.AudioHostState().controls.empty() && soc.AudioHostState().knownChannel==0);
    call();assert(soc.FastAudioActive() && audioQueuePendingSamples(queue)==1536);
    // Hardware still runs while the normal host PCM route is muted.
    write(0xa0000000,0xeafffffe);cpuInvalidateCodeCache(cpu);cpuSetReg(cpu,15,0xa0000000);
    soc.SetPcmSuspended(true);soc.Run(100000,10000000);
    assert(audioQueuePendingSamples(queue)==1536);
    assert(soc.Save());auto saved=soc.GetSavestate();std::vector<uint8_t> snapshot(saved.size);
    std::memcpy(snapshot.data(),saved.data,saved.size);
    assert(soc.Load(snapshot.size(),snapshot.data()));
    assert(!soc.FastAudioActive() && audioQueuePendingSamples(queue)==0);
    call();assert(soc.FastAudioActive() && audioQueuePendingSamples(queue)==1536);
    soc.NotifyFastAudioUnderrun();
    assert(!soc.FastAudioActive() && audioQueuePendingSamples(queue)==0);
    assert(soc.Load(snapshot.size(),snapshot.data()));
    call();assert(soc.FastAudioActive() && audioQueuePendingSamples(queue)==1536);
    soc.Reset();assert(!soc.FastAudioActive() && audioQueuePendingSamples(queue)==0);
    // A changed runtime driver cannot reactivate preview after a reset.
    write(entry,0);call();assert(!soc.FastAudioActive() && soc.AudioHostState().failed);
    assert(audioQueuePendingSamples(queue)==0);
    puts("PASS: real ARM HAL interception is opt-in; no duplicate hardware PCM; restore/reset clear preview");
}

int main(){exercise(deviceTypeTX);exercise(deviceTypeE2);exercise(deviceTypeFrankenE2);exercise(deviceTypeE2,true);}
