// Digital PWM frequency, routing, mixing and phase continuation.
#include <cassert>
#include <vector>
#include "Tg50Audio.h"
int main() {
    Tg50Audio audio;
    audio.WritePwm({0x7f,0x90,0x120}); audio.SetMuxOutput(true); audio.Clock(true);
    std::vector<uint32_t> frames;
    auto emit = [&](uint32_t frame) { frames.push_back(frame); };
    audio.Advance(1000000000, emit); assert(frames.size() == 44100);
    unsigned rises{};
    for (unsigned i = 1; i < frames.size(); ++i) {
        assert(int16_t(frames[i]) == int16_t(frames[i] >> 16));
        if (int16_t(frames[i-1]) <= 0 && int16_t(frames[i]) > 0) ++rises;
    }
    assert(rises >= 198 && rises <= 200); // 3686400 / (64 * 289) = 199.30796 Hz
    const auto saved = audio.GetState(); frames.clear(); audio.Advance(1234567, emit);
    const auto expected = frames; frames.clear(); assert(audio.Load(saved));
    audio.Advance(1234567, emit); assert(frames == expected);
    auto invalid = saved; invalid.pwmPhase = uint64_t(64)*289*44100;
    assert(!audio.Load(invalid)); invalid = saved; invalid.dac[15] = 1; assert(!audio.Load(invalid));
    invalid = saved; invalid.dacCount = 17; assert(!audio.Load(invalid));
    invalid = saved; invalid.hostPhase = 1000000000; assert(!audio.Load(invalid));
    audio.Clock(false); frames.clear(); audio.Advance(1000000, emit);
    for (auto frame : frames) assert(!frame); // Abrupt stop selected by bit6.
    audio.Reset(); audio.SetMuxOutput(false); audio.WritePwm({0,1,2}); audio.Clock(true);
    frames.clear(); audio.Advance(1000000, emit); for (auto frame : frames) assert(!frame);
    audio.Reset();
    for (unsigned i = 0; i < 16; ++i) assert(audio.PushDac(0x80007fff));
    assert(!audio.PushDac(1)); frames.clear(); audio.Advance(1000000, emit);
    for (unsigned i = 0; i < 16; ++i) assert(frames[i] == 0x80007fff);
    for (unsigned i = 16; i < frames.size(); ++i) assert(!frames[i]);
    // The measured low-volume duty preserves frequency and reduces RMS.
    auto render = [&](uint32_t duty, uint32_t period) {
        audio.Reset(); audio.SetMuxOutput(true);
        audio.WritePwm({0x7f,duty,period}); audio.Clock(true);
        frames.clear(); audio.Advance(1000000000, emit);
        unsigned crossings{}; double energy{};
        for (unsigned i=1;i<frames.size();++i) {
            auto v=int16_t(frames[i]); energy += double(v)*v;
            if (int16_t(frames[i-1])<=0 && v>0) ++crossings;
        }
        return std::pair<unsigned,double>{crossings,energy};
    };
    auto loud=render(0x90,0x120), quiet=render(2,0x120);
    assert(loud.first==quiet.first && quiet.second < loud.second/20);
    auto high=render(14,28); assert(high.first>=1985 && high.first<=1987);
    for (auto duty : {0u,0x400u,1023u}) {
        render(duty,28); for(auto f:frames) assert(!f);
    }
    render(1,0); for(auto f:frames) assert(!f);
    // A pending update latches at the next PWM cycle and survives reload.
    audio.Reset(); audio.SetMuxOutput(true); audio.WritePwm({63,144,288}); audio.Clock(true);
    frames.clear(); audio.Advance(100000,emit);
    const auto active=audio.GetState().active;
    audio.WritePwm({63,2,28});
    assert(audio.GetState().active.period==active.period);
    auto mid=audio.GetState(); frames.clear(); audio.Advance(10000000,emit); auto continuation=frames;
    assert(audio.Load(mid)); frames.clear(); audio.Advance(10000000,emit); assert(frames==continuation);
    // Graceful shutdown can be cancelled without restarting the active cycle.
    audio.Clock(false); auto phase=audio.GetState().pwmPhase;
    audio.Clock(true); assert(audio.GetState().pwmPhase==phase);
    audio.Clock(false); frames.clear(); audio.Advance(10000000,emit);
    assert(!audio.GetState().running && !frames.back());
    audio.SetMuxOutput(false); audio.Clock(true); frames.clear(); audio.Advance(1000000,emit);
    for(auto f:frames) assert(!f);
    audio.SetMuxOutput(true); frames.clear(); audio.Advance(1000000,emit);
    bool any{}; for(auto f:frames) any |= bool(f); assert(any);
    // Distinctive stereo DAC samples mix into each channel with saturation.
    audio.Reset(); audio.SetMuxOutput(true); audio.WritePwm({0x7f,144,288}); audio.Clock(true);
    for(unsigned i=0;i<16;++i) assert(audio.PushDac(0x80007fff));
    frames.clear(); audio.Advance(362812,emit); assert(frames.size()==16);
    for(auto f:frames) assert(int16_t(f)==32767 && int16_t(f>>16)>-32768);
    const auto phaseBefore=audio.GetState().pwmPhase;
    audio.CodecBoundary(); assert(audio.GetState().dacCount==0);
    assert(audio.GetState().pwmPhase==phaseBefore);
}
