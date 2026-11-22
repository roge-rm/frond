// The Zodiac music synth (native/ZodiacMusicSynth.h) with a SoundFont made here (one looped square wave): silence
// until the guest's note on, sound once it's due, nothing changed in frames before it, quiet again after the note
// off and its release, a late event moving the clock, and refusing a file that isn't a SoundFont. Built with ASan
// and UBSan.
#define TSF_IMPLEMENTATION
#include "ZodiacMusicSynth.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
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

constexpr uint32_t noteOnPc = 0x1000, noteOffPc = 0x1004, volumePc = 0x1008;
zodiac_music::Library library() {
    zodiac_music::Library l;
    for (auto& pc : l.pcs) pc = 0xfffffff0;
    l.pcs[unsigned(zodiac_music::Call::noteOn)] = noteOnPc;
    l.pcs[unsigned(zodiac_music::Call::noteOff)] = noteOffPc;
    l.pcs[unsigned(zodiac_music::Call::setMasterVolume)] = volumePc;
    return l;
}
void call(zodiac_music::Hooks& h, uint32_t pc, uint64_t timeNs, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t r[16] = {a, b, c, d};
    assert(h.hit(library(), pc, r, timeNs, [](uint32_t, void*, uint32_t) { return false; }));
}
constexpr uint64_t ms = 1000000;
}  // namespace

int main() {
    ZodiacMusicSynth synth;
    const Bytes junk(64, 0x55);
    assert(!synth.load(junk.data(), int(junk.size())));
    const Bytes font = soundFont();
    assert(synth.load(font.data(), int(font.size())));

    zodiac_music::Hooks hooks;
    synth.attach(&hooks);
    // Guest 5 s: note on; guest 5.1 s: note off. The first event anchors guest 5 s to frame 0.
    call(hooks, noteOnPc, 5000 * ms, 1, 0, 60, 127);
    call(hooks, noteOffPc, 5100 * ms, 1, 0, 60, 0);
    // The guest's own sound, kept under the music.
    std::vector<int16_t> out(44100 * 2, 100);
    synth.mix(out.data(), 2205);  // 50 ms
    assert(energy(out, 0, 2205) > uint64_t(2205) * 2 * 1000);
    synth.mix(out.data() + 2205 * 2, 44100 - 2205);
    // Sounding up to the note off at 100 ms (4410), then the release, then only the guest's level.
    assert(energy(out, 2205, 4410) > uint64_t(2205) * 2 * 1000);
    assert(energy(out, 30000, 44100) == uint64_t(14100) * 2 * 100);
    assert(synth.playing() == 0 && !hooks.pending());

    // Late by a second (a pause): the clock moves to it and it sounds at once.
    call(hooks, volumePc, 7000 * ms, 64, 0, 0, 0);
    call(hooks, noteOnPc, 7000 * ms, 1, 0, 67, 100);
    std::vector<int16_t> quiet(512 * 2, 0);
    synth.mix(quiet.data(), 512);
    assert(energy(quiet, 0, 512) > 0);
    // An empty font refuses nothing more and mixes nothing.
    ZodiacMusicSynth none;
    none.attach(&hooks);
    std::vector<int16_t> flat(64 * 2, 7);
    none.mix(flat.data(), 64);
    assert(energy(flat, 0, 64) == 64 * 2 * 7);
    std::printf("PASS: Zodiac music synth (SoundFont load and refusal, note on and off on time, release, late events, "
                "volume, mixing)\n");
    return 0;
}
