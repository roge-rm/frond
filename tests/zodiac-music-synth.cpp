// The Zodiac music synth (native/ZodiacMusicSynth.h) with a SoundFont made here (one looped square wave): silence
// until the guest's note on, sound once it's due, nothing changed in frames before it, quiet again after the note
// off and its release, a late event moving the clock, a song from the guest's memory played through and its Held
// freed, hold, a backlog bigger than the rings with its Stop kept, refusing a file that isn't a SoundFont, the clock
// on the guest audio through an audio queue's marks (queue depth, a stall, the guest's sound stopping and starting
// again, a hold), a deep queue, a stale cue from before a load, the queue's marks kept in order with a gap as a
// break, and the three threads (hooks, helper, audio) at once, which the TSan build checks. Built with ASan and UBSan, and with TSan.
#define TSF_IMPLEMENTATION
#include "ZodiacMusicSynth.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using Bytes = std::vector<uint8_t>;

namespace {
void le(Bytes& b, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void name(Bytes& b, const char* s) {
    char n[20] = {};
    std::strncpy(n, s, 19);
    b.insert(b.end(), n, n + 20);
}
Bytes chunk(const char* id, const Bytes& body) {
    Bytes b(id, id + 4);
    le(b, uint32_t(body.size()), 4);
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() & 1) b.push_back(0);
    return b;
}
Bytes list(const char* type, const std::vector<Bytes>& chunks) {
    Bytes body(type, type + 4);
    for (const auto& c : chunks) body.insert(body.end(), c.begin(), c.end());
    return chunk("LIST", body);
}

// One preset (bank 0, program 0) of one instrument playing a 100-sample square wave, looped, pitch 60 at 44100 Hz.
Bytes soundFont() {
    Bytes smpl;
    for (int i = 0; i < 100; ++i) le(smpl, uint16_t(i < 50 ? 12000 : -12000), 2);
    for (int i = 0; i < 46; ++i) le(smpl, 0, 2);
    Bytes phdr, pbag, pmod(10, 0), pgen, inst, ibag, imod(10, 0), igen, shdr;
    name(phdr, "square"); le(phdr, 0, 2); le(phdr, 0, 2); le(phdr, 0, 2); le(phdr, 0, 4); le(phdr, 0, 4); le(phdr, 0, 4);
    name(phdr, "EOP"); le(phdr, 0, 2); le(phdr, 0, 2); le(phdr, 1, 2); le(phdr, 0, 4); le(phdr, 0, 4); le(phdr, 0, 4);
    le(pbag, 0, 2); le(pbag, 0, 2); le(pbag, 1, 2); le(pbag, 0, 2);
    le(pgen, 41, 2); le(pgen, 0, 2); le(pgen, 0, 2); le(pgen, 0, 2);  // instrument 0
    name(inst, "square"); le(inst, 0, 2);
    name(inst, "EOI"); le(inst, 1, 2);
    le(ibag, 0, 2); le(ibag, 0, 2); le(ibag, 2, 2); le(ibag, 0, 2);
    le(igen, 54, 2); le(igen, 1, 2); le(igen, 53, 2); le(igen, 0, 2); le(igen, 0, 2); le(igen, 0, 2);  // loop, sample 0
    name(shdr, "square"); le(shdr, 0, 4); le(shdr, 100, 4); le(shdr, 0, 4); le(shdr, 100, 4); le(shdr, 44100, 4);
    shdr.push_back(60); shdr.push_back(0); le(shdr, 0, 2); le(shdr, 1, 2);
    name(shdr, "EOS"); for (int i = 0; i < 26; ++i) shdr.push_back(0);
    Bytes ifil;
    le(ifil, 2, 2); le(ifil, 1, 2);
    Bytes body = {'s', 'f', 'b', 'k'};
    for (const auto& l : {list("INFO", {chunk("ifil", ifil)}), list("sdta", {chunk("smpl", smpl)}),
                          list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen),
                                        chunk("inst", inst), chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen),
                                        chunk("shdr", shdr)})})
        body.insert(body.end(), l.begin(), l.end());
    return chunk("RIFF", body);
}

uint64_t energy(const std::vector<int16_t>& s, size_t from, size_t to) {
    uint64_t e = 0;
    for (size_t i = from * 2; i < to * 2; ++i) e += uint64_t(std::abs(int(s[i])));
    return e;
}

