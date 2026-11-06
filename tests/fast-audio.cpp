#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "fast_audio.h"

// RAM at 0 and the Zire 72 NOR code range at its real addresses.
struct Memory {
    std::vector<uint8_t> ram=std::vector<uint8_t>(0x10000),code=std::vector<uint8_t>(0x80000);
    static constexpr uint32_t codeBase=0x20040000;
    uint8_t* At(uint32_t a,unsigned size) {
        if(a<ram.size() && size<=ram.size()-a)return ram.data()+a;
        if(a>=codeBase && a-codeBase<code.size() && size<=code.size()-(a-codeBase))return code.data()+(a-codeBase);
        return nullptr;
    }
};

static void exercise(FastAudio::Driver driver) {
    Memory memory;
    const bool zire=driver==FastAudio::Driver::zire72;
    auto write=[&](uint32_t a,uint32_t v,unsigned size=4){auto* p=memory.At(a,size);assert(p);std::memcpy(p,&v,size);};
    auto reader=[&](uint32_t a,void* p,unsigned size){auto* from=memory.At(a,size);if(!from)return false;std::memcpy(p,from,size);return true;};
    uint32_t r[16]{};r[9]=0x100;r[12]=0x1000;r[1]=0x8000;r[2]=4096;
    const uint32_t entry=zire?0x2009ce1c:0x2000,object=0x5000,table=0x4000;
    // Channel table entries: 20 bytes with the object at +4, or 16 bytes with it at +0 on the Zire.
    const uint32_t stride=zire?16:20,slot=zire?0:4;
    write(0x1320,entry);write(entry,0xe92d4200);write(entry+4,0xe5999000);write(entry+8,0xe5999000);
    if(!zire) {
        write(entry+32,0xe59fe100);write(entry+36,0xe0800100);write(entry+40,0xe08ee009);
        write(entry+44,0xe08e0100);write(entry+48,0xe5900004);
        write(entry-0x5f8,0xe4920004);write(entry-0x5f4,0xe1a005c0);write(entry-0x5f0,0xe250cc7f);
        write(entry+40+0x100,0x1000);
    }
    write(0x100,0x200);write(0x200,0x3000);write(table+slot,object);
    write(object,0x12);write(object+4,44100);write(object+8,2,2);write(object+40,1024,2);write(object+42,512,2);
    const uint32_t first=driver==FastAudio::Driver::e2?0xe59fc004:0xe92d4200;
    // Zire 72 words the profile checks: HAL write, queue writer and mixer thread, at their NOR addresses.
    const std::pair<uint32_t,uint32_t> zireCode[] = {
        {entry+12,0xe3500000},{entry+20,0xe3500010},{entry+28,0xe59fc100},{entry+32,0xe089c00c},
        {entry+36,0xe79c0200},{entry+40,0xe3500000},{entry+68,0xe8bd4200},{entry+72,0xeaffc8d5},
        {0x2008f1d4,0xe5983000},{0x2008f1e0,0xe1d830f8},{0x2008f1ec,0xe5983004},
        {0x2008f2d8,0xe3530011},{0x2008f39c,0xe3530002},
        {0x2009c83c,0xe35c0012},{0x2009c84c,0xe1d730f8},{0x2009c850,0xe3530002},{0x2009c860,0xebffc645},
        {0x2009c930,0xe493b004},{0x2009c938,0xe1a025cb},{0x2008e214,0xe1db22f8},{0x2008e228,0xe1db22fa}};
    if(zire) {
        for(auto [addr,value]:zireCode)write(addr,value);
        write(entry+36+0x100,0x1000);
    }
    if(driver==FastAudio::Driver::e2) {
        const uint32_t code=0x6000;
        write(entry,0xe59fc004);write(entry+4,0xe08cc00f);write(entry+8,0xe12fff1c);
        write(entry+12,code+1-entry-12);
        for(auto [offset,value]:{std::pair<unsigned,uint16_t>{0,0xb570},{2,0x1c1c},{4,0x2800},
            {10,0x2810},{14,0x2314},{16,0x4358},{18,0x464e},{20,0x6836},{22,0x6836},
            {24,0x4b20},{26,0x199b},{28,0x331c},{30,0x18c0},{32,0x6840}})write(code+offset,value,2);
        write(code+28+128,0x1000-28);
        for(auto [offset,value]:{std::pair<int,uint16_t>{-0x420,0x6810},{-0x41e,0x6851},
            {-0x418,0x12c0},{-0x416,0x12c9},{0x1d7c,0x8981},{0x1d82,0x8941},
            {0x1d84,0x8541},{0x1d86,0x8501}})write(code+offset,value,2);
    }
    FastAudio preview(reader,driver);auto queue=audioQueueCreate(8192);preview.SetQueue(queue);
    preview.Handle(0x4320,r,0,true);
    assert(preview.Active() && audioQueuePendingSamples(queue)==0);
    for(unsigned off=0;off<4096;off+=4)write(r[1]+off,0xf82f07d1); // 2001, -2001
    preview.Handle(0x4320,r,1000,true);
    uint32_t padding[512];assert(audioQueuePopChunk(queue,padding,512)==512);
    for (auto value : padding) assert(value==0);
    uint32_t sample;assert(audioQueuePopChunk(queue,&sample,1)==1);
    assert(int16_t(sample)==1000 && int16_t(sample>>16)==-501);
    audioQueueClear(queue);
    // Per-stream gains are read for every block; muted output stays silent.
    write(object+40,0,2);write(object+42,0,2);
    preview.Handle(0x4320,r,2000,true);assert(audioQueuePopChunk(queue,&sample,1)==1 && sample==0);
    audioQueueClear(queue);
    preview.Handle(0x4320,r,3000,false);assert(audioQueuePendingSamples(queue)==0);
    preview.RestartOutput();
    assert(audioQueuePendingSamples(queue)==0);
    // Multiple open channels return to the normal path, with no delayed echo.
    write(table+stride+slot,object+128);
    preview.Handle(0x4320,r,4000,true);assert(preview.Failed() && preview.Bypass());
    assert(audioQueuePendingSamples(queue)==0);
    preview.Tick(250004001);assert(!preview.Bypass());
    write(table+stride+slot,0);preview.Reset();write(object+40,1024,2);write(object+42,1024,2);
    preview.Handle(0x4320,r,300000000,true);assert(preview.Active());
    preview.Handle(0x4318,r,300000001,true);assert(!preview.Active() && !preview.Failed());
    preview.Tick(550000002);assert(!preview.Bypass());
    // An unrecognised driver or unsupported format cannot activate bypass.
    preview.Reset();write(entry,0);preview.Handle(0x4320,r,0,true);assert(!preview.Bypass() && preview.Failed());
    write(entry,first);preview.Reset();write(object+4,48000);
    preview.Handle(0x4320,r,0,true);assert(!preview.Bypass() && preview.Failed());
    write(object+4,44100);preview.Reset();
    // Reject wrapped/unmapped PCM without reading a partial block into the queue.
    r[1]=0xfffffff0;preview.Handle(0x4320,r,0,true);
    assert(preview.Failed() && !preview.Bypass() && audioQueuePendingSamples(queue)==0);
    preview.Reset();r[1]=0xf800;preview.Handle(0x4320,r,0,true);
    assert(preview.Failed() && !preview.Bypass() && audioQueuePendingSamples(queue)==0);
    preview.Reset();r[1]=0x8000;write(object+8,1,2);
    preview.Handle(0x4320,r,0,true);assert(preview.Failed() && !preview.Bypass());
    preview.Reset();write(object+8,2,2);
    // A stalled consumer falls back before overflowing the host queue.
    for (unsigned i=0;i<7;++i) preview.Handle(0x4320,r,i,true);
    assert(preview.Failed() && preview.Bypass() && audioQueuePendingSamples(queue)==0);
    preview.Tick(250000010);assert(!preview.Bypass());preview.Reset();
    // A 1024-frame host period starts with 1024 frames of silence and holds up to 12 periods.
    {
        auto large=audioQueueCreate(16384);preview.SetQueue(large);preview.SetPeriod(1024);
        for (unsigned i=0;i<11;++i) preview.Handle(0x4320,r,i,true);
        assert(preview.Active() && !preview.Failed() && audioQueuePendingSamples(large)==12*1024);
        uint32_t silence[1024];assert(audioQueuePopChunk(large,silence,1024)==1024);
        for (auto value : silence) assert(value==0);
        assert(audioQueuePopChunk(large,&sample,1)==1 && sample!=0);
        preview.Handle(0x4320,r,11,true);preview.Handle(0x4320,r,12,true);
        assert(preview.Failed() && audioQueuePendingSamples(large)==0);
        preview.Tick(250000012);preview.Reset();preview.SetPeriod(512);preview.SetQueue(queue);
    }
    preview.Handle(0x4320,r,0,true);assert(preview.Active());preview.Reset();assert(!preview.Bypass());
    preview.Handle(0x4320,r,0,true);
    preview.NotifyUnderrun(10);
    assert(preview.Failed() && !preview.Active() && preview.Bypass());
    assert(audioQueuePendingSamples(queue)==0);
    preview.Tick(250000011);assert(!preview.Bypass());
    preview.Handle(0x4320,r,300000000,true);assert(!preview.Active());
    assert(FastAudio::Scale(-1,1024)==-1 && FastAudio::Scale(32767,1024)==16383);
    if(driver==FastAudio::Driver::e2) {
        for(uint32_t addr:{0x6000u,0x601cu,0x6000u-0x418,0x6000u+0x1d84}) {
            uint16_t before;std::memcpy(&before,memory.At(addr,2),2);
            write(addr,before^1,2);preview.Reset();preview.Handle(0x4320,r,0,true);
            assert(preview.Failed() && !preview.Active());write(addr,before,2);
        }
        FastAudio wrong(reader,FastAudio::Driver::tx);wrong.SetQueue(queue);
        wrong.Handle(0x4320,r,0,true);assert(wrong.Failed() && !wrong.Active());
        preview.Reset();write(entry+12,0x7fffffff);preview.Handle(0x4320,r,0,true);
        assert(preview.Failed() && !preview.Active());
    }
    if(zire) {
        // Any changed word in the HAL write, queue writer or mixer refuses the preview.
        for(auto [addr,value]:zireCode) {
            write(addr,value^0x10);preview.Reset();preview.Handle(0x4320,r,0,true);
            assert(preview.Failed() && !preview.Active());write(addr,value);
        }
        write(entry+36+0x100,0x1000-16);preview.Reset();preview.Handle(0x4320,r,0,true);
        assert(preview.Failed() && !preview.Active());write(entry+36+0x100,0x1000);
        preview.Reset();preview.Handle(0x4320,r,0,true);assert(preview.Active());
        // The TX and E2 profiles refuse the Zire driver, and the Zire profile refuses the TX one.
        for(auto other:{FastAudio::Driver::tx,FastAudio::Driver::e2}) {
            FastAudio wrong(reader,other);wrong.SetQueue(queue);
            wrong.Handle(0x4320,r,0,true);assert(wrong.Failed() && !wrong.Active());
        }
        // Qualified start: SndStreamStart's ARM call to HAL control 5 at its NOR address.
        const uint32_t start=0x20049d5c,call=0x20049ddc;
        const std::pair<uint32_t,uint32_t> startCode[] = {{start,0xe92d43f0},{start+4,0xe5999000},
            {call-12,0xe5940028},{call-8,0xe28d200c},{call-4,0xe3a01005},{call,0xeb00543b},{call+4,0xe3500000}};
        for(auto [addr,value]:startCode)write(addr,value);
        uint32_t ctl[16]{};ctl[1]=5;ctl[14]=call+4;
        assert(preview.QualifiedStart(ctl));
        ctl[14]=call+5;assert(!preview.QualifiedStart(ctl));ctl[14]=call+4;
        for(auto [addr,value]:startCode) {
            write(addr,value^1);assert(!preview.QualifiedStart(ctl));write(addr,value);
        }
        // A start with a result of 0 tracks the channel; a stop holds the preview until the next start.
        preview.Reset();uint32_t result[16]{};
        preview.ControlHead(ctl,10,true);preview.ControlTail(ctl,result,11);assert(!preview.ControlSuspended());
        ctl[1]=6;preview.ControlHead(ctl,12,true);preview.ControlTail(ctl,result,13);assert(preview.ControlSuspended());
        preview.Handle(0x4320,r,14,true);assert(!preview.Active());
        ctl[1]=5;preview.ControlHead(ctl,15,true);preview.ControlTail(ctl,result,16);
        preview.Handle(0x4320,r,17,true);assert(preview.Active() && !preview.Failed());
        // The Thumb early hook never runs here, so an early transition records nothing.
        uint32_t regs[16]{};preview.EarlyTransition(regs,0x2006c974,0x20057cd4,18);
        assert(preview.EarlyStatus().ready==0 && preview.EarlyStatus().rejected==0 && preview.EarlyStatus().pending==0);
    }
    puts("PASS: fast audio profile, PCM/gains, leading silence, pause, fallback, close and reset");
}

int main(){exercise(FastAudio::Driver::tx);exercise(FastAudio::Driver::e2);exercise(FastAudio::Driver::zire72);}
