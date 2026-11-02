#include <cassert>
#include <cstdio>
#include <vector>
#include "SonyDacPreview.h"
#include "Tg50Audio.h"

using Preview = SonyDacPreview;
static const uint32_t gain[2] = {65536, 65536};
static Preview::Identity identity(unsigned node, uintptr_t source, unsigned frames) {
    return {node, node + 100, 0x1234, source, frames};
}

static void lifecycle() {
    Preview p;
    assert(p.Start());
    uint32_t words[] = {0x10002000, 0x30004000, 0x50006000};
    auto id = identity(1, 0x1000, 3);
    auto gen = p.Submit(id, words, true); assert(gen);
    assert(p.Render(0, {}, gain) == words[0]);
    auto t0 = p.Dma(0x1000, 4, words[0]); assert(t0.generation == gen);
    // Pause prevents new preview, retaining suppression for the word already heard.
    p.Fence();
    assert(p.Render(words[0], t0, gain) == 0);
    auto t1 = p.Dma(0x1004, 4, words[1]);
    auto t2 = p.Dma(0x1008, 4, words[2]);
    p.Complete(1, 101);
    assert(!p.Quiescent());
    // Completion permits immediate node/source reuse while old FIFO tags remain.
    auto second = p.Submit(id, words, false); assert(second && second != gen);
    auto new0 = p.Dma(0x1000, 4, words[0]); assert(new0.generation == second);
    assert(p.Render(words[1], t1, gain) == words[1]);
    assert(p.Render(words[2], t2, gain) == words[2]);
    assert(p.Render(words[0], new0, gain) == words[0]);
    // Checkpoint copy restores exact heard and in-flight metadata.
    auto saved = p;
    auto next = p.Dma(0x1004, 4, words[1]);
    assert(p.Render(words[1], next, gain) == words[1]);
    p = saved;
    auto restored = p.Dma(0x1004, 4, words[1]);
    assert(restored.generation == next.generation && restored.frame == next.frame);
    assert(p.Render(words[1], restored, gain) == words[1]);
    auto final = p.Dma(0x1008, 4, words[2]); p.Discard(final);
    p.Complete(1, 101); assert(p.Quiescent());
    assert(p.Start());
    auto old = p.Submit(id, words, true); assert(old);
    auto stale = p.Dma(0x1000, 4, words[0]);
    p.Replace(); assert(p.Quiescent() && p.Fenced());
    assert(p.Start()); auto fresh = p.Submit(id, words, true); assert(fresh != old);
    p.Discard(stale); assert(p.Fenced());
}