constexpr uint32_t noteOnPc = 0x1000, noteOffPc = 0x1004, volumePc = 0x1008, openPc = 0x100c, playPc = 0x1010,
                   stopPc = 0x1014, sysExPc = 0x1018;
zodiac_music::Library library() {
    zodiac_music::Library l;
    for (auto& pc : l.pcs) pc = 0xfffffff0;
    l.pcs[unsigned(zodiac_music::Call::noteOn)] = noteOnPc;
    l.pcs[unsigned(zodiac_music::Call::noteOff)] = noteOffPc;
    l.pcs[unsigned(zodiac_music::Call::setMasterVolume)] = volumePc;
    l.pcs[unsigned(zodiac_music::Call::smfOpen)] = openPc;
    l.pcs[unsigned(zodiac_music::Call::smfPlay)] = playPc;
    l.pcs[unsigned(zodiac_music::Call::smfStop)] = stopPc;
    l.pcs[unsigned(zodiac_music::Call::sysEx)] = sysExPc;
    return l;
}

// A guest's memory, from 0: an SMF at smfAt, its handle word at handleAt, a sysex at sysExAt, a 10 s song at longAt.
// Two 100 ms notes 1 s apart at twoAt.
constexpr uint32_t smfAt = 0x2000, handleAt = 0x3000, sysExAt = 0x3100, handle = 0x4444, longAt = 0x2100,
                   twoAt = 0x2200;
struct Guest {
    Bytes mem = Bytes(0x4000, 0);
    Guest() {
        // Note on 60 at 0, note off at 96 ticks (500 ms at 120 bpm), end there.
        const Bytes smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 12,
                           0x00, 0x90, 0x3c, 0x64, 0x60, 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00};
        std::copy(smf.begin(), smf.end(), mem.begin() + smfAt);
        // Note on 60 at 0, note off at 1920 ticks (10 s), end there.
        const Bytes long10 = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 13,
                              0x00, 0x90, 0x3c, 0x64, 0x8f, 0x00, 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00};
        std::copy(long10.begin(), long10.end(), mem.begin() + longAt);
        // Note on 60 at 0, off at 19 ticks, on again at 192 (1 s), off at 211, end there.
        const Bytes two = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 21,
                           0x00, 0x90, 0x3c, 0x64, 0x13, 0x80, 0x3c, 0x00, 0x81, 0x2d, 0x90, 0x3c, 0x64, 0x13, 0x80,
                           0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00};
        std::copy(two.begin(), two.end(), mem.begin() + twoAt);
        const Bytes sysEx = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
        std::copy(sysEx.begin(), sysEx.end(), mem.begin() + sysExAt);
    }
    bool read(uint32_t address, void* data, uint32_t size) const {
        if (address > mem.size() || size > mem.size() - address) return false;
        std::memcpy(data, mem.data() + address, size);
        return true;
    }
};

void call(zodiac_music::Hooks& h, const Guest& g, uint32_t pc, uint64_t timeNs, uint32_t a, uint32_t b, uint32_t c,
          uint32_t d) {
    const uint32_t r[16] = {a, b, c, d};
    const bool hit = h.hit(library(), pc, r, timeNs, [&g](uint32_t at, void* data, uint32_t n) { return g.read(at, data, n); });
    assert(hit);
    (void)hit;
}
constexpr uint64_t ms = 1000000;

bool same(const std::vector<int16_t>& s, size_t from, size_t to, int16_t level) {
    for (size_t i = from * 2; i < to * 2; ++i)
        if (s[i] != level) return false;
    return true;
}

