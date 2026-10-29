#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <map>
#include "fast_audio.h"
static void exercise(FastAudio::Driver driver) {
    std::vector<uint8_t> ram(0x10000);
    auto write=[&](uint32_t a,uint32_t v,unsigned size=4){assert(a+size<=ram.size());std::memcpy(ram.data()+a,&v,size);};
    std::map<uint32_t,uint8_t> code;
    auto reader=[&](uint32_t a,void* p,unsigned size){if(a>=0x20000000) { for(unsigned i=0;i<size;++i) {auto it=code.find(a+i);if(it==code.end())return false;static_cast<uint8_t*>(p)[i]=it->second;}return true;}if(a>ram.size() || size>ram.size()-a)return false;std::memcpy(p,ram.data()+a,size);return true;};
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
    const bool e2=driver==FastAudio::Driver::e2;
    auto codeWord=[&](uint32_t a,uint32_t v){for(unsigned i=0;i<4;++i)code[a+i]=v>>(i*8);};
    const uint32_t wrapper=e2?0x20072c6c:0x20057cc4,writer=e2?0x2006659c:0x2004b5f4;
    codeWord(wrapper,0x1c04b510);codeWord(wrapper+4,0x43d22200);codeWord(wrapper+8,0xff48f7ff);
    codeWord(wrapper+12,e2?0xf01368e0:0xf01468e0);codeWord(writer,0x1c05b5f8);
    codeWord(writer+0x18,0x6920d007);codeWord(writer+0x1c,0xd00a2800);codeWord(writer+0x20,e2?0xf01e1c20:0xf0201c20);
    codeWord((e2?0x200665ec:0x2004b644)+0x34,0x2e00692e);
    codeWord((e2?0x200665ec:0x2004b644)+0x58,e2?0xfd38f019:0xf8d2f01b);
    codeWord(e2?0x200800b8:0x20066844,0x47384730);
    const uint32_t from=e2?0x20072c7c:0x20057cd4,pc=e2?0x200861f9:0x2006c975;
    write(0xfc,0x1000);write(object+44,44100);
    const uint32_t stream=0xa000,message=0xb000,stack=0xc000,queue=0xd000,array=0xe000,semaphore=0xf100;
    write(stream+36,queue);write(stream+40,0);
    write(message,stream);write(message+4,0x8000);write(message+8,4096);write(message+12,4096);write(message+16,0);
    write(stack,message);write(stack+4,e2?0x20066669:0x2004b6c1);
    write(queue,array);write(queue+4,0x10010);write(queue+12,semaphore);write(array,message);
    uint32_t ready[16]{};ready[0]=semaphore;ready[4]=queue;ready[5]=stream;ready[9]=0x100;ready[13]=stack;
    uint32_t hal[16]{};hal[0]=0;hal[1]=0x8000;hal[2]=4096;hal[4]=message;hal[5]=stream;hal[9]=0x100;hal[12]=0x1000;

    // Qualified raw-basic output, control completion, and retained suppression tokens.
    {
        FastAudio f(reader,driver);auto* output=audioQueueCreate(16384);f.SetQueue(output);f.SetEarlyPlayback(true);
        const uint32_t start=e2?0x20065de4:0x2004ae3c,call=e2?0x20065e30:0x2004ae88;
        codeWord(start,0xb089b5f0);codeWord(start+4,0x1c04464d);codeWord(call-4,0x21056aa0);
        codeWord(call,e2?0xfa0ef020:0xfda0f021);codeWord(call+4,0x2800);
        write(stream,0xd2d20101);write(stream+4,0);write(stream+8,0xd2d20012);write(stream+12,44100);write(stream+16,0x10000);
        write(stream+20,0);write(stream+52,message);write(stream+56,message+128);
        for(unsigned i=0;i<1024;++i)write(0x8000+i*4,0x08000800+i);
        uint32_t ctl[16]{};ctl[1]=5;ctl[13]=0x9000;ctl[14]=call+5;uint32_t result[16]{};
        f.ControlHead(ctl,1,true);assert(f.ControlSuspended());f.ControlTail(ctl,result,2);assert(!f.ControlSuspended());
        uint32_t unrelated[16]{};unrelated[0]=1;
        f.ObserveHal(0x4318,unrelated);f.Handle(0x4318,unrelated,2,true);
        // One exact write establishes ownership before any earlier output.
        f.EarlyTransition(ready,pc,from,3);assert(!f.CaptureHostState().ledger.Records().front().previewed);
        bool skip=f.ObserveHal(0x4320,hal);assert(!skip);f.Handle(0x4320,hal,4,true,skip);f.TryEarly(4);
        uint32_t drained[16384];audioQueuePopChunk(output,drained,16384);
        f.EarlyTransition(ready,pc,from,5);assert(f.CaptureHostState().ledger.Records().front().previewed);
        assert(audioQueuePendingSamples(output)==1024);
        skip=f.ObserveHal(0x4320,hal);assert(skip);f.Handle(0x4320,hal,6,true,skip);assert(audioQueuePendingSamples(output)==1024);
        f.EarlyTransition(ready,pc,from,7);
        auto state=f.CaptureHostState();const auto token=f.BeginAudioCheckpoint();assert(!audioQueuePendingSamples(output));
        f.ObserveHal(0x4320,hal);f.RestoreAudioCheckpoint(token);f.EndAudioCheckpoint(token);
        assert(f.CaptureHostState().ledger.Records().front().sequence==state.ledger.Records().front().sequence);
        assert(!audioQueuePendingSamples(output));assert(f.ObserveHal(0x4320,hal));
        // Gain controls flush immediately and cannot resume before their return.
        f.EarlyTransition(ready,pc,from,8);ctl[1]=1;f.ControlHead(ctl,9,true);assert(!audioQueuePendingSamples(output));
        assert(f.ControlSuspended());write(object+40,512,2);f.ControlTail(ctl,result,10);assert(!f.ControlSuspended());
        assert(f.ObserveHal(0x4320,hal));f.EarlyTransition(ready,pc,from,11);
        assert(f.CaptureHostState().ledger.Records().front().previewed);
        ctl[1]=6;f.ControlHead(ctl,12,true);f.ControlTail(ctl,result,13);assert(f.ControlSuspended());
        assert(!audioQueuePendingSamples(output));assert(f.ObserveHal(0x4320,hal));
        ctl[1]=5;f.ControlHead(ctl,14,true);f.ControlTail(ctl,result,15);assert(!f.ControlSuspended());
        f.EarlyTransition(ready,pc,from,16);f.ObserveHal(0x4320,hal);f.EarlyTransition(ready,pc,from,17);
        assert(f.CaptureHostState().ledger.Records().front().previewed);f.Sleep();f.Sleep();assert(!audioQueuePendingSamples(output));
        f.Wake(18);assert(f.ObserveHal(0x4320,hal));assert(!audioQueuePendingSamples(output));
        // Extended or combined fields remain on HAL, including a supported start.
        f.Reset();ctl[1]=5;f.ControlHead(ctl,20,true);f.ControlTail(ctl,result,21);write(stream+20,0x10004);
        f.EarlyTransition(ready,pc,from,22);f.ObserveHal(0x4320,hal);f.EarlyTransition(ready,pc,from,23);
        assert(!f.CaptureHostState().ledger.Records().front().previewed);
        f.Reset();write(stream+20,0);ctl[14]+=2;f.ControlHead(ctl,24,true);f.ControlTail(ctl,result,25);
        f.EarlyTransition(ready,pc,from,26);f.ObserveHal(0x4320,hal);f.EarlyTransition(ready,pc,from,27);
        assert(!f.CaptureHostState().ledger.Records().front().previewed);
        f.Reset();ctl[14]=call+5;ctl[1]=5;f.ControlHead(ctl,28,true);result[0]=0x801;
        f.ControlTail(ctl,result,29);assert(f.Failed());result[0]=0;
        f.Reset();ctl[1]=1;f.ControlHead(ctl,30,true);const auto scope=f.BeginAudioCheckpoint();
        f.ControlTail(ctl,result,31);assert(!f.ControlSuspended());
        f.RestoreAudioCheckpoint(scope);f.EndAudioCheckpoint(scope);assert(f.ControlSuspended());
        f.ControlTail(ctl,result,32);assert(!f.ControlSuspended());
        f.Reset();f.ControlHead(ctl,28,false);assert(f.Failed());f.Sleep();f.Wake(29);assert(f.Failed());
        // A delayed preview token outlives the fallback timeout. Drain starts at retirement.
        {
            FastAudio g(reader,driver);g.SetQueue(output);g.SetEarlyPlayback(true);
            ctl[1]=5;ctl[14]=call+5;result[0]=0;g.ControlHead(ctl,1,true);g.ControlTail(ctl,result,2);
            g.EarlyTransition(ready,pc,from,3);g.ObserveHal(0x4320,hal,4);g.EarlyTransition(ready,pc,from,5);
            assert(g.CaptureHostState().ledger.HasPreviewed());g.NotifyUnderrun(6);
            assert(g.Failed() && !audioQueuePendingSamples(output) && g.CaptureHostState().ledger.HasPreviewed());
            g.Tick(1000000000);assert(g.Bypass());
            assert(g.ObserveHal(0x4320,hal,1000000000));g.Tick(1000000000);assert(g.Bypass());
            g.Tick(1250000000);assert(!g.Bypass() && g.Failed());
        }
        {
            FastAudio g(reader,driver);g.SetQueue(output);g.SetEarlyPlayback(true);
            ctl[1]=5;ctl[14]=call+5;result[0]=0;g.ControlHead(ctl,1,true);g.ControlTail(ctl,result,2);
            g.EarlyTransition(ready,pc,from,3);g.ObserveHal(0x4320,hal,4);g.EarlyTransition(ready,pc,from,5);
            uint32_t savedSample;std::memcpy(&savedSample,ram.data()+0x8000,4);
            write(0x8000,0x12345678);g.ObserveHal(0x4320,hal,1000000000);g.ValidateEarly(1000000000);
            assert(g.Failed());g.Tick(1000000001);assert(g.Bypass());
            g.Tick(1250000000);assert(!g.Bypass());write(0x8000,savedSample);
        }
        write(object+40,1024,2);write(stream,0);write(stream+16,0);write(stream+20,0);
        puts("PASS playback: exact suppression, control completion, stop/start, checkpoint, sleep, extended/combined and unknown-start refusal");
    }
    FastAudio p(reader,driver);
    p.EarlyTransition(ready,pc,from);assert(p.EarlyStatus().ready==1 && p.EarlyStatus().pending==1);
    hal[0]=1;p.ObserveHal(0x4320,hal);assert(p.EarlyStatus().pending==1 && p.EarlyStatus().contradictions==0);
    hal[0]=0;p.ObserveHal(0x4320,hal);assert(p.EarlyStatus().consumed==1 && p.EarlyStatus().pending==0);
    // Original public API doesn't expose both fields: synthetic guard acceptance.
    write(stream+16,1);write(stream+20,1);
    p.EarlyTransition(ready,pc,from);p.ObserveHal(0x4320,hal);
    assert(p.EarlyStatus().ready==2 && p.EarlyStatus().consumed==2);
    p.Reset();write(message+12,0);p.EarlyTransition(ready,pc,from);assert(p.EarlyStatus().pending==0);
    write(message+12,4100);p.EarlyTransition(ready,pc,from);assert(p.EarlyStatus().pending==0);
    write(message+12,4096);write(array,message+4);p.EarlyTransition(ready,pc,from);assert(!p.EarlyStatus().pending);
    write(array,message);write(queue+4,0x410010);p.EarlyTransition(ready,pc,from);assert(!p.EarlyStatus().pending);
    write(queue+4,0x10010);write(stack+4,0x12345679);p.EarlyTransition(ready,pc,from);assert(!p.EarlyStatus().pending);
    write(stack+4,e2?0x20066669:0x2004b6c1);
    // Canceled data retires through the writer's disposal, not a nonexistent HAL.
    p.EarlyTransition(ready,pc,from);p.ObserveHal(0x4324,hal);write(message+16,1);
    uint32_t dispose[16]{};dispose[0]=dispose[4]=message;dispose[5]=stream;
    p.EarlyTransition(dispose,e2?0x20085411:0x2006bb95,e2?0x200665c0:0x2004b618);
    assert(p.EarlyStatus().disposed==1 && !p.EarlyStatus().pending);
    write(message+16,0);p.Reset();
    p.EarlyTransition(ready,pc,from);write(0x8000,0x12345678);p.ObserveHal(0x4320,hal);
    assert(p.EarlyStatus().contradictions==1 && !p.EarlyStatus().pending);
    p.Reset();p.EarlyTransition(ready,pc,from);write(0x4004,object+128);p.ObserveHal(0x4320,hal);
    assert(p.EarlyStatus().contradictions==2 && p.EarlyStatus().pending==1);
    write(0x4004,object);
    p.Reset();codeWord(wrapper,0);p.EarlyTransition(ready,pc,from);assert(!p.EarlyStatus().pending);
    assert(p.EarlyStatus().rejected>=5);
    puts("PASS owned adapter: qualified context, combined fields, zero/oversize, membership failure, queue bounds, unrelated caller, cancellation disposal, PCM mutation and code changes");
}
int main(){exercise(FastAudio::Driver::tx);exercise(FastAudio::Driver::e2);}
