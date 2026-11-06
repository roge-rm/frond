// Host-source input through the production codec and I2S FIFO, no test injection.
#define main existingAudioChecks
#include "pxa-i2s-audio.cpp"
#undef main
#include <cmath>
struct Source {
    unsigned pulled{}, fences{}, disarms{};
    int16_t level{1000};
    bool alternating{true};
    AudioInputRoute route{};
    AudioInputSource Hooks() { return {this,
        [](void *p, AudioInputRoute route, uint32_t n, uint32_t d) -> int16_t {
            auto &s=*static_cast<Source *>(p); assert(n && d); s.route=route;
            ++s.pulled; return s.alternating && (s.pulled & 1) ? -s.level : s.level;
        }, [](void *p,bool){++static_cast<Source *>(p)->fences;},
        [](void *p){++static_cast<Source *>(p)->disarms;}}; }
};
static uint32_t tick(PxaI2s *link) { pxaI2sPeriodic(link); controller=link; return rd(0x80); }
struct Fixture {
    PxaI2s *link=pxaI2sInit(nullptr,nullptr,nullptr);
    AK4534 *ak=ak4534Init(nullptr,link,nullptr);
    Source source;
    Fixture() {
        auto hooks=source.Hooks();ak4534SetInputSource(ak,&hooks);
        reg(0,0x83);reg(1,0x60);reg(4,0x6a);reg(7,0x0c);wr(0,0x7701);
        for(unsigned i=0;i<2081;++i)assert(tick(link)==0);
        assert(source.pulled==2081);
    }
    ~Fixture(){ak4534Destroy(ak);}
    unsigned gain(){AudioState state{link,ak};auto data=saveState(state);return chunkData(data,tg50CodecChunk)[37];}
    void run(unsigned frames){for(unsigned i=0;i<frames;++i)tick(link);}
};
static void contractChecks() {
    {
        Fixture f;f.source.alternating=false;f.run(2);reg(11,20);
        f.run(127);assert(f.gain()==16);f.run(1);assert(f.gain()==20);
        // PMMIC resets actual gain, preserving the last software register value.
        reg(0,0x81);assert(f.gain()==16);
        AudioState st{f.link,f.ak};auto data=saveState(st);assert(chunkData(data,tg50CodecChunk)[12]==20);
    }
    {
        Fixture f;f.source.alternating=false;f.source.level=1000;f.run(2);
        reg(9,0x20);reg(10,20);f.run(128);assert(f.gain()==16);
        reg(9,0);f.run(256);assert(f.gain()==16); // Pending recovery is cancelled.
    }
    {
        Fixture f;f.source.alternating=false;f.source.level=20000;f.run(2);
        reg(8,8);reg(9,0x20);reg(10,20); // ZTM128, WTM512.
        f.run(1);assert(f.gain()==16);f.source.level=1000;f.run(128);assert(f.gain()==15);
        f.run(400);assert(f.gain()==15); // Recovery waits from completed limiter.
        f.run(240);assert(f.gain()==16);
    }
    for(unsigned ltm=0;ltm<4;++ltm) {
        Fixture f;f.source.level=3000;reg(7,0x0d);reg(8,ltm);reg(9,0x30);
        f.run(1);assert(f.gain()==(ltm?15:14));
        if(ltm==2){f.run(1);assert(f.gain()==15);f.run(1);assert(f.gain()==14);}
        if(ltm==3){f.run(3);assert(f.gain()==15);f.run(1);assert(f.gain()==14);}
    }
    {
        Fixture f;f.source.level=1700;reg(7,0x0d);reg(9,0x30);
        f.run(1);assert(f.gain()==15); // Second half-frame sees level below threshold.
    }
    {
        Fixture f;f.source.level=1700;reg(7,0x0d);reg(9,0x20);
        f.run(1);assert(f.gain()==15); // Zero crossing limiter.
    }
    {
        Fixture f;f.source.level=0;f.source.alternating=false;f.run(32);
        f.source.level=1000;
        for(unsigned i=0;i<17;++i)assert(tick(f.link)==0);
        assert(int16_t(tick(f.link))>990); // Explicit17/fs signal delay.
        f.run(50000);assert(std::abs(int16_t(tick(f.link)))<2); // DC cancellation.
        const auto before=f.source.pulled;reg(0,0x82);assert(tick(f.link)==0);assert(f.source.pulled==before);
    }
    {
        Fixture f;f.source.level=32767;reg(7,0x0d);f.run(20);
        assert(std::abs(int16_t(tick(f.link)))>=32750);
        reg(4,0x69);assert(tick(f.link)==0); // I2S receiver/MSB codec mismatch.
        wr(4,1);assert(tick(f.link)!=0); // MSB justified DIF01.
        reg(4,0x68);assert(tick(f.link)!=0); // MSB justified DIF00 ADC.
        reg(4,0x6b);assert(tick(f.link)==0); // Reserved DIF11.
        const auto before=f.source.pulled;reg(1,0);pxaI2sAdvance(f.link,1000000);assert(f.source.pulled==before);
    }
}
int main() {
    auto *link=pxaI2sInit(nullptr,nullptr,nullptr);
    auto *ak=ak4534Init(nullptr,link,nullptr);
    Source source; auto hooks=source.Hooks(); ak4534SetInputSource(ak,&hooks);
    reg(0,0x83);reg(1,0x60);reg(4,0x6a);reg(7,0x0c);
    wr(0,0x7701);
    for(unsigned i=0;i<2081;++i) assert(tick(link)==0);
    assert(source.pulled==2081);
    const auto first=tick(link); assert(first>>16==0 && int16_t(first)<-990);
    assert(source.pulled==2082 && source.route==AudioInputRoute::internalMic);
    for(unsigned i=0;i<100;++i) assert(std::abs(int16_t(tick(link)))>=990);
    reg(7,0x08); tick(link);assert(source.route==AudioInputRoute::analogIn);
    reg(7,0x16); tick(link);assert(source.route==AudioInputRoute::externalMic);
    const auto before=source.pulled;reg(7,0x06);tick(link);assert(source.pulled==before);
    reg(7,0x0d);for(unsigned i=0;i<18;++i)tick(link);auto amplified=int16_t(tick(link));assert(std::abs(amplified)>9900);
    reg(11,0);for(unsigned i=0;i<130;++i)tick(link);
    assert(std::abs(int16_t(tick(link)))>3900 && std::abs(int16_t(tick(link)))<4100);
    reg(11,16);for(unsigned i=0;i<130;++i)tick(link);
    reg(7,0x0c);reg(9,0x21);reg(10,20); // Recovery gains up to REF20.
    for(unsigned i=0;i<700;++i)tick(link);
    AudioState state{link,ak};auto saved=saveState(state);
    auto *data=chunkData(saved,tg50CodecChunk);
    // Version2 appends ADC state after the intact version1 payload.
    constexpr unsigned adcOffset=36;
    assert(data[0]==2 && data[adcOffset+1]==20);
    ak4534FenceInput(ak,true);ak4534FenceInput(ak,true);
    const auto count=source.pulled;
    for(unsigned i=0;i<50;++i)tick(link);
    assert(source.pulled==count && source.fences==1);
    ak4534FenceInput(ak,false);assert(source.fences==1);
    ak4534FenceInput(ak,false);assert(source.fences==2);
    tick(link);assert(source.pulled==count+1);
    auto beforeBad=saveState(state);auto bad=beforeBad;
    chunkData(bad,tg50CodecChunk)[adcOffset+1]=72;SavestateLoader<ChunkType> loader;
    assert(!loader.Load(bad.data(),bad.size()*4,state));
    // Codec refuses atomically; controller has the same state in this candidate.
    assert(source.pulled==count+1);
    assert(saveState(state)==beforeBad);
    assert(loader.Load(saved.data(),saved.size()*4,state));
    auto *freshLink=pxaI2sInit(nullptr,nullptr,nullptr);
    auto *freshAk=ak4534Init(nullptr,freshLink,nullptr);AudioState fresh{freshLink,freshAk};
    assert(loader.Load(saved.data(),saved.size()*4,fresh));
    Source other;other.pulled=source.pulled;auto otherHooks=other.Hooks();ak4534SetInputSource(freshAk,&otherHooks);
    // Both continue identical guest filter/ALC state after explicit host rearm.
    for(unsigned i=0;i<200;++i)assert(tick(link)==tick(freshLink));
    reg(0,0x80);assert(tick(freshLink)==0); // ADC power off.
    ak4534Reset(ak);assert(source.disarms==1);
    ak4534Destroy(ak);ak4534Destroy(freshAk);
    contractChecks();
}