static void rejection() {
    uint32_t words[] = {1, 2};
    Preview p; assert(p.Start());
    assert(p.Submit(identity(1, 0x1000, 2), words, true));
    assert(p.Dma(0x1004, 4, words[1]).generation && p.Fenced());
    p.Replace(); assert(p.Start());
    assert(p.Submit(identity(1, 0x1000, 2), words, true));
    assert(!p.Dma(0x1000, 2, words[0]).generation && p.Fenced());
    p.Replace(); assert(p.Start());
    assert(p.Submit(identity(1, 0x1000, 2), words, true));
    assert(p.Dma(0x1000, 4, 99).generation && p.Fenced());
    p.Replace(); assert(p.Start());
    for (unsigned n = 1; n <= Preview::maxGenerations; ++n)
        assert(p.Submit(identity(n, 0x1000 + n * 16, 2), words, true));
    assert(!p.Submit(identity(99, 0x9000, 2), words, true) && p.Fenced());
    p.Replace(); assert(p.Start());
    assert(p.Submit(identity(1, 0x1000, 2), words, true));
    assert(!p.Submit(identity(1, 0x2000, 2), words, true) && p.Fenced());
    p.Replace(); assert(p.Start());
    assert(p.Submit(identity(1, 0x1000, 2), words, true));
    assert(p.Render(0x12345678, {}, gain) == 0x12345678 && p.Fenced());
    // Cancel alone never proves unread words cannot arrive later.
    p.Replace(); assert(p.Start());
    auto gen = p.Submit(identity(1, 0x1000, 2), words, true);
    assert(p.Render(0, {}, gain) == words[0]);
    p.Complete(1, 101, true); assert(!p.Quiescent());
    auto late = p.Dma(0x1000, 4, words[0]); assert(late.generation == gen);
    assert(p.Render(words[0], late, gain) == 0);
    p.TransportDiscarded(gen); assert(p.Quiescent());
    // Wrong order/value after preview fences but retains exact known-offset tags.
    p.Replace(); assert(p.Start());
    gen = p.Submit(identity(1, 0x1000, 2), words, true);
    assert(p.Render(0, {}, gain) == words[0]);
    auto wrongOrder = p.Dma(0x1004, 4, words[1]);
    assert(wrongOrder.generation == gen && p.Fenced());
    assert(p.Render(words[1], wrongOrder, gain) == words[1]);
    auto changed = p.Dma(0x1000, 4, 99);
    assert(changed.generation == gen && !changed.matches && p.Render(99, changed, gain) == 99);
    p.Complete(1, 101); assert(p.Quiescent());
    // Only one delayed occurrence spends the preview's suppression credit.
    p.Replace(); assert(p.Start());
    gen = p.Submit(identity(1, 0x1000, 2), words, true);
    assert(p.Render(0, {}, gain) == words[0]);
    auto first = p.Dma(0x1000, 4, words[0]);
    auto duplicate = p.Dma(0x1000, 4, words[0]);
    assert(p.Fenced() && p.Serial(first) && !p.Serial(duplicate));
    p.TransportDiscarded(gen); assert(p.Quiescent());
    // A FIFO discard does not spend the eventual serial suppression credit.
    p.Replace(); assert(p.Start());
    gen = p.Submit(identity(1, 0x1000, 2), words, true);
    assert(p.Render(0, {}, gain) == words[0]);
    first = p.Dma(0x1000, 4, words[0]);
    duplicate = p.Dma(0x1000, 4, words[0]);
    p.Discard(first); assert(p.Serial(duplicate));
    p.TransportDiscarded(gen); assert(p.Quiescent());
}

static void commonCadence() {
    Tg50Audio normal, early;
    for (auto *audio : {&normal, &early}) {
        audio->SetMuxOutput(true); audio->WritePwm({63, 144, 288}); audio->Clock(true);
    }
    Preview p; assert(p.Start());
    std::array<uint32_t, 256> words{};
    for (unsigned i = 0; i < words.size(); ++i) words[i] = uint16_t(i + 100) | (uint32_t(uint16_t(-int(i) - 100)) << 16);
    assert(p.Submit(identity(1, 0x1000, words.size()), words.data(), true));
    std::vector<uint32_t> base, preview;
    normal.Advance(10000000, [&](auto word) { base.push_back(word); });
    early.AdvanceHost(10000000, [&](auto word) { preview.push_back(word); },
        [&](uint32_t dac, bool) { return p.Render(dac, {}, gain); });
    assert(base.size() == 441 && preview.size() == base.size());
    // Subtract each independent stereo preview channel to recover every PWM frame.
    for (unsigned i = 0; i < base.size(); ++i) {
        const auto raw = i < words.size() ? words[i] : 0;
        assert(int16_t(preview[i]) - int16_t(raw) == int16_t(base[i]));
        assert(int16_t(preview[i] >> 16) - int16_t(raw >> 16) == int16_t(base[i] >> 16));
    }
    assert(normal.GetState().pwmPhase == early.GetState().pwmPhase);
    assert(normal.GetState().hostPhase == early.GetState().hostPhase);
    assert(normal.GetState().dacCount == early.GetState().dacCount);
}

int main() {
    lifecycle(); rejection(); commonCadence();
    std::puts("Sony host ledger, pause/reuse/rollback and overlapping PWM cadence PASS");
}
