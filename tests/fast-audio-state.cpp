#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "audio_queue.h"

int main() {
    std::vector<uint8_t> rom(16*1024*1024), nand(132*1024*1024,0xff);
    SocPXA soc(DeviceType5::deviceTypeTX,32*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,2);
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
    write(0xa0000000,0xe519c004); // LDR ip,[r9,#-4]
    write(0xa0000004,0xe59cf320); // LDR pc,[ip,#0x320]
    auto call=[&](){
        write(0xa0000000,0xe519c004);
        cpuInvalidateCodeCache(cpu);cpuSetReg(cpu,ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC);
        cpuSetReg(cpu,0,0);cpuSetReg(cpu,1,pcm);cpuSetReg(cpu,2,4096);
        cpuSetReg(cpu,9,globals);cpuSetReg(cpu,15,0xa0000000);
        cpuCyclePure(cpu);cpuCyclePure(cpu);
        assert(cpuGetRegisters(cpu)[15]==entry);
    };
    call();assert(!soc.FastAudioActive() && audioQueuePendingSamples(queue)==0);
    assert(soc.EnableFastAudio());soc.SetPcmOutputEnabled(true);
    assert(!soc.EnableFastAudio());
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
    SocPXA e2(DeviceType5::deviceTypeE2,16*1024*1024,rom.data(),8*1024*1024,
              nand.data(),33*1024*1024,-1,0);
    assert(!e2.EnableFastAudio());
    puts("PASS: real ARM HAL interception is opt-in; no duplicate hardware PCM; restore/reset clear preview");
}
