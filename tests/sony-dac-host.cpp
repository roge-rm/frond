// Synthetic RAM, actual DMA/I2S/codec and the ordinary PWM mixer cadence.
#define main existingDmaFixture
#include "pxa-i2s-dma.cpp"
#undef main
#include "SonyDacHost.h"
#include "savestate/savestateAll.h"
#include "Tg50Chunks.h"
static I2cDeviceActionF bus;
static void *busContext;
bool pxaI2cDeviceAdd(PxaI2c *,I2cDeviceActionF f,void *context) {
    bus=f; busContext=context; return true;
}
static void reg(unsigned address,unsigned value) {
    bus(busContext,i2cStart,0); assert(bus(busContext,i2cTx,0x20));
    assert(bus(busContext,i2cTx,address)); assert(bus(busContext,i2cTx,value));
    bus(busContext,i2cStop,0);
}
struct Portable {
    PxaI2s *link;
    AK4534 *codec;
    template <typename T> void Save(T &save) { pxaI2sSave(link,save); ak4534Save(codec,save); }
    template <typename T> void Load(T &load) { pxaI2sLoad(link,load); ak4534Load(codec,load); }
};
int main() {
    regions.clear();
    Reschedule reschedule{[](void *,uint32_t){},nullptr};
    auto *dma=pxaDmaInit(nullptr,reschedule,nullptr);
    auto *link=pxaI2sInit(nullptr,nullptr,dma);
    auto *codec=ak4534Init(nullptr,link,nullptr);
    Tg50Audio mixer, pwm;
    for (auto *a : {&mixer,&pwm}) {
        a->SetMuxOutput(true); a->WritePwm({63,144,288}); a->Clock(true);
    }
    ak4534SetOutput(codec,&mixer,[](void *p,uint32_t word) {
        assert(static_cast<Tg50Audio *>(p)->PushDac(word));
    },[](void *p) { static_cast<Tg50Audio *>(p)->CodecBoundary(); });
    reg(0,0x80); reg(1,0x61); reg(4,0x6a); reg(6,0x10);
    wr(0x40400004,8); wr(0x40400000,0x7701);
    {
        SonyDacHost host(dma,link,codec,mixer,nullptr,[](void *,uint32_t address,unsigned width) -> uintptr_t {
            if (!memIsBufferRange(nullptr,address,width,false)) return 0;
            return reinterpret_cast<uintptr_t>(ram) + address-0x1000;
        });
        assert(host.Start());
        for (unsigned i=0;i<16;++i) ram[64+i]=uint16_t(i+100) | (uint32_t(uint16_t(-int(i)-200)) << 16);
        const auto generation=host.Ledger().Submit({1,101,0x1234,reinterpret_cast<uintptr_t>(ram+64),16},ram+64,true);
        assert(generation);
        std::vector<uint32_t> early, base;
        for (unsigned i=0;i<16;++i) {
            mixer.AdvanceHost(22676,[&](auto word) { early.push_back(word); },
                [&](auto word,bool present) { return host.Select(word,present); });
            pwm.Advance(22676,[&](auto word) { base.push_back(word); });
        }
        assert(early.size()==16 && base.size()==16);
        for (unsigned i=0;i<16;++i) {
            assert(int16_t(early[i])-int16_t(ram[64+i]) == int16_t(base[i]));
            assert(int16_t(early[i]>>16)-int16_t(ram[64+i]>>16) == int16_t(base[i]>>16));
        }
        // A rate change after preview retains FIFO provenance; each heard input
        // contributes zero to the host resampler, preserving actual portable PCM.
        reg(5,7<<5);
        ram[0]=1; ram[1]=0x1100; ram[2]=0x40400080; ram[3]=0x9063c040;
        wr(0x40000100+DMA_CMR_I2S_TX*4,0x80);
        wr(0x40000200,0x1000); wr(0x40000000,0x80000000);
        auto run=[&](unsigned quanta) {
            std::vector<uint32_t> result;
            for (unsigned i=0;i<quanta;++i) {
                pxaDmaPeriodic(dma); pxaI2sAdvance(link,20000);
                mixer.AdvanceHost(20000,[&](auto word) { result.push_back(word); },
                    [&](auto word,bool present) { return host.Select(word,present); });
            }
            return result;
        };
        auto first=run(20);
        assert(rd(0x4000020c)%8192==0);
        Portable portable{link,codec}; Savestate<ChunkType> save; assert(save.Save(portable));
        const auto hostSaved=host.Capture();
        const auto mixerSaved=mixer.GetState();
        auto expected=run(30);
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(save.GetBuffer(),save.GetSize(),portable));
        assert(mixer.Load(mixerSaved)); assert(host.Restore(hostSaved));
        assert(run(30)==expected);
        // The DMA channel is already fully filled before this checkpoint, so
        // no device/channel rollback is skipped by the fixture above.
        auto rest=run(75);
        std::vector<uint32_t> actual=first;
        actual.insert(actual.end(),expected.begin(),expected.end());
        actual.insert(actual.end(),rest.begin(),rest.end());
        std::vector<uint32_t> reference;
        pwm.Advance(2500000,[&](auto word) { reference.push_back(word); });
        assert(reference.size()==actual.size());
        for (unsigned i=0;i<88;++i) assert(actual[i]==reference[i]);
        bool repeated{};
        for (unsigned i=95;i<actual.size();++i) repeated |= actual[i]!=reference[i];
        assert(repeated); // Underrun repeats are new, unpreviewed occurrences.
        host.Ledger().Complete(1,101); assert(host.Ledger().Quiescent());
    }
    ak4534Destroy(codec);
}