// The hooks on one thread, the helper on its own, mix on a third, as in the app.
void threads(const Bytes& font) {
    ZodiacMusicSynth synth;
    const bool loaded = synth.load(font.data(), int(font.size()));
    assert(loaded);
    (void)loaded;
    zodiac_music::Hooks hooks;
    hooks.enable();
    synth.attach(&hooks);
    synth.start();
    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<int16_t> out(512 * 2);
        while (!done.load()) {
            std::fill(out.begin(), out.end(), 0);
            synth.mix(out.data(), 512);
            std::this_thread::yield();
        }
    });
    Guest guest;
    uint64_t t = 1000 * ms;
    for (unsigned i = 0; i < 3000; ++i, t += ms / 4) {
        switch (i % 6) {
            case 0: call(hooks, guest, openPc, t, handleAt, smfAt, 0, 0); break;
            case 1:
                std::memcpy(guest.mem.data() + handleAt, &handle, 4);
                call(hooks, guest, playPc, t, handle, 0, 0, 0);
                break;
            case 2: call(hooks, guest, noteOnPc, t, 1, 2, 64, 100); break;
            case 3: call(hooks, guest, sysExPc, t, 1, sysExAt, 6, 0); break;
            case 4: call(hooks, guest, noteOffPc, t, 1, 2, 64, 0); break;
            case 5: call(hooks, guest, stopPc, t, handle, 0, 0, 0); break;
        }
        if (i % 64 == 0) std::this_thread::yield();
    }
    while (hooks.pending() || synth.cuesWaiting()) std::this_thread::yield();
    done.store(true);
    audio.join();
    synth.stop();
    // This thread is both sides now: whatever still waits on the helper's side goes through, two more seconds end
    // the last song, and then every Held is back.
    std::vector<int16_t> out(44100 * 2 * 2, 0);
    for (int i = 0; i < 1000 && (synth.pump() || synth.waiting() || synth.cuesWaiting()); ++i) synth.mix(out.data(), 512);
    assert(!synth.waiting() && !synth.cuesWaiting() && !hooks.stats().dropped);
    synth.mix(out.data(), 44100 * 2);
    synth.pump();
    assert(synth.playing() == 0 && !synth.outstanding());
}

// Held: nothing renders and the clock stands. A backlog past the cue ring waits on the helper's side; past its own
// limit notes are dropped, and the hooks' ring never fills while the helper drains it, so the Stop after them
// still ends the song.
void heldAndBacklog(const Bytes& font) {
    ZodiacMusicSynth synth;
    const bool loaded = synth.load(font.data(), int(font.size()));
    assert(loaded);
    (void)loaded;
    zodiac_music::Hooks hooks;
    hooks.enable();
    synth.attach(&hooks);
    Guest guest;
    call(hooks, guest, openPc, 1000 * ms, handleAt, longAt, 0, 0);
    std::memcpy(guest.mem.data() + handleAt, &handle, 4);
    call(hooks, guest, playPc, 1000 * ms, handle, 0, 0, 0);
    synth.pump();
    std::vector<int16_t> out(512 * 2, 0);
    synth.mix(out.data(), 512);
    assert(synth.playing() == 1 && energy(out, 0, 512) > 0);
    const uint64_t before = synth.framesMixed();
    synth.hold(true);
    std::fill(out.begin(), out.end(), 3);
    synth.mix(out.data(), 512);
    assert(synth.framesMixed() == before && same(out, 0, 512, 3));
    synth.hold(false);

    // 40000 note calls with no mix, the helper pumping every 1000.
    const size_t notes = 40000;
    for (size_t i = 0; i < notes; ++i) {
        call(hooks, guest, i & 1 ? noteOffPc : noteOnPc, 1001 * ms, 1, 3, 70, i & 1 ? 0 : 90);
        if (i % 1000 == 999) synth.pump();
    }
    call(hooks, guest, volumePc, 1002 * ms, 100, 0, 0, 0);
    call(hooks, guest, stopPc, 1002 * ms, handle, 0, 0, 0);
    synth.pump();
    assert(!hooks.stats().dropped && synth.notesDropped() > 0);
    assert(synth.cuesWaiting() == ZodiacMusicSynth::maxCues);
    assert(synth.waiting() <= ZodiacMusicSynth::maxWaiting + 16 + 2);
    for (int i = 0; i < 200 && (synth.cuesWaiting() || synth.waiting()); ++i) {
        synth.mix(out.data(), 512);
        synth.pump();
    }
    assert(!synth.cuesWaiting() && !synth.waiting());
    // The Stop came through: the song is over and its Held back.
    synth.pump();
    assert(synth.playing() == 0 && !synth.outstanding());
}

// One queue for every run, cleared for each (the queue has no destroy).
AudioQueue* sharedQueue() {
    static AudioQueue* const queue = audioQueueCreate(1 << 18);
    audioQueueClear(queue);
    return queue;
}

