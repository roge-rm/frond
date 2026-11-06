// The TG50 on the ring-buffer queue through the actual SoC: a full queue drops its oldest frame,
// nothing is produced while PCM output is off, a codec boundary keeps frames and a load clears.
// A constant level counts as silence for the low-latency limits.
#include "tg50-audio-fixture.h"
#include "audio_queue.h"
#include "Tg50Audio.h"
#include <thread>
#include <cstdio>

static std::vector<uint32_t> mixed(bool pwm) {
    AudioMachine m; m.configure(); m.reg(0,0x80); m.reg(1,0x61); m.reg(4,0x6a); m.reg(6,0x10);
    m.wr(0x40400004,8); m.run(100);
    auto* queue=audioQueueCreate(128); m.soc.SetAudioQueue(queue); m.soc.SetPcmOutputEnabled(true);
    for(unsigned i=0;i<16;++i)m.wr(0x40400080,0);
    auto memory=m.soc.GetMemoryData();auto* raw=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(memory.data)+0x1000);
    for(unsigned i=0;i<32;++i)raw[i]=i<8?0:0x01000200+i;
    m.wr(0xa0000100,1);m.wr(0xa0000104,0xa0001000);m.wr(0xa0000108,0x40400080);m.wr(0xa000010c,0x9063c080);
    m.wr(0x40000100+DMA_CMR_I2S_TX*4,0x80);m.wr(0x40000200,0xa0000100);m.wr(0x40000000,0x80000000);
    if(pwm){m.wr(0x40e0000c,1u<<16);m.wr(0x40e00058,2);m.wr(0x40b00000,0x7f);
        m.wr(0x40b00004,144);m.wr(0x40b00008,288);m.wr(0x41300004,0x101);}
    m.soc.Run(100000,100000000);
    std::vector<uint32_t> output(audioQueuePendingSamples(queue));
    assert(audioQueuePopChunk(queue,output.data(),output.size())==output.size());return output;
}
int main() {
    // Drop-oldest and accounting on the queue itself.
    uint32_t frame{};
    auto* small=audioQueueCreate(2);
    audioQueuePush(small,1);audioQueuePush(small,2);audioQueuePush(small,3);
    assert(audioQueuePendingSamples(small)==2 && audioQueueAccounting(small).discarded==1);
    assert(audioQueuePopChunk(small,&frame,1)==1 && frame==2);
    assert(audioQueuePopChunk(small,&frame,1)==1 && frame==3);

    // Only a change counts as sound: silence or firmware idling at a constant level doesn't.
    auto* level=audioQueueCreate(4096);
    for(unsigned i=0;i<1000;++i)audioQueuePush(level,0);
    assert(audioQueueAccounting(level).nonSilent==0);
    for(unsigned i=0;i<1000;++i)audioQueuePush(level,0x01000100);
    assert(audioQueueAccounting(level).nonSilent==1);
    for(unsigned i=0;i<100;++i)audioQueuePush(level,i&1?0x01000100:0xff00ff00);
    assert(audioQueueAccounting(level).nonSilent==101);

    // Growth keeps the old segments in order while a consumer reads.
    auto* growth=audioQueueCreate(2);
    audioQueuePush(growth,1);audioQueuePush(growth,2);audioQueueEnsureCapacity(growth,4);
    audioQueuePush(growth,3);audioQueuePush(growth,4);audioQueueEnsureCapacity(growth,8);
    for(unsigned i=5;i<=8;++i)audioQueuePush(growth,i);
    std::thread segmented([&]{for(unsigned i=1;i<=8;++i){uint32_t value;assert(audioQueuePopChunk(growth,&value,1)==1 && value==i);}});
    audioQueueEnsureCapacity(growth,88201);segmented.join();assert(!audioQueuePendingSamples(growth));

    // A stalled consumer: the guest keeps running and the queue keeps its newest frames.
    AudioMachine m; m.configure(); auto* queue=audioQueueCreate(16);
    m.soc.SetAudioQueue(queue); m.soc.SetPcmOutputEnabled(true);
    m.run(); const auto time=m.soc.GetTime();
    const auto first=audioQueueAccounting(queue);
    assert(audioQueuePendingSamples(queue)==16 && first.discarded>0);
    m.run(); assert(m.soc.GetTime()>time && audioQueuePendingSamples(queue)==16);
    auto acc=audioQueueAccounting(queue);
    assert(acc.committed>first.committed && acc.discarded>first.discarded);
    assert(acc.committed==acc.consumed+acc.discarded+audioQueuePendingSamples(queue));

    // A codec boundary keeps the frames already mixed.
    auto generation=audioQueueGetState(queue).generation;
    m.reg(3,0x01);
    assert(audioQueuePendingSamples(queue)==16 && audioQueueGetState(queue).generation==generation);

    // A load clears the queue and starts a new generation.
    const auto saved=m.save();
    const auto beforeLoad=audioQueueAccounting(queue);
    assert(m.soc.Load(saved.size(), const_cast<uint8_t*>(saved.data())));
    assert(!audioQueuePendingSamples(queue) && audioQueueGetState(queue).generation!=generation);
    assert(audioQueueAccounting(queue).discarded-beforeLoad.discarded==16);

    // Off detaches the producer, with normal clocks still progressing.
    m.soc.SetPcmOutputEnabled(false); const auto offTime=m.soc.GetTime();
    const auto offCommitted=audioQueueAccounting(queue).committed;
    m.run(); assert(m.soc.GetTime()>offTime && !audioQueuePendingSamples(queue));
    assert(audioQueueAccounting(queue).committed==offCommitted);
    m.soc.SetPcmOutputEnabled(true); m.run(); assert(audioQueuePendingSamples(queue)>0);

    // At 10 cycles/s one cycle is 100 ms of guest time; injected runs account for it without
    // dividing by zero, and the 4410 frames it mixes overrun a 2-frame queue.
    AudioMachine low; low.configure(); auto* lowQueue=audioQueueCreate(2);
    low.soc.SetAudioQueue(lowQueue); low.soc.SetPcmOutputEnabled(true);
    assert(low.soc.Run(1,10)==1);
    low.soc.SetPcmOutputEnabled(false);
    low.soc.ResetInjectedTimeNsec(); assert(!low.soc.ExecuteInjected(1,10));
    assert(low.soc.GetInjectedTimeNsec()==100000000);
    low.soc.ResetInjectedTimeNsec(); assert(!low.soc.RunToPaceSyscall(0xa000,1,10));
    assert(low.soc.GetInjectedTimeNsec()==100000000);
    low.soc.SetPcmOutputEnabled(true);assert(low.soc.Run(1,10)==1);
    assert(audioQueuePendingSamples(lowQueue)==2 && audioQueueAccounting(lowQueue).discarded>4000);
    assert(!low.soc.Run(1,0));
    // Late rollback and destruction must not dereference a destroyed SoC.
    auto owned = std::make_unique<AudioMachine>(); owned->configure(); owned->soc.SetAudioQueue(queue);
    auto checkpoint = std::make_unique<SoC::InputCheckpoint>(owned->soc);
    owned.reset(); assert(!checkpoint->Restore(0, nullptr)); checkpoint.reset();
    auto ordinary=mixed(false), overlap=mixed(true); assert(ordinary.size()==overlap.size());
    Tg50Audio pwm; pwm.SetMuxOutput(true);pwm.WritePwm({0x7f,144,288});pwm.Clock(true);
    std::vector<uint32_t> expected; pwm.Advance(1000000ull,[&](uint32_t value){expected.push_back(value);});
    // Run(100000) at 100 MHz is 1 ms of guest time.
    assert(expected.size()==overlap.size()); unsigned nonzeroPwm{},nonzeroDac{};
    for(unsigned i=0;i<overlap.size();++i){
        assert(int16_t(overlap[i])-int16_t(ordinary[i])==int16_t(expected[i]));
        assert(int16_t(overlap[i]>>16)-int16_t(ordinary[i]>>16)==int16_t(expected[i]>>16));
        nonzeroPwm+=expected[i]!=0;nonzeroDac+=ordinary[i]!=0;
    }
    assert(nonzeroPwm && nonzeroDac);
    std::puts("TG50 ring-buffer queue: drop-oldest, PCM off, codec boundary, load clear PASS");
}
