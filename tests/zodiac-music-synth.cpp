// The Zodiac music synth (native/ZodiacMusicSynth.h) with a SoundFont made here (one looped square wave): silence
// until the guest's note on, sound once it's due, nothing changed in frames before it, quiet again after the note
// off and its release, a late event moving the clock, a song from the guest's memory played through and its Held
// freed, hold, a backlog bigger than the rings with its Stop kept, refusing a file that isn't a SoundFont, the clock
// on the guest audio through an audio queue's marks (queue depth, an underrun, no guest audio), and the three
// threads (hooks, helper, audio) at once, which the TSan build checks. Built with ASan and UBSan, and with TSan.
#define TSF_IMPLEMENTATION
#include "ZodiacMusicSynth.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
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

// The clock on the guest audio, through an AudioQueue as the app has it: the guest runs 150 ms ahead of the device
// and marks the queue every 16 ms, the device pops 512-frame periods and passes the span. A song played at guest
// 1 s sounds where the guest audio of 1 s plays, not 150 ms early; after a 116 ms underrun its second note still
// lines up with the guest audio of 2 s; with no guest audio at all (SSI off) a later song still plays, by the marks.
void guestClock(const Bytes& font) {
    ZodiacMusicSynth synth;
    const bool loaded = synth.load(font.data(), int(font.size()));
    assert(loaded);
    (void)loaded;
    zodiac_music::Hooks hooks;
    hooks.enable();
    synth.attach(&hooks);
    Guest guest;
    // Kept for the run (the queue has no destroy).
    static AudioQueue* const queue = audioQueueCreate(1 << 16);
    constexpr size_t period = 512;
    uint64_t guestNs = 0, pushed = 0;
    bool played = false, playedAgain = false, ssiOn = true;
    // The guest's frame index each device frame played, or -1 for silence; the music on its own.
    std::vector<int64_t> played_;
    std::vector<int16_t> music;
    const auto runGuest = [&](uint64_t untilNs) {
        while (guestNs < untilNs) {
            const uint64_t to = std::min(untilNs, guestNs - guestNs % (16 * ms) + 16 * ms);
            if (!played && to > 1000 * ms) {
                call(hooks, guest, openPc, 1000 * ms, handleAt, twoAt, 0, 0);
                std::memcpy(guest.mem.data() + handleAt, &handle, 4);
                call(hooks, guest, playPc, 1000 * ms, handle, 0, 0, 0);
                played = true;
            }
            if (!playedAgain && to > 5000 * ms) {
                call(hooks, guest, playPc, 5000 * ms, handle, 0, 0, 0);
                playedAgain = true;
            }
            if (ssiOn)
                for (const uint64_t want = to * 441 / 10000000; pushed < want; ++pushed) audioQueuePush(queue, uint32_t(pushed));
            guestNs = to;
            audioQueueMark(queue, guestNs);
        }
    };
    const auto device = [&](bool underrun) {
        std::vector<uint32_t> pcm(period);
        AudioQueueSpan span;
        const size_t got = audioQueuePopChunkIfGeneration(queue, pcm.data(), underrun ? 0 : period,
                                                          audioQueueGetState(queue).generation, &span);
        assert(span.known && span.frames == got);
        for (size_t i = 0; i < period; ++i) played_.push_back(i < got ? int64_t(pcm[i]) : -1);
        std::vector<int16_t> out(period * 2, 0);
        synth.mix(out.data(), period, span);
        music.insert(music.end(), out.begin(), out.end());
    };
    runGuest(150 * ms);
    uint64_t lostNs = 0;
    for (int p = 0; p < 690; ++p) {  // about 8 s of device frames
        synth.pump();
        const uint64_t deviceNs = uint64_t(played_.size()) * 1000000000ull / 44100;
        // An underrun at 1.5 s: the guest stalls for 10 periods and that time is lost.
        const bool underrun = p >= 130 && p < 140;
        if (underrun) lostNs += period * 1000000000ull / 44100;
        else runGuest(deviceNs + 150 * ms - lostNs);
        if (deviceNs > 4000 * ms) ssiOn = false;
        device(underrun);
    }
    // Onsets: sound after 300 ms of quiet.
    std::vector<size_t> onsets;
    size_t quiet = 1 << 30;
    for (size_t f = 0; f < music.size() / 2; ++f) {
        if (music[2 * f] || music[2 * f + 1]) {
            if (quiet > 13230) onsets.push_back(f);
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    assert(onsets.size() == 4);
    // Where the guest audio of guest 1 s and 2 s plays: the music is there within one 64-frame step.
    for (int i = 0; i < 2; ++i) {
        const int64_t frame = int64_t(1000 * (i + 1)) * 441 / 10;
        size_t at = 0;
        while (at < played_.size() && played_[at] < frame) ++at;
        assert(onsets[i] >= at && onsets[i] < at + ZodiacMusicSynth::step + 1);
    }
    // With no guest audio from 4 s on, the clock follows the marks, the guest's time now: the second Play at guest
    // 5 s sounds when the guest gets there, 34 ms before the device's 5 s (150 ms ahead less the 116 ms lost).
    const double again = double(onsets[2]) / 44100;
    assert(again > 4.96 && again < 4.99);
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
    threads(font);
    std::printf("PASS: Zodiac music synth (SoundFont load and refusal, nothing before the first note, note on and off "
                "on time, release, late events, a guest song through the helper, Helds freed, hold, backlog, clock on the guest "
                "audio, three threads)\n");
    return 0;
}