// The app's audio chain around a synth: an AudioQueue with marks, a guest running leadNs ahead of the device in
// 16 ms slices, and a device popping periods with SdlAudioDriver's buffering (start at 6 periods, an underrun
// below 1, refill to 3). The guest can stall (its time lost) and its SSI can stop and start again. Every note-on
// the synth plays is kept with its device frame, and every device frame with the guest frame it played (-1 for
// silence).
struct Chain {
    ZodiacMusicSynth& synth;
    zodiac_music::Hooks& hooks;
    size_t period;
    uint64_t leadNs;
    AudioQueue* queue = sharedQueue();
    GuestAudioClock clock;
    // Guest ns between the run loop's marks: 0 for each slice, more for an Uncapped guest's long passes.
    uint64_t markEveryNs = 0, markedNs = 0;
    uint64_t guestNs = 0, pushed = 0, lostNs = 0;
    bool ssiOn = true, stalled = false, buffering = true, starting = true;
    std::vector<int64_t> played;
    std::vector<uint64_t> noteOns;
    std::function<void(uint64_t from, uint64_t to)> calls;  // the guest's calls in its slice
    // The guest leads by what the device needs to start, as when it paces to wall time while the device waits.
    Chain(ZodiacMusicSynth& s, zodiac_music::Hooks& h, size_t p)
        : synth(s), hooks(h), period(p), leadNs(uint64_t(p) * 13 / 2 * 1000000000ull / 44100 + 20 * ms) {
        synth.setClock(&clock);
        synth.monitor([this](uint64_t frame, const uint8_t* b, size_t n) {
            if (n == 3 && (b[0] & 0xf0) == 0x90 && b[2]) noteOns.push_back(frame);
        });
    }
    uint64_t deviceNs() const { return uint64_t(played.size()) * 1000000000ull / 44100; }
    void runGuest(uint64_t untilNs) {
        while (guestNs < untilNs) {
            const uint64_t to = std::min(untilNs, guestNs - guestNs % (16 * ms) + 16 * ms);
            if (calls) calls(guestNs, to);
            if (ssiOn)
                for (const uint64_t want = to * 441 / 10000000; pushed < want; ++pushed) audioQueuePush(queue, uint32_t(pushed));
            else
                pushed = to * 441 / 10000000;
            guestNs = to;
            if (guestNs >= markedNs + markEveryNs) {
                audioQueueMark(queue, guestNs);
                clock.noteNewest(guestNs);
                markedNs = guestNs;
            }
        }
    }
    void periodOnce() {
        if (stalled) lostNs += period * 1000000000ull / 44100;
        else runGuest(deviceNs() + leadNs - lostNs);
        synth.pump();
        const auto st = audioQueueGetState(queue);
        if (buffering && st.pendingSamples > (starting ? 6 : 3) * period) buffering = starting = false;
        if (!buffering && st.pendingSamples < period) buffering = true;
        std::vector<uint32_t> pcm(period);
        AudioQueueSpan span;
        const size_t got = audioQueuePopChunkIfGeneration(queue, pcm.data(), buffering ? 0 : period, st.generation, &span);
        // Known from the first mark on.
        assert(span.known ? span.frames == got : !markedNs);
        for (size_t i = 0; i < period; ++i) played.push_back(i < got ? int64_t(pcm[i]) : -1);
        std::vector<int16_t> out(period * 2, 0);
        synth.mix(out.data(), period, span);
    }
    // The device frame where the guest audio of guest ns t played, or -1.
    int64_t frameOf(uint64_t t) const {
        const int64_t want = int64_t(t * 441 / 10000000);
        for (size_t f = 0; f < played.size(); ++f)
            if (played[f] >= want) return int64_t(f);
        return -1;
    }
};

// A song of 60 short notes 100 ms apart (96 ppq at 96000 us a quarter: a tick is 1 ms), at manyAt.
constexpr uint32_t manyAt = 0x2400;
constexpr int manyNotes = 60;
void putMany(Guest& g) {
    Bytes trk = {0x00, 0xff, 0x51, 0x03, 0x01, 0x77, 0x00};
    for (int i = 0; i < manyNotes; ++i) {
        const Bytes note = {uint8_t(i ? 80 : 0), 0x90, 0x3c, 0x64, 20, 0x80, 0x3c, 0x00};
        trk.insert(trk.end(), note.begin(), note.end());
    }
    const Bytes end = {0x00, 0xff, 0x2f, 0x00};
    trk.insert(trk.end(), end.begin(), end.end());
    Bytes smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k'};
    for (int i = 3; i >= 0; --i) smf.push_back(uint8_t(trk.size() >> (8 * i)));
    smf.insert(smf.end(), trk.begin(), trk.end());
    assert(manyAt + smf.size() <= handleAt);
    std::copy(smf.begin(), smf.end(), g.mem.begin() + manyAt);
}

