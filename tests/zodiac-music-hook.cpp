// The Zodiac's music hooks (zodiac_music.h). findLibrary finds a Tapwave Midi Library code resource and refuses
// broken ones, and on the real 1.1 ROM (ZODIAC_ROM, skipped without it) gives the functions the plan lists. A
// guest program with a library of the ROM's shape calls Open, Play, the real-time calls (one in a hot loop),
// Stop, Close, a Play after the Close and an Open of a file that isn't an SMF; with the hooks on, Exact, lean
// Fast and Fast with blocks each give the same events with the guest's arguments, and every engine ends as it
// does with no hooks.
#define ZODIAC_FAST_LEAN_MAIN fastLeanMain
#include "zodiac-fast-lean.cpp"

#include <fstream>
#include <iterator>

#include "zodiac_music.h"

namespace {
using zodiac_music::Kind;

constexpr uint32_t libraryAt = 0x80000, smfAt = 0x90000, sysExAt = 0x91000;
constexpr uint32_t handleAt = 0x00310000, handleValue = 0x00abcd00, midiHandle = 0x1234;
const uint8_t sysExBytes[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};

std::vector<uint8_t> smfFile() {
    std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96};
    const std::vector<uint8_t> body = {0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,  // tempo 500000
                                       0x00, 0x90, 0x3c, 0x64, 0x60, 0x80, 0x3c, 0x40,
                                       0x00, 0xc1, 0xff,                          // program 0xff, kept raw
                                       0x30, 0xff, 0x2f, 0x00};
    b.insert(b.end(), {'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(body.size())});
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// A code resource of the ROM's shape at at: header, 21 branches, one function per entry. Open writes
// handleValue to [r0]; every function returns 0.
void library(std::vector<uint32_t>& words, uint32_t at) {
    auto put = [&](uint32_t offset, uint32_t value) { words[offset / 4] = value; };
    const uint32_t table = at + 0x24, functions = at + 0x100;
    put(at, 0xea000000 | (((functions - at - 8) / 4) & 0xffffff));
    const uint32_t header[] = {0x24, 0x204, 1, 21, 0x6c696272, 0x74774d44, 0x616d6463, 0x40000};
    for (unsigned i = 0; i < 8; ++i) put(at + 4 + i * 4, header[i]);
    for (uint32_t i = 0; i < 21; ++i) {
        const uint32_t fn = functions + i * 0x20, entry = table + i * 4;
        put(entry, 0xea000000 | (((fn - entry - 8) / 4) & 0xffffff));
        if (i == 0) {
            const uint32_t open[] = {0xe92d4010, 0xe59f400c, 0xe5804000, 0xe3a00000, 0xe8bd4010, 0xe12fff1e, handleValue};
            for (unsigned k = 0; k < std::size(open); ++k) put(fn + k * 4, open[k]);
        } else {
            const uint32_t plain[] = {0xe92d4010, 0xe3a00000, 0xe8bd4010, 0xe12fff1e};
            for (unsigned k = 0; k < std::size(plain); ++k) put(fn + k * 4, plain[k]);
        }
    }
}

void bytesAt(std::vector<uint32_t>& words, uint32_t at, const uint8_t* bytes, size_t size) {
    std::memcpy(reinterpret_cast<uint8_t*>(words.data()) + at, bytes, size);
}

std::vector<uint32_t> guestImage() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    const auto pc = [&] { return 0x20000000 + g.cursor + uint32_t(words.size()) * 4; };
    const uint32_t table = 0x20000000 + libraryAt + 0x24;
    const auto call = [&](uint32_t offset) {
        words.push_back(0xeb000000 | (((table + offset - pc() - 8) / 4) & 0xffffff));
    };
    // Sections: 2 peripherals, 3 SRAM cached, 0x160 graphics, 0x200 the image.
    load(words, 0, 0x00304000);
    store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c1e);
    load(words, 0, 0x00304580); store(words, 0, 0, 0x16000c12);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                         // TTB
    load(words, 1, 0x0d); words.push_back(0xee031f10);    // domain 0 client, 1 manager
    load(words, 1, 0x107d); words.push_back(0xee011f10);  // MMU, D- and I-cache on
    load(words, 13, 0x0031f000);
    load(words, 5, handleAt);
    // TwSmfOpen(&handle, smf, 0), then TwSmfPlay(handle, 0, 0, callback).
    load(words, 0, handleAt); load(words, 1, 0x20000000 + smfAt); words.push_back(0xe3a02000);
    call(0);
    words.push_back(0xe5950000);  // ldr r0, [r5]
    words.push_back(0xe3a01000); words.push_back(0xe3a02000); load(words, 3, 0x20012345);
    call(8);
    words.push_back(0xe3a00063);  // mov r0, #99
    call(48);
    const auto midi = [&](uint32_t offset, uint32_t a, uint32_t b, uint32_t c) {
        load(words, 0, midiHandle); load(words, 1, a); load(words, 2, b); load(words, 3, c);
        call(offset);
    };
    midi(28, 2, 60, 100);
    midi(32, 2, 60, 64);
    midi(60, 1, 7, 200);
    midi(36, 3, 5, 0);
    midi(40, 0, 0x2000, 0);
    midi(44, 0x20000000 + sysExAt, sizeof sysExBytes, 0);
    // 100 control changes from a hot loop: CC 10 with r6 as the value.
    words.push_back(0xe3a06000);  // mov r6, #0
    const uint32_t loop = pc();
    load(words, 0, midiHandle); words.push_back(0xe3a01001); words.push_back(0xe3a0200a);
    words.push_back(0xe1a03006);  // mov r3, r6
    call(60);
    words.push_back(0xe2866001);  // add r6, r6, #1
    words.push_back(0xe3560064);  // cmp r6, #100
    words.push_back(0xba000000 | (((loop - pc() - 8) / 4) & 0xffffff));
    words.push_back(0xe5950000); call(16);  // Stop
    words.push_back(0xe5950000); call(4);   // Close
    words.push_back(0xe5950000); call(8);   // a Play after the Close: nothing to play
    // An Open of bytes that aren't an SMF.
    load(words, 0, handleAt + 4); load(words, 1, 0x20000000 + sysExAt); words.push_back(0xe3a02000);
    call(0);
    load(words, 9, 0x600d);
    words.push_back(0xe7f000f0);
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    library(g.words, libraryAt);
    const auto smf = smfFile();
    bytesAt(g.words, smfAt, smf.data(), smf.size());
    bytesAt(g.words, sysExAt, sysExBytes, sizeof sysExBytes);
    return g.words;
}

struct Caught {
    Outcome outcome;
    std::vector<zodiac_music::Event> events;
    zodiac_music::Stats stats;
    unsigned hits = 0;
};

Caught hooked(const std::vector<uint32_t>& words, bool fast, const zodiac_music::Library& lib) {
    Imx1Machine* m = make(words, fast);
    assert(imx1MachineSetCodeHooks(m, lib.pcs, zodiac_music::callCount));
    zodiac_music::Hooks hooks;
    hooks.enable();
    const auto read = [m](uint32_t address, void* data, uint32_t size) {
        return imx1MachineAccessMemory(m, address, data, size, false);
    };
    Caught out;
    for (unsigned n = 0; n < 400000 && running(m); ++n) {
        const auto run = imx1MachineRunBounded(m, {997, never, 100000});
        assert(run.reason != Imx1MachineYield::invalidBounds && run.reason != Imx1MachineYield::pcStop);
        if (run.reason != Imx1MachineYield::codeHook) continue;
        uint32_t r[16], cpsr = 0;
        assert(imx1MachineAtCodeHook(m) && imx1MachineRegisters(m, r, &cpsr));
        uint32_t before[16];
        std::memcpy(before, r, sizeof before);
        assert(hooks.hit(lib, r[15], r, imx1MachineGetStatus(m).time.ticks, read));
        // The hook changes nothing in the guest.
        uint32_t after[16];
        assert(imx1MachineRegisters(m, after, &cpsr) && !std::memcmp(before, after, sizeof after));
        ++out.hits;
        imx1MachineLeaveCodeHook(m);
    }
    out.outcome = finish(m);
    zodiac_music::Event e;
    while (hooks.take(e)) out.events.push_back(e);
    out.stats = hooks.stats();
    return out;
}

void checkEvents(const Caught& c) {
    const auto& ev = c.events;
    assert(c.hits == 113 && c.stats.calls == 113);
    assert(c.stats.opens == 1 && c.stats.badFiles == 1 && c.stats.unmatchedPlays == 1 && !c.stats.dropped);
    assert(ev.size() == 110);
    assert(ev[0].kind == Kind::play && ev[0].handle == handleValue && ev[0].callback == 0x20012345);
    assert(!ev[0].range && !ev[0].flags);
    const auto& smf = *ev[0].smf;
    assert(smf.address == 0x20000000 + smfAt && smf.bytes == smfFile());
    // Tempo, note on, note off, program change, end of track; 96 ticks a quarter at 120 bpm.
    zodiac_smf::Song song;
    assert(zodiac_smf::parseSmf(smf.bytes.data(), smf.bytes.size(), song) == zodiac_smf::Error::none);
    assert(song.events.size() == 5 && song.events[2].timeUs == 500000);
    assert(song.events[3].status == 0xc1 && song.events[3].data1 == 0xff);
    assert(ev[1].kind == Kind::masterVolume && ev[1].value == 99);
    const auto message = [&](size_t i, uint8_t a, uint8_t b, uint8_t c2, uint8_t length) {
        assert(ev[i].kind == Kind::message && ev[i].handle == midiHandle && ev[i].length == length);
        assert(ev[i].bytes[0] == a && ev[i].bytes[1] == b && ev[i].bytes[2] == c2);
    };
    message(2, 0x92, 60, 100, 3);
    message(3, 0x82, 60, 64, 3);
    message(4, 0xb1, 7, 200, 3);  // kept raw, the player clamps
    message(5, 0xc3, 5, 0, 2);
    message(6, 0xe0, 0x00, 0x40, 3);
    assert(ev[7].kind == Kind::sysEx && ev[7].data == std::vector<uint8_t>(std::begin(sysExBytes), std::end(sysExBytes)));
    for (unsigned i = 0; i < 100; ++i) message(8 + i, 0xb1, 10, uint8_t(i), 3);
    assert(ev[108].kind == Kind::stop && ev[108].handle == handleValue);
    assert(ev[109].kind == Kind::close && ev[109].handle == handleValue);
    for (size_t i = 1; i < ev.size(); ++i) assert(ev[i].timeNs >= ev[i - 1].timeNs);
}

void sameEvents(const Caught& a, const Caught& b) {
    assert(a.events.size() == b.events.size());
    for (size_t i = 0; i < a.events.size(); ++i) {
        const auto &x = a.events[i], &y = b.events[i];
        assert(x.kind == y.kind && x.handle == y.handle && x.value == y.value && x.length == y.length);
        assert(!std::memcmp(x.bytes, y.bytes, 3) && x.data == y.data && x.callback == y.callback);
    }
}

void guestCalls() {
    const auto words = guestImage();
    zodiac_music::Library lib;
    assert(zodiac_music::findLibrary(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4, 0x20000000, lib));
    assert(lib.table == 0x20000000 + libraryAt + 0x24 && lib.pcs[0] == 0x20000000 + libraryAt + 0x100);
    Caught first;
    for (int engine = 0; engine < 3; ++engine) {
        const bool fast = engine > 0;
        imx1SetFastBlocksDefault(engine == 2);
        const auto plain = runGuest(words, fast);
        const auto run = hooked(words, fast, lib);
        const char* name = engine == 0 ? "Exact" : engine == 1 ? "lean Fast" : "Fast with blocks";
        std::printf("music hooks, %s: %u hits, %zu events, %llu instructions (%llu lean, %llu in the JIT), r9 %08x\n",
                    name, run.hits, run.events.size(), (unsigned long long)run.outcome.cheap.retired,
                    (unsigned long long)run.outcome.lean, (unsigned long long)run.outcome.jit,
                    run.outcome.run.result.registers[9]);
        assert(plain.run.result.registers[9] == 0x600d && plain.run.result.registers[6] == 100);
        checkEvents(run);
        same(plain, run.outcome);
        assert(run.outcome.memory == plain.memory);
        if (engine == 1) assert(run.outcome.lean > 1000);
        if (engine == 2) assert(run.outcome.jit > 1000);
        if (engine == 0) first = run;
        else sameEvents(first, run);
        // Event times are the machine's, so the engines agree on them too.
        for (size_t i = 0; i < run.events.size(); ++i)
            if (engine == 1) assert(run.events[i].timeNs == first.events[i].timeNs);
    }
    imx1SetFastBlocksDefault(true);
}

// findLibrary on hand-made images: the guest's library, and each way one can be wrong.
void finding() {
    std::vector<uint32_t> words(0x2000 / 4);
    library(words, 0x400);
    const auto bytes = [&] { return reinterpret_cast<const uint8_t*>(words.data()); };
    zodiac_music::Library lib;
    assert(zodiac_music::findLibrary(bytes(), words.size() * 4, 0x30000000, lib));
    assert(lib.header == 0x30000400 && lib.table == 0x30000424);
    for (unsigned i = 0; i < zodiac_music::callCount; ++i)
        assert(lib.pcs[i] == 0x30000500 + zodiac_music::callOffsets[i] / 4 * 0x20);
    const auto refused = [&](uint32_t word, uint32_t value) {
        auto copy = words;
        copy[word / 4] = value;
        zodiac_music::Library none;
        none.table = 7;
        assert(!zodiac_music::findLibrary(reinterpret_cast<const uint8_t*>(copy.data()), copy.size() * 4, 0x30000000, none));
        assert(none.table == 7);
    };
    refused(0x400 + 16, 20);              // entry count
    refused(0x400 + 24, 0x74774d45);      // creator
    refused(0x424 + 20 * 4, 0xe1a00000);  // the last entry, not hooked, isn't a branch
    refused(0x424 + 28, 0xe1a00000);      // NoteOn's entry
    refused(0x500 + 7 * 0x20, 0xe1a00000);  // NoteOn's function doesn't start with STMDB sp!
    refused(0x424, 0xea7fffff);           // Open's branch leaves the image
    // Cut short: the table doesn't fit.
    assert(!zodiac_music::findLibrary(bytes(), 0x424 + 40, 0x30000000, lib));
}

// The 1.1 ROM's library: the table and functions found by hand (step-zodiac-finish-plan, MIDI out design).
void realRom() {
    const char* path = std::getenv("ZODIAC_ROM");
    if (!path || !*path) { std::puts("SKIP: real ROM check, set ZODIAC_ROM"); return; }
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(input), {}};
    if (rom.empty()) { std::puts("SKIP: real ROM check, ZODIAC_ROM unreadable"); return; }
    zodiac_music::Library lib;
    assert(zodiac_music::findLibrary(rom.data(), rom.size(), 0x20000000, lib));
    const uint32_t expected[] = {0x203224e0, 0x20321e44, 0x203223e8, 0x203226e4, 0x20322824, 0x2032288c,
                                 0x203228f4, 0x20322a10, 0x20322a80, 0x20321d04, 0x2032295c};
    assert(lib.header == 0x20321b8c && lib.table == 0x20321bb0);
    for (unsigned i = 0; i < zodiac_music::callCount; ++i) assert(lib.pcs[i] == expected[i]);
    std::puts("real ROM: Tapwave Midi Library at 20321b8c, its 11 hooked functions as listed");
}
}  // namespace

int main() {
    unsetenv("FROND_JIT");
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    finding();
    realRom();
    guestCalls();
    std::puts("PASS: music hooks find the library, catch every call with its arguments in Exact and both Fast "
              "paths, and change nothing the guest does");
    return 0;
}
