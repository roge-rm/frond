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
    FastAudio p(reader,driver);
    auto output=audioQueueCreate(8192);p.SetQueue(output);
    write(0x8000,0x10001000);
    p.EarlyTransition(ready,pc,from);
    p.Handle(0x4320,hal,0,true);
    assert(p.Active() && audioQueuePendingSamples(output)>0);
    auto metadata=p.CaptureHostState();const auto identity=metadata.ledger.Records().front();
    const auto beforeSave=audioQueueGetState(output);
    p.CaptureHostState();
    assert(audioQueueGetState(output).generation==beforeSave.generation && audioQueuePendingSamples(output)==beforeSave.pendingSamples);
    const auto checkpoint=p.BeginAudioCheckpoint();
    assert(p.OutputFenced() && !audioQueuePendingSamples(output));
    p.Handle(0x4320,hal,1,true);assert(!audioQueuePendingSamples(output));
    p.Reset();assert(!p.Active() && p.EarlyStatus().pending==0 && p.OutputFenced());
    p.RestoreAudioCheckpoint(checkpoint);
    assert(p.Active() && p.EarlyStatus().pending==1 && !audioQueuePendingSamples(output));
    auto restored=p.CaptureHostState().ledger.Records().front();
    assert(restored.generation==identity.generation && restored.sequence==identity.sequence && restored.incarnation==identity.incarnation);
    p.GuestLoaded(true);assert(p.EarlyStatus().pending==1);
    p.BeforeGuestReplacement();p.GuestReplacementFinished();assert(p.OutputFenced());
    p.EndAudioCheckpoint(checkpoint);
    assert(!p.OutputFenced() && p.EarlyStatus().pending==1 && !audioQueuePendingSamples(output));
    p.ObserveHal(0x4320,hal);assert(!p.EarlyStatus().pending);
    // Commit retains the progressed ledger, not the old snapshot.
    p.EarlyTransition(ready,pc,from);auto commit=p.BeginAudioCheckpoint();
    p.ObserveHal(0x4320,hal);p.EndAudioCheckpoint(commit);assert(!p.EarlyStatus().pending);
    // Nested rollback restores identity while output stays fenced by the outer scope.
    p.EarlyTransition(ready,pc,from);auto outer=p.BeginAudioCheckpoint();p.Reset();
    auto inner=p.BeginAudioCheckpoint();p.RestoreAudioCheckpoint(inner);p.EndAudioCheckpoint(inner);
    assert(p.OutputFenced() && !p.EarlyStatus().pending);
    p.RestoreAudioCheckpoint(outer);p.EndAudioCheckpoint(outer);
    assert(!p.OutputFenced() && p.EarlyStatus().pending==1);
    p.RestartOutput();assert(p.EarlyStatus().pending==1 && !audioQueuePendingSamples(output));
    // Intentional replacement starts a new timeline and never restores host PCM.
    p.BeforeGuestReplacement();p.GuestLoaded(false);p.GuestReplacementFinished();
    assert(!p.OutputFenced() && !p.EarlyStatus().pending && !p.Active());
    std::vector<uint64_t> scopes;
    for(unsigned i=0;i<8;++i)scopes.push_back(p.BeginAudioCheckpoint());
    bool refused=false;try {p.BeginAudioCheckpoint();}catch(const std::runtime_error&) {refused=true;}
    assert(refused && p.OutputFenced());
    for(auto it=scopes.rbegin();it!=scopes.rend();++it)p.EndAudioCheckpoint(*it);
    assert(!p.OutputFenced());
    puts("PASS host audio fence: output blocked before work, rollback identity, no PCM resurrection, commit, nested scopes, intentional replacement and bounds");
}
int main(){exercise(FastAudio::Driver::tx);exercise(FastAudio::Driver::e2);}