// One run of the song played at guest 1 s through a Chain; what happens to the guest is up to `event`, called
// each device period with the device's time. cueNs: the time on the Open and Play cues; playNs: the guest time
// they're made at; leadNs: the guest's lead on the device, if not the Chain's; markEveryNs: the Chain's.
struct ManyRun {
    std::vector<uint64_t> noteOns;
    std::vector<int64_t> guestAudio;  // where the guest audio of each note's time played
};
ManyRun manyRun(const Bytes& font, size_t period, const std::function<void(Chain&, uint64_t deviceNs)>& event,
                uint64_t cueNs = 1000 * ms, uint64_t playNs = 1000 * ms, uint64_t leadNs = 0,
                uint64_t markEveryNs = 0) {
    ZodiacMusicSynth synth;
    const bool loaded = synth.load(font.data(), int(font.size()));
    assert(loaded);
    (void)loaded;
    zodiac_music::Hooks hooks;
    hooks.enable();
    synth.attach(&hooks);
    Guest guest;
    putMany(guest);
    Chain chain(synth, hooks, period);
    if (leadNs) chain.leadNs = leadNs;
    chain.markEveryNs = markEveryNs;
    bool played = false;
    chain.calls = [&](uint64_t, uint64_t to) {
        if (played || to <= playNs) return;
        call(hooks, guest, openPc, cueNs, handleAt, manyAt, 0, 0);
        std::memcpy(guest.mem.data() + handleAt, &handle, 4);
        call(hooks, guest, playPc, cueNs, handle, 0, 0, 0);
        played = true;
    };
    while (chain.deviceNs() < playNs + 9000 * ms - chain.leadNs) {
        event(chain, chain.deviceNs());
        chain.periodOnce();
    }
    ManyRun r;
    r.noteOns = chain.noteOns;
    for (int i = 0; i < manyNotes; ++i) r.guestAudio.push_back(chain.frameOf(playNs + 100 * uint64_t(i) * ms));
    return r;
}

constexpr int64_t noteGap = 4410, stepFrames = int64_t(ZodiacMusicSynth::step);

// Note i within one step of the guest audio of its time.
bool onGuestAudio(const ManyRun& r, int i) {
    return r.guestAudio[i] >= 0 && int64_t(r.noteOns[i]) >= r.guestAudio[i] &&
           int64_t(r.noteOns[i]) <= r.guestAudio[i] + stepFrames;
}

