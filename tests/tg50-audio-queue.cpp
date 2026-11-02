// Final PCM admission and live FIFO checkpoint ownership through the actual SoC.
#include "tg50-audio-fixture.h"
#include "audio_queue.h"
#include "SonyDacHost.h"
#include <atomic>
#include <thread>
#include <cstdio>
#include <future>

struct Clock { unsigned ticks{}; uint32_t DispatchTicks(uint32_t task, uint32_t count) {
    if (task == SCHEDULER_TASK_I2S) ticks += count;
    return 1;
} };

static std::vector<uint32_t> mixed(bool pwm) {
    AudioMachine m; m.configure(); m.reg(0,0x80); m.reg(1,0x61); m.reg(4,0x6a); m.reg(6,0x10);
    m.wr(0x40400004,8); m.run(100);
    auto* queue=audioQueueCreate(128); m.soc.SetAudioQueue(queue); m.soc.SetPcmOutputEnabled(true);
    audioQueueSetPreviewAllowed(queue,true);
    for(unsigned i=0;i<16;++i)m.wr(0x40400080,0);
    auto* host=m.soc.AttachSonyTransport();assert(host->Start());
    auto memory=m.soc.GetMemoryData();auto* raw=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(memory.data)+0x1000);
    for(unsigned i=0;i<32;++i)raw[i]=i<8?0:0x01000200+i;
    assert(host->Ledger().Submit({1,101,0x1234,reinterpret_cast<uintptr_t>(raw),32},raw,true));
    m.wr(0xa0000100,1);m.wr(0xa0000104,0xa0001000);m.wr(0xa0000108,0x40400080);m.wr(0xa000010c,0x9063c080);
    m.wr(0x40000100+DMA_CMR_I2S_TX*4,0x80);m.wr(0x40000200,0xa0000100);m.wr(0x40000000,0x80000000);
    if(pwm){m.wr(0x40e0000c,1u<<16);m.wr(0x40e00058,2);m.wr(0x40b00000,0x7f);
        m.wr(0x40b00004,144);m.wr(0x40b00008,288);m.wr(0x41300004,0x101);}
    m.soc.Run(40000,100000000);
    std::vector<uint32_t> output(audioQueuePendingSamples(queue));
    assert(audioQueuePopChunk(queue,output.data(),output.size())==output.size());return output;
}
static uint32_t* payload(std::vector<uint8_t>& data, uint32_t type) {
    auto* words=reinterpret_cast<uint32_t*>(data.data());
    size_t offset=1+2*words[0];
    for (unsigned i=0;i<words[0];++i) {
        if (words[1+2*i]==type) return words+offset+1;
        offset+=words[2+2*i];
    }
    assert(false); return nullptr;
}
struct LoadConsumer {
    std::promise<void> ready, done;
    std::future<void> doneFuture=done.get_future();
    bool armed{true};
    static void Fence(void* p,bool fenced) {
        auto& gate=*static_cast<LoadConsumer*>(p);
        if (fenced && gate.armed) {
            gate.armed=false; gate.ready.set_value(); gate.doneFuture.wait();
        }
    }
};
struct Reentry {
    AudioMachine& machine; std::vector<uint8_t>& state; AudioQueue* other; SoC::InputCheckpoint* checkpoint; unsigned calls{};
    static void Callback(ArmCpu*,void* opaque) {
        auto& x=*static_cast<Reentry*>(opaque);auto& soc=x.machine.soc;++x.calls;
        const auto time=soc.GetTime();
        assert(!soc.Load(x.state.size(),x.state.data()));
        assert(!x.checkpoint->Restore(x.state.size(),x.state.data()));
        assert(!soc.Run(100,100000000) && !soc.ExecuteInjected(100,100000000));
        assert(!soc.RunToPaceSyscall(0xa000,100,100000000));
        assert(!soc.RunToNativePc(0xa0004000,0,100,100000000));
        soc.Reset();soc.HardReset();soc.SetAudioQueue(x.other);soc.SetPcmOutputEnabled(false);
        soc.SetAudioOutputFenced(true);assert(!soc.AttachSonyTransport());
        assert(soc.PcmOutputEnabled() && soc.GetTime()==time);
    }
};
int main() {
    AudioMachine callback;callback.configure();auto state=callback.save();
    auto* callbackQueue=audioQueueCreate(16);auto* other=audioQueueCreate(16);
    callback.soc.SetAudioQueue(callbackQueue);callback.soc.SetPcmOutputEnabled(true);
    SoC::InputCheckpoint callbackCheckpoint(callback.soc);
    Reentry reentry{callback,state,other,&callbackCheckpoint};auto* cpu=callback.soc.GetCpu();
    assert(cpuAcquireNativeSession(cpu));
    assert(cpuRegisterNativeCallback(cpu,0xa0003020,Reentry::Callback,&reentry));
    assert(cpuStartNativeCall(cpu,0xa0003020));assert(callback.soc.ExecuteInjected(100,100000000));
    cpuEndNativeCall(cpu);cpuUnregisterNativeCallback(cpu,0xa0003020);assert(reentry.calls==1);
    callbackCheckpoint.Finish();
    callback.loop();callback.run();assert(audioQueuePendingSamples(callbackQueue)>0 && !audioQueuePendingSamples(other));
    assert(callback.soc.Load(state.size(),state.data())); // Ownership is released after the outer run.
    Clock clock; Scheduler<Clock> scheduler(clock);
    scheduler.ScheduleTask(SCHEDULER_TASK_I2S, 20000, 1);
    uint64_t ticks{};
    assert(scheduler.AudioTicksWithin(20000, ticks) && ticks == 1);
    assert(scheduler.AudioTicksWithin(2000000000, ticks) && ticks == 100000);
    assert((ticks * 441 + 499) / 500 == 88200);
    scheduler.Advance(50000, 1000000000); assert(clock.ticks == 2);
    assert(scheduler.AudioTicksWithin(10000, ticks) && ticks == 1);
    assert(scheduler.CyclesToNextUpdate(UINT64_MAX) > 0);
    assert(scheduler.AudioTicksWithin(2000010000, ticks) && ticks == 100001);
    assert((ticks*441+499)/500 == 88201);
    scheduler.ScheduleTask(SCHEDULER_TASK_I2S,20000,2);
    assert(!scheduler.AudioTicksWithin(20000,ticks));
    auto* queue = audioQueueCreate(2);
    audioQueueProtect(queue, true);
    audioQueuePush(queue, 0x11112222); audioQueuePush(queue, 0x33334444);
    assert(!audioQueueCanProduce(queue, 1));
    const auto generation = audioQueueGetState(queue).generation;
    uint32_t frame{};
    audioQueueRetain(queue, true); audioQueueRetain(queue, true);
    audioQueueClear(queue); assert(audioQueuePendingSamples(queue) == 2);
    assert(audioQueuePopChunkIfGeneration(queue, &frame, 1, generation) == 1 && frame == 0x11112222);
    audioQueueRetain(queue, false); audioQueueClear(queue);
    audioQueueRetain(queue, false); assert(audioQueuePendingSamples(queue) == 1);
    assert(audioQueuePopChunk(queue, &frame, 1) == 1 && frame == 0x33334444);

    auto* growth=audioQueueCreate(2);audioQueueProtect(growth,true);
    audioQueuePush(growth,1);audioQueuePush(growth,2);audioQueueEnsureCapacity(growth,4);
    audioQueuePush(growth,3);audioQueuePush(growth,4);audioQueueEnsureCapacity(growth,8);
    for(unsigned i=5;i<=8;++i)audioQueuePush(growth,i);
    std::thread segmented([&]{for(unsigned i=1;i<=8;++i){uint32_t value;assert(audioQueuePopChunk(growth,&value,1)==1 && value==i);}});
    audioQueueEnsureCapacity(growth,88201);segmented.join();assert(!audioQueuePendingSamples(growth));
    AudioMachine m; m.configure(); m.soc.SetAudioQueue(queue); m.soc.SetPcmOutputEnabled(true);
    // A stalled consumer stops the entire guest before another mixed frame is committed.
    m.run(); assert(audioQueuePendingSamples(queue) == 2 && m.soc.AudioAdmissionBlocked());
    auto stopped = m.save(); const auto time = m.soc.GetTime();
    m.run(); assert(m.soc.GetTime() == time && m.save() == stopped);
    assert(!m.soc.ExecuteInjected(100,100000000));
    assert(!m.soc.RunToPaceSyscall(0xa000,100,100000000));
    assert(!m.soc.RunToNativePc(0xa0003004,0,100,100000000));
    assert(m.soc.GetTime()==time && m.save()==stopped);
    assert(audioQueuePopChunk(queue, &frame, 1) == 1);
    m.run(); assert(m.soc.GetTime() > time && audioQueuePendingSamples(queue) == 2);
    audioQueueDiscard(queue);
    audioQueuePush(queue, 0x12345678); audioQueuePush(queue, 0x89abcdef);
    const auto saved = m.save();
    const auto liveGeneration = audioQueueGetState(queue).generation;
    {
        SoC::InputCheckpoint outer(m.soc);
        assert(audioQueueGetState(queue).generation == liveGeneration);
        // A real consumer takes the live prefix while output is detached.
        std::thread consumer([&] {
            assert(audioQueuePopChunkIfGeneration(queue, &frame, 1, liveGeneration) == 1);
            assert(frame == 0x12345678);
        }); consumer.join();
        {
            SoC::InputCheckpoint inner(m.soc);
            m.run(); m.soc.Reset();
            assert(inner.Restore(saved.size(), const_cast<uint8_t*>(saved.data())));
        }
        assert(outer.Restore(saved.size(), const_cast<uint8_t*>(saved.data())));
    }
    assert(audioQueueGetState(queue).generation == liveGeneration);
    assert(audioQueuePopChunk(queue, &frame, 1) == 1 && frame == 0x89abcdef);
    assert(!audioQueuePendingSamples(queue));
    audioQueuePush(queue, 0x100); audioQueuePush(queue, 0x101);
    auto refused=saved; payload(refused,0x1d0)[36]=72;
    LoadConsumer gate; auto ready=gate.ready.get_future();
    Ak4534InputSource source{&gate,[](void*,Ak4534InputRoute,uint32_t,uint32_t)->int16_t{return 0;},
        LoadConsumer::Fence,[](void*){}};
    // Arm after attaching: the pop occurs inside the actual Load checkpoint fence.
    gate.armed=false; m.soc.SetMicrophoneSource(&source); gate.armed=true;
    std::thread duringLoad([&] {
        ready.wait(); assert(audioQueuePopChunk(queue,&frame,1)==1 && frame==0x100);
        gate.done.set_value();
    });
    assert(!m.soc.Load(refused.size(), refused.data())); duringLoad.join();
    m.soc.SetMicrophoneSource(nullptr);
    assert(audioQueueGetState(queue).generation == liveGeneration);
    assert(audioQueuePopChunk(queue,&frame,1)==1 && frame==0x101);
    assert(!audioQueuePendingSamples(queue));
    audioQueuePush(queue,0x100); audioQueuePush(queue,0x101);
    const auto before = audioQueueAccounting(queue);
    assert(m.soc.Load(saved.size(), const_cast<uint8_t*>(saved.data())));
    const auto after = audioQueueAccounting(queue);
    assert(after.discarded - before.discarded == 2);
    assert(!audioQueuePendingSamples(queue));
    // Off detaches the producer, with normal clocks still progressing.
    m.soc.SetPcmOutputEnabled(false); const auto offTime = m.soc.GetTime();
    m.run(); assert(m.soc.GetTime() > offTime && !audioQueuePendingSamples(queue));
    AudioMachine low; low.configure(); auto* lowQueue=audioQueueCreate(2);
    low.soc.SetAudioQueue(lowQueue); low.soc.SetPcmOutputEnabled(true);
    assert(low.soc.Run(1,10)==1);
    low.soc.SetPcmOutputEnabled(false);
    low.soc.ResetInjectedTimeNsec(); assert(!low.soc.ExecuteInjected(1,10));
    assert(low.soc.GetInjectedTimeNsec()==100000000);
    low.soc.ResetInjectedTimeNsec(); assert(!low.soc.RunToPaceSyscall(0xa000,1,10));
    assert(low.soc.GetInjectedTimeNsec()==100000000);
    low.soc.SetPcmOutputEnabled(true);assert(low.soc.Run(1,10)==1);
    assert(audioQueuePendingSamples(lowQueue)>2 && audioQueuePendingSamples(lowQueue)<=88201);
    auto valid=low.save(); auto debt=valid; auto* timing=payload(debt,0x10000);
    const uint64_t acc=uint64_t(timing[65]) | (uint64_t(timing[66])<<32);
    const uint64_t owing=acc+20000; timing[65]=uint32_t(owing); timing[66]=owing>>32;
    assert(low.soc.Load(debt.size(),debt.data()));
    assert(low.soc.Run(1,100000000)==1);
    auto invalid=valid; payload(invalid,0x10000)[3*8]=2;
    const auto beforeInvalid=low.save(); const auto invalidTime=low.soc.GetTime();
    assert(!low.soc.Load(invalid.size(),invalid.data()));
    assert(low.soc.GetTime()==invalidTime && low.save()==beforeInvalid);
    // Late rollback and destruction must not dereference a destroyed SoC.
    auto owned = std::make_unique<AudioMachine>(); owned->configure(); owned->soc.SetAudioQueue(queue);
    auto checkpoint = std::make_unique<SoC::InputCheckpoint>(owned->soc);
    owned.reset(); assert(!checkpoint->Restore(0, nullptr)); checkpoint.reset();
    // Internal host transport on actual SoC, with no firmware/ABI adapter.
    AudioMachine audio; audio.configure(); audio.reg(0,0x80); audio.reg(1,0x61);
    audio.reg(4,0x6a); audio.reg(6,0x10); audio.wr(0x40400004,8); audio.run(100);
    auto* output = audioQueueCreate(256); audio.soc.SetAudioQueue(output);
    audio.soc.SetPcmOutputEnabled(true); audioQueueSetPreviewAllowed(output, true);
    for (unsigned i=0;i<16;++i) audio.wr(0x40400080,0);
    auto* host = audio.soc.AttachSonyTransport(); assert(host && host->Start());
    auto memory = audio.soc.GetMemoryData();
    auto* raw = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(memory.data)+0x1000);
    for (unsigned i=0;i<32;++i) raw[i] = i<8 ? 0 : 0x01000200 + i;
    const auto id = host->Ledger().Submit({1,101,0x1234,reinterpret_cast<uintptr_t>(raw),32},raw,true);
    assert(id);
    audio.wr(0xa0000100,1); audio.wr(0xa0000104,0xa0001000);
    audio.wr(0xa0000108,0x40400080); audio.wr(0xa000010c,0x9063c080);
    audio.wr(0x40000100+DMA_CMR_I2S_TX*4,0x80);
    audio.wr(0x40000200,0xa0000100); audio.wr(0x40000000,0x80000000);
    audio.soc.Run(20000,100000000);
    unsigned previewed{};
    for (const auto& g:host->Ledger().Generations())
        for (unsigned i=0;i<g.identity.frames;++i) previewed += bool(g.delivered[i]&1);
    assert(previewed);
    auto portable = audio.save(); const auto transport = host->Capture();
    auto sameHost=[&] {
        auto restored = host->Capture();
        assert(restored.fifoCount==transport.fifoCount && restored.dacCount==transport.dacCount);
        assert(restored.ledger.Fenced()==transport.ledger.Fenced());
        assert(restored.partial.sum[0]==transport.partial.sum[0] && restored.partial.sum[1]==transport.partial.sum[1]);
        for (unsigned i=0;i<transport.fifoCount;++i) {
            assert(restored.fifo[i].generation==transport.fifo[i].generation);
            assert(restored.fifo[i].frame==transport.fifo[i].frame && restored.fifo[i].matches==transport.fifo[i].matches);
        }
        assert(restored.scoped.generation==transport.scoped.generation && restored.scoped.frame==transport.scoped.frame && restored.scoped.matches==transport.scoped.matches);
        assert(restored.serial.generation==transport.serial.generation && restored.serial.frame==transport.serial.frame && restored.serial.matches==transport.serial.matches);
        assert(restored.inWrite==transport.inWrite);
        for(unsigned i=0;i<16;++i) assert(restored.dac[i].portable==transport.dac[i].portable && restored.dac[i].host==transport.dac[i].host);
        for (unsigned i=0;i<SonyDacPreview::maxGenerations;++i) {
            const auto& a=transport.ledger.Generations()[i]; const auto& b=restored.ledger.Generations()[i];
            assert(a.id==b.id && a.references==b.references && a.delivered==b.delivered && a.outstanding==b.outstanding);
            assert(a.identity.node==b.identity.node && a.identity.token==b.identity.token && a.identity.context==b.identity.context && a.identity.backing==b.identity.backing && a.identity.frames==b.identity.frames);
            assert(a.preview==b.preview && a.previewNext==b.previewNext && a.dmaNext==b.dmaNext &&
                a.completed==b.completed && a.cancelled==b.cancelled && a.transportDiscarded==b.transportDiscarded &&
                a.raw==b.raw && a.read==b.read);
        }
        auto x=transport.ledger,y=host->Capture().ledger;
        uint32_t zero{}; const SonyDacPreview::Identity next{2,102,0x1234,reinterpret_cast<uintptr_t>(&zero),1};
        assert(x.Submit(next,&zero,false)==y.Submit(next,&zero,false));
    };
    {
        SoC::InputCheckpoint outer(audio.soc);
        {
            SoC::InputCheckpoint inner(audio.soc); audio.run();
            assert(inner.Restore(portable.size(),portable.data()));sameHost();
            assert(inner.Restore(portable.size(),portable.data()));sameHost();
        }
        assert(outer.Restore(portable.size(),portable.data()));sameHost();
    }
    sameHost(); assert(audio.save()==portable);
    audioQueueDiscard(output);audioQueuePush(output,0x11223344);audioQueuePush(output,0x55667788);
    auto badHost=portable;payload(badHost,0x1d0)[36]=72;
    LoadConsumer hostGate;auto hostReady=hostGate.ready.get_future();
    Ak4534InputSource hostSource{&hostGate,[](void*,Ak4534InputRoute,uint32_t,uint32_t)->int16_t{return 0;},
        LoadConsumer::Fence,[](void*){}};
    hostGate.armed=false;audio.soc.SetMicrophoneSource(&hostSource);hostGate.armed=true;
    std::thread hostConsumer([&]{hostReady.wait();uint32_t value;
        assert(audioQueuePopChunk(output,&value,1)==1 && value==0x11223344);hostGate.done.set_value();});
    const auto hostGeneration=audioQueueGetState(output).generation;
    assert(!audio.soc.Load(badHost.size(),badHost.data()));hostConsumer.join();sameHost();
    assert(audioQueueGetState(output).generation==hostGeneration);
    assert(audioQueuePopChunk(output,&frame,1)==1 && frame==0x55667788);
    assert(!audioQueuePendingSamples(output));audio.soc.SetMicrophoneSource(nullptr);
    // Same-timeline mute detaches future preview while retaining existing suppression.
    audioQueueSetPreviewAllowed(output,false); audio.soc.Run(5000,100000000);
    assert(host->Ledger().Fenced());
    const auto committed=audioQueueGetState(output);
    audio.reg(5,7<<5); // The codec boundary retains already mixed output.
    assert(audioQueueGetState(output).generation==committed.generation &&
        audioQueuePendingSamples(output)==committed.pendingSamples);
    const auto beforeSleep=host->Capture(); const auto accounting=audioQueueAccounting(output);
    audio.sleep(); assert(host->Ledger().Fenced());
    const auto sleeping=host->Capture();
    assert(sleeping.fifoCount==beforeSleep.fifoCount);
    for(unsigned i=0;i<SonyDacPreview::maxGenerations;++i) {
        const auto& a=beforeSleep.ledger.Generations()[i];const auto& b=sleeping.ledger.Generations()[i];
        assert(a.id==b.id && a.delivered==b.delivered && a.references==b.references);
    }
    assert(!audioQueuePendingSamples(output));
    assert(audioQueueAccounting(output).discarded-accounting.discarded==committed.pendingSamples);
    assert(audio.soc.Load(portable.size(),portable.data()));
    assert(host->Ledger().Quiescent()); // Accepted replacement starts a new host timeline.
    auto ordinary=mixed(false), overlap=mixed(true); assert(ordinary.size()==overlap.size());
    Tg50Audio pwm; pwm.SetMuxOutput(true);pwm.WritePwm({0x7f,144,288});pwm.Clock(true);
    std::vector<uint32_t> expected; pwm.Advance(400000,[&](uint32_t value){expected.push_back(value);});
    assert(expected.size()==overlap.size()); unsigned nonzeroPwm{},nonzeroDac{};
    for(unsigned i=0;i<overlap.size();++i){
        assert(int16_t(overlap[i])-int16_t(ordinary[i])==int16_t(expected[i]));
        assert(int16_t(overlap[i]>>16)-int16_t(ordinary[i]>>16)==int16_t(expected[i]>>16));
        nonzeroPwm+=expected[i]!=0;nonzeroDac+=ordinary[i]!=0;
    }
    assert(nonzeroPwm && nonzeroDac);
    std::puts("TG50 protected admission/live FIFO checkpoint PASS");
}
