#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "fast_audio.h"

static void exercise(FastAudio::Driver driver) {
    std::vector<uint8_t> ram(0x10000);
    auto write=[&](uint32_t a,uint32_t v,unsigned size=4){assert(a+size<=ram.size());std::memcpy(ram.data()+a,&v,size);};
    auto reader=[&](uint32_t a,void* p,unsigned size){if(a>ram.size() || size>ram.size()-a)return false;std::memcpy(p,ram.data()+a,size);return true;};
    uint32_t r[16]{};r[9]=0x100;r[12]=0x1000;r[1]=0x8000;r[2]=4096;
    const uint32_t entry=0x2000,object=0x5000;
    write(0x1320,entry);write(entry,0xe92d4200);write(entry+4,0xe5999000);write(entry+8,0xe5999000);
    write(entry+32,0xe59fe100);write(entry+36,0xe0800100);write(entry+40,0xe08ee009);
    write(entry+44,0xe08e0100);write(entry+48,0xe5900004);
    write(entry-0x5f8,0xe4920004);write(entry-0x5f4,0xe1a005c0);write(entry-0x5f0,0xe250cc7f);
    write(entry+40+0x100,0x1000);write(0x100,0x200);write(0x200,0x3000);write(0x4004,object);
    write(object,0x12);write(object+4,44100);write(object+8,2,2);write(object+40,1024,2);write(object+42,512,2);
    const uint32_t first=driver==FastAudio::Driver::tx?0xe92d4200:0xe59fc004;
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
    write(0x4018,object+128);
    preview.Handle(0x4320,r,4000,true);assert(preview.Failed() && preview.Bypass());
    assert(audioQueuePendingSamples(queue)==0);
    preview.Tick(250004001);assert(!preview.Bypass());
    write(0x4018,0);preview.Reset();write(object+40,1024,2);write(object+42,1024,2);
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
            uint16_t before;std::memcpy(&before,ram.data()+addr,2);
            write(addr,before^1,2);preview.Reset();preview.Handle(0x4320,r,0,true);
            assert(preview.Failed() && !preview.Active());write(addr,before,2);
        }
        FastAudio wrong(reader,FastAudio::Driver::tx);wrong.SetQueue(queue);
        wrong.Handle(0x4320,r,0,true);assert(wrong.Failed() && !wrong.Active());
        preview.Reset();write(entry+12,0x7fffffff);preview.Handle(0x4320,r,0,true);
        assert(preview.Failed() && !preview.Active());
    }
    puts("PASS: fast audio profile, PCM/gains, leading silence, pause, fallback, close and reset");
}

int main(){exercise(FastAudio::Driver::tx);exercise(FastAudio::Driver::e2);}