// The clock on the guest audio through the chain, across every change in it. Steady: each note within a step of
// where the guest audio of its time plays, at a 512 and at an 8192 frame period. A stall the guest never makes
// up (an underrun): back on the guest audio after it. The SSI stopping and starting again: no note dropped or
// played at once with another, every gap within a step of 100 ms while there is no guest audio, so not only at
// period boundaries, and back on the guest audio after it starts again.
void guestClock(const Bytes& font) {
    for (const size_t period : {size_t(512), size_t(8192)}) {
        const ManyRun steady = manyRun(font, period, [](Chain&, uint64_t) {});
        assert(steady.noteOns.size() == manyNotes);
        for (int i = 0; i < manyNotes; ++i) assert(onGuestAudio(steady, i));
    }

    const ManyRun stall = manyRun(font, 512, [](Chain& c, uint64_t t) { c.stalled = t >= 2000 * ms && t < 2300 * ms; });
    assert(stall.noteOns.size() == manyNotes);
    for (int i = 0; i < manyNotes; ++i) {
        if (i < 9 || i > 35) assert(onGuestAudio(stall, i));
        if (i) assert(int64_t(stall.noteOns[i] - stall.noteOns[i - 1]) >= noteGap * 3 / 4);
    }

    for (const size_t period : {size_t(512), size_t(4096)}) {
        // The SSI off from guest 2.5 s to 4.5 s; songs keep the guest's time meanwhile.
        const ManyRun gap = manyRun(font, period, [](Chain& c, uint64_t) {
            c.ssiOn = !(c.guestNs >= 2500 * ms && c.guestNs < 4500 * ms);
        });
        assert(gap.noteOns.size() == manyNotes);
        for (int i = 1; i < manyNotes; ++i) {
            const int64_t d = int64_t(gap.noteOns[i] - gap.noteOns[i - 1]);
            assert(d >= noteGap * 3 / 4 && d <= noteGap * 2);
            // From the last guest audio before the stop to the first after the start: on the device's frames.
            if (i > 16 && i < 33) assert(d >= noteGap - stepFrames && d <= noteGap + stepFrames);
        }
        for (int i = 0; i < 14; ++i) assert(onGuestAudio(gap, i));
        for (int i = 50; i < manyNotes; ++i) assert(onGuestAudio(gap, i));
    }

    // A hold of 400 ms (the guest paused, the device playing out a 600 ms queue at 4096, no underrun): on resume
    // the guest audio is ahead by the hold, steered to with every note kept.
    const ManyRun held = manyRun(font, 4096, [](Chain& c, uint64_t t) {
        const bool in = t >= 2000 * ms && t < 2400 * ms;
        c.stalled = in;
        c.synth.hold(in);
    });
    assert(held.noteOns.size() == manyNotes);
    for (int i = 1; i < manyNotes; ++i) assert(int64_t(held.noteOns[i] - held.noteOns[i - 1]) >= noteGap * 3 / 4);
    for (int i = 50; i < manyNotes; ++i) assert(onGuestAudio(held, i));

    // A queue 2.5 s deep: a fresh cue that far ahead of the clock waits for its time, as it's no later than the
    // guest's newest time.
    const ManyRun deep = manyRun(font, 4096, [](Chain&, uint64_t) {}, 5000 * ms, 5000 * ms, 2500 * ms);
    assert(deep.noteOns.size() == manyNotes);
    for (int i = 0; i < manyNotes; ++i) assert(onGuestAudio(deep, i));

    // Uncapped-like passes, 400 ms of guest time each with one mark at the end: a Play late in a pass is 350 ms
    // past the newest time, still fresh, so it waits for its time.
    const ManyRun uncapped = manyRun(font, 512, [](Chain&, uint64_t) {}, 1150 * ms, 1150 * ms, 0, 400 * ms);
    assert(uncapped.noteOns.size() == manyNotes);
    for (int i = 0; i < manyNotes; ++i) assert(onGuestAudio(uncapped, i));

    // A cue far ahead of the clock, as from a state loaded back while later cues wait: taken as due now, so its
    // song starts then and plays at its pace, neither silent until its time nor all at once.
    const ManyRun stale = manyRun(font, 512, [](Chain&, uint64_t) {}, 6000 * ms);
    assert(stale.noteOns.size() == manyNotes);
    // It's seen when the guest makes it, ahead of the guest audio of then by the queue.
    assert(int64_t(stale.noteOns[0]) + 6615 > stale.guestAudio[0] && int64_t(stale.noteOns[0]) <= stale.guestAudio[0]);
    for (int i = 1; i < manyNotes; ++i) {
        const int64_t d = int64_t(stale.noteOns[i] - stale.noteOns[i - 1]);
        assert(d >= noteGap - stepFrames && d <= noteGap + stepFrames);
    }
}

// The marks stay in order: guest time set back with no clear starts them again, so every frame still queued is
// timed from the new marks, those before the set-back counted back from the first.
void marksInOrder() {
    AudioQueue* queue = sharedQueue();
    for (uint32_t i = 0; i < 4410; ++i) audioQueuePush(queue, i);
    audioQueueMark(queue, 5000 * ms);
    for (uint32_t i = 0; i < 4410; ++i) audioQueuePush(queue, i);
    audioQueueMark(queue, 1100 * ms);
    for (uint32_t i = 0; i < 4410; ++i) audioQueuePush(queue, i);
    audioQueueMark(queue, 1200 * ms);
    std::vector<uint32_t> pcm(4410);
    AudioQueueSpan span;
    const uint64_t generation = audioQueueGetState(queue).generation;
    audioQueuePopChunkIfGeneration(queue, pcm.data(), 4410, generation, &span);
    assert(span.known && span.startNs == 1000 * ms - 100 * ms);
    audioQueuePopChunkIfGeneration(queue, pcm.data(), 4410, generation, &span);
    assert(span.startNs == 1000 * ms && span.endNs == 1100 * ms && span.breakAt > span.frames);
    audioQueuePopChunkIfGeneration(queue, pcm.data(), 4410, generation, &span);
    assert(span.startNs == 1100 * ms && span.endNs == 1200 * ms);

    // A gap (no frames from 1.2 s to 3 s) inside a pop is a break at the first frame after it.
    for (uint32_t i = 0; i < 441; ++i) audioQueuePush(queue, i);
    audioQueueMark(queue, 1210 * ms);
    audioQueueMark(queue, 3000 * ms);
    for (uint32_t i = 0; i < 441; ++i) audioQueuePush(queue, i);
    audioQueueMark(queue, 3010 * ms);
    audioQueuePopChunkIfGeneration(queue, pcm.data(), 882, generation, &span);
    assert(span.frames == 882 && span.breakAt == 441 && span.breakNs == 3000 * ms);
    assert(span.startNs == 1200 * ms && span.endNs == 3010 * ms);
}

}  // namespace

int main() {
    ZodiacMusicSynth synth;
    const Bytes junk(64, 0x55);
    const bool junkLoaded = synth.load(junk.data(), int(junk.size()));
    assert(!junkLoaded);
    const Bytes font = soundFont();
    const bool loaded = synth.load(font.data(), int(font.size()));
    assert(loaded);
    (void)junkLoaded;
    (void)loaded;

    zodiac_music::Hooks hooks;
    hooks.enable();
    synth.attach(&hooks);
    Guest guest;
    // Guest 5 s: master volume, which anchors guest 5 s to frame 0; 5.05 s note on; 5.15 s note off.
    call(hooks, guest, volumePc, 5000 * ms, 127, 0, 0, 0);
    call(hooks, guest, noteOnPc, 5050 * ms, 1, 0, 60, 127);
    call(hooks, guest, noteOffPc, 5150 * ms, 1, 0, 60, 0);
    synth.pump();
    assert(!hooks.pending() && synth.cuesWaiting() == 3);
    // The guest's own sound, kept under the music.
    std::vector<int16_t> out(44100 * 2, 100);
    synth.mix(out.data(), 2205);  // 50 ms: up to the note on, nothing changed
    assert(same(out, 0, 2205, 100));
    synth.mix(out.data() + 2205 * 2, 44100 - 2205);
    // Sounding from the note on at 50 ms (2205) to the note off at 150 ms (6615), then the release, then only the
    // guest's level.
    assert(energy(out, 2205, 6615) > uint64_t(4410) * 2 * 1000);
    assert(same(out, 30000, 44100, 100));
    assert(synth.playing() == 0 && !synth.cuesWaiting());

    // Late by a second and more (guest 7 s, the clock at 6.0 s): the clock moves to it and it sounds at once.
    call(hooks, guest, noteOnPc, 7000 * ms, 1, 0, 67, 100);
    synth.pump();
    std::vector<int16_t> quiet(512 * 2, 0);
    synth.mix(quiet.data(), 512);
    assert(energy(quiet, 0, 512) > 0);
    call(hooks, guest, noteOffPc, 7020 * ms, 1, 0, 67, 0);

    // A song from guest memory: copied at its open, parsed on the helper's side, played, and its Held freed once it
    // ends and the helper runs again. A guest sysex's Held comes back at once.
    call(hooks, guest, openPc, 8000 * ms, handleAt, smfAt, 0, 0);
    std::memcpy(guest.mem.data() + handleAt, &handle, 4);
    call(hooks, guest, playPc, 8000 * ms, handle, 0, 0, 0);
    call(hooks, guest, sysExPc, 8000 * ms, 1, sysExAt, 6, 0);
    synth.pump();
    assert(synth.outstanding() == 2 && !synth.refused());
    std::vector<int16_t> song(44100 * 2, 0);
    synth.mix(song.data(), 4410);
    assert(synth.playing() == 1 && energy(song, 0, 4410) > 0);
    synth.mix(song.data() + 4410 * 2, 44100 - 4410);
    assert(synth.playing() == 0);
    synth.pump();
    assert(!synth.outstanding());

    // An empty font mixes nothing.
    ZodiacMusicSynth none;
    none.attach(&hooks);
    std::vector<int16_t> flat(64 * 2, 7);
    none.mix(flat.data(), 64);
    assert(same(flat, 0, 64, 7));

    heldAndBacklog(font);
    guestClock(font);
    marksInOrder();
    threads(font);
    std::printf("PASS: Zodiac music synth (SoundFont load and refusal, nothing before the first note, note on and off "
                "on time, release, late events, a guest song through the helper, Helds freed, hold, backlog, clock on the guest "
                "audio across stalls, gaps and a hold, a deep queue, a stale cue, marks in order, three threads)\n");
    return 0;
}
