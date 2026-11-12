// Zodiac machine save state: every refusal leaves the machine as it was, the I2C bus is part of
// a state, a state loads in place into a machine that ran further, and a Fast state loads into Exact.
#define ZODIAC_STATE_MAIN originalStateMain
#include "zodiac-machine-state.cpp"
#include "imx1_i2c_read_window.h"
#include "machine_state_archive.h"
#include <map>
#include <string>
namespace {
using S = Imx1StateStatus;
uint32_t get32(const Bytes& b, size_t at) { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; }
uint64_t get64(const Bytes& b, size_t at) { uint64_t v; std::memcpy(&v, b.data() + at, 8); return v; }
void put32(Bytes& b, size_t at, uint32_t v) { assert(at + 4 <= b.size()); std::memcpy(b.data() + at, &v, 4); }
void put64(Bytes& b, size_t at, uint64_t v) { assert(at + 8 <= b.size()); std::memcpy(b.data() + at, &v, 8); }

// A state taken apart, to change one thing and put it back together with good CRCs.
struct Section {
    uint32_t fourcc = 0, version = 0;
    uint64_t offset = 0;
    Bytes bytes;
    std::string name() const { return {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24)}; }
};
struct Parsed {
    uint32_t coreRevision = 0, flags = 0;
    uint64_t layout = 0, tableOffset = 0;
    std::vector<Section> sections;
    explicit Parsed(const Bytes& b) {
        assert(b.size() >= stateContainerHeaderBytes && !std::memcmp(b.data(), stateContainerMagic, 8));
        coreRevision = get32(b, 12); layout = get64(b, 16); flags = get32(b, 24);
        const uint32_t count = get32(b, 28);
        tableOffset = get64(b, 32);
        assert(get64(b, 40) == b.size() && tableOffset + uint64_t(count) * stateContainerEntryBytes == b.size());
        for (uint32_t n = 0; n < count; ++n) {
            const size_t at = size_t(tableOffset) + n * stateContainerEntryBytes;
            Section s;
            s.fourcc = get32(b, at); s.version = get32(b, at + 4); s.offset = get64(b, at + 8);
            const uint64_t size = get64(b, at + 16);
            s.bytes.assign(b.begin() + s.offset, b.begin() + s.offset + size);
            sections.push_back(std::move(s));
        }
    }
    Section* find(const char (&name)[5]) {
        for (auto& s : sections) if (s.fourcc == stateFourcc(name)) return &s;
        return nullptr;
    }
    Bytes& at(const char (&name)[5]) { auto* s = find(name); assert(s); return s->bytes; }
    void drop(uint32_t fourcc) {
        sections.erase(std::remove_if(sections.begin(), sections.end(), [&](const Section& s) { return s.fourcc == fourcc; }),
                       sections.end());
    }
    Bytes build() const {
        Bytes out;
        StateContainerWriter file(out, coreRevision, layout, flags);
        for (const auto& s : sections) {
            auto a = file.begin(s.fourcc, s.version);
            a.DoBuffer(const_cast<uint8_t*>(s.bytes.data()), s.bytes.size());
            file.end();
        }
        file.finish();
        return out;
    }
};
// After a change to the header's first 52 bytes.
void sealHeader(Bytes& b) { put32(b, 52, crc::CRC32(b.data(), 52)); }

// A machine a refused state is offered to. Each refusal has to leave its next save as the last one.
struct Target {
    Live& live;
    Bytes before;
    unsigned refusals = 0;
    explicit Target(Live& live) : live(live), before(live.save()) {}
    void refuse(const char* what, const Bytes& state, S status, const char* section = nullptr, const char* detail = nullptr,
                uint32_t flags = 0) {
        refuseRaw(what, state.data(), state.size(), status, section, detail, flags);
    }
    void refuseRaw(const char* what, const uint8_t* data, size_t size, S status, const char* section, const char* detail,
                   uint32_t flags = 0) {
        const auto check = imx1MachineCheckState(live.machine, data, size, flags);
        const auto load = imx1MachineLoadState(live.machine, data, size, flags);
        const auto fits = [&](const Imx1StateInfo& info) {
            return info.status == status && (!section || !std::strcmp(info.section, section)) &&
                   (!detail || (info.detail && !std::strcmp(info.detail, detail)));
        };
        if (!fits(check) || !fits(load))
            std::fprintf(stderr, "%s: expected %s %s %s, check gave %s %s %s, load gave %s %s %s\n", what,
                         imx1StateStatusName(status), section ? section : "-", detail ? detail : "-",
                         imx1StateStatusName(check.status), check.section, check.detail ? check.detail : "-",
                         imx1StateStatusName(load.status), load.section, load.detail ? load.detail : "-");
        assert(fits(check) && fits(load));
        const Bytes after = live.save();
        if (after != before) std::fprintf(stderr, "%s: the refused state changed the machine\n", what);
        assert(after == before);
        ++refusals;
    }
};

Imx1I2cReadWindow::Image image(uint8_t seed) {
    Imx1I2cReadWindow::Image bytes{};
    for (unsigned n = 0; n < bytes.size(); ++n) bytes[n] = uint8_t(n * 7 + seed);
    return bytes;
}
// Reads two bytes at offset 0x20 through a repeated START: the first acknowledged, the second not.
struct I2cGuest {
    Guest guest{"I2C read", setup(), {}};
    // The branch that closes each wait for a byte.
    std::vector<uint32_t> waits;
    I2cGuest() {
        auto& w = guest.words;
        const auto wait = [&] {
            w.push_back(0xe590200c); w.push_back(0xe3120080);
            waits.push_back(uint32_t(w.size() * 4));
            w.push_back(0x0afffffc);
        };
        load(w, 0, 0x0021c020); store(w, 0, 0, 0);
        load(w, 0, 0x00217000);
        store(w, 0, 8, 0x80); store(w, 0, 8, 0xb0); store(w, 0, 0x10, 0xa0); wait();
        store(w, 0, 0x10, 0x20); wait();
        store(w, 0, 8, 0xb4); store(w, 0, 0x10, 0xa1); wait();
        store(w, 0, 8, 0xa0); w.push_back(0xe5903010); wait();
        store(w, 0, 8, 0xa8); w.push_back(0xe5903010); wait();
        store(w, 0, 8, 0x88); w.push_back(0xe5904010);
        w.push_back(0xe7f000f0);
        guest.options.bootstrapPeripherals = guest.options.assumeI2cSingleMaster = true;
    }
    // Which wait loop the last instruction was in, counted from 1, or 0.
    unsigned waiting(const Record& r) const {
        for (unsigned n = 0; n < waits.size(); ++n)
            if (r.run.result.trace.pc + 8 >= waits[n] && r.run.result.trace.pc <= waits[n]) return n + 1;
        return 0;
    }
};
// The bus section of a save: cursor, then protocol state.
uint32_t busCursor(const Bytes& save) { Parsed p(save); return get32(p.at("I2CB"), 0); }
uint32_t busProtocol(const Bytes& save) { Parsed p(save); return get32(p.at("I2CB"), 4); }

std::vector<Call> i2cCalls() { return std::vector<Call>(400, slice(7, 10000)); }
void i2cRoundTrip(bool fast) {
    I2cGuest i2c;
    const auto bytes = image(3);
    Imx1I2cReadWindow window(bytes);
    i2c.guest.options.i2cBus = &window;
    // Every machine here uses the one window in turn, so each load also has to put the window back.
    const auto r = roundTrip(i2c.guest, fast, i2cCalls(), [&](const std::vector<Record>& all, size_t k) {
        return k % 9 == 0 || (i2c.waiting(all[k]) && k % 3 == 0);
    });
    const auto& end = r.back().run;
    if (end.status != Imx1MachineStatus::terminal || end.result.trace.instruction != 0xe7f000f0)
        std::fprintf(stderr, "I2C guest stop=%s pc=%08x address=%08x status=%s points=%zu\n", imx1ProbeStopName(end.result.stop),
                     end.result.trace.pc, end.result.address, imx1BootstrapAccessName(end.result.peripheralStatus), r.size());
    assert(end.status == Imx1MachineStatus::terminal && end.result.trace.instruction == 0xe7f000f0);
    assert(end.result.registers[3] == bytes[0x20] && end.result.registers[4] == bytes[0x21]);
    // Saves fell inside the wait for every byte, and with the window at each step of the read.
    std::map<uint32_t, unsigned> inFlight, protocol;
    bool advanced = false;
    for (size_t k = 1; k + 1 < r.size(); ++k) {
        // Still in the same loop a slice later, so the byte was on the wire at this save.
        if (i2c.waiting(r[k]) && i2c.waiting(r[k]) == i2c.waiting(r[k + 1])) ++inFlight[i2c.waiting(r[k])];
        ++protocol[busProtocol(r[k].save)];
        advanced |= busCursor(r[k].save) == 0x21 && busProtocol(r[k].save) == 5;
    }
    if (inFlight.size() != i2c.waits.size() || !advanced)
        std::fprintf(stderr, "I2C saves: waits %zu of %zu, protocol states %zu, advanced %d\n", inFlight.size(),
                     i2c.waits.size(), protocol.size(), advanced);
    assert(inFlight.size() == i2c.waits.size() && advanced);
    // offset, offsetSet, reading and ended.
    assert(protocol.count(2) && protocol.count(3) && protocol.count(5) && protocol.count(6));
}

// A state from the middle of the read, loaded into a machine that ran to the end, then into one
// that ran somewhere else, has to continue like a fresh machine that loaded it.
void loadInPlace(bool fast) {
    I2cGuest i2c;
    const auto calls = i2cCalls();
    const auto bytes = image(3);
    std::vector<Record> reference;
    size_t middle = 0;
    {
        Imx1I2cReadWindow window(bytes);
        auto g = i2c.guest; g.options.i2cBus = &window;
        Live a(g, fast);
        reference.push_back({}); reference.back().save = a.save();
        for (const auto& c : calls) {
            reference.push_back(a.run(c));
            if (reference.back().run.status == Imx1MachineStatus::terminal) break;
        }
    }
    for (size_t k = 1; k < reference.size(); ++k)
        if (busProtocol(reference[k].save) == 5 && i2c.waiting(reference[k])) middle = k;
    assert(middle && middle + 2 < reference.size());
    const auto continued = [&](Live& m) {
        std::vector<Record> out;
        for (size_t k = middle; k + 1 < reference.size(); ++k) out.push_back(m.run(calls[k]));
        return out;
    };
    std::vector<Record> fresh;
    {
        Imx1I2cReadWindow window(bytes);
        auto g = i2c.guest; g.options.i2cBus = &window;
        Live b(g, fast);
        b.load(reference[middle].save);
        fresh = continued(b);
    }
    for (size_t n = 0; n < fresh.size(); ++n) match("I2C fresh load", fast, middle + n, "fresh", reference[middle + 1 + n], fresh[n]);
    assert(fresh.back().run.status == Imx1MachineStatus::terminal);
    Imx1I2cReadWindow window(bytes);
    auto g = i2c.guest; g.options.i2cBus = &window;
    Live c(g, fast);
    // Past the end, terminal, with the window ended.
    for (const auto& call : calls) if (c.run(call).run.status == Imx1MachineStatus::terminal) break;
    assert(imx1MachineGetStatus(c.machine).status == Imx1MachineStatus::terminal);
    c.load(reference[middle].save);
    assert(c.save() == reference[middle].save);
    auto again = continued(c);
    for (size_t n = 0; n < fresh.size(); ++n) match("I2C load after the end", fast, middle + n, "in place", fresh[n], again[n]);
    // Back from the end to an earlier point, then forward to a later one, then to the middle again.
    c.load(reference[2].save);
    c.run(calls[2]);
    c.load(reference[middle + 1].save);
    c.run(calls[middle + 1]);
    c.load(reference[middle].save);
    again = continued(c);
    for (size_t n = 0; n < fresh.size(); ++n) match("I2C load after other loads", fast, middle + n, "in place", fresh[n], again[n]);
}

// A Fast state continues on an Exact machine the way the Exact run did from that point.
void fastIntoExact() {
    I2cGuest i2c;
    const auto calls = i2cCalls();
    const auto bytes = image(3);
    std::vector<Record> runs[2];
    for (bool fast : {false, true}) {
        Imx1I2cReadWindow window(bytes);
        auto g = i2c.guest; g.options.i2cBus = &window;
        Live a(g, fast);
        runs[fast].push_back({}); runs[fast].back().save = a.save();
        for (const auto& c : calls) {
            runs[fast].push_back(a.run(c));
            if (runs[fast].back().run.status == Imx1MachineStatus::terminal) break;
        }
    }
    assert(runs[0].size() == runs[1].size());
    unsigned checked = 0;
    for (size_t k = 1; k + 1 < runs[0].size(); k += 5) {
        assert(runs[1][k].save != runs[0][k].save);
        Imx1I2cReadWindow window(bytes);
        auto g = i2c.guest; g.options.i2cBus = &window;
        Live x(g, false);
        const auto info = imx1MachineLoadState(x.machine, runs[1][k].save.data(), runs[1][k].save.size());
        assert(info.status == S::ok && info.engineChanged && info.retired == runs[0][k].run.retiredInstructions);
        // Right after the load only curInstrPC can differ: Fast doesn't keep it per instruction.
        // It's CPU bytes 172 to 175, after the sequence, the registers, the banks, SPSR and the flags.
        const Parsed loaded(x.save()), exact(runs[0][k].save);
        assert(loaded.flags == exact.flags && loaded.sections.size() == exact.sections.size());
        for (size_t n = 0; n < loaded.sections.size(); ++n) {
            auto mine = loaded.sections[n].bytes;
            const auto& theirs = exact.sections[n].bytes;
            assert(loaded.sections[n].fourcc == exact.sections[n].fourcc && mine.size() == theirs.size());
            if (loaded.sections[n].fourcc == stateFourcc("CPU ")) std::copy(theirs.begin() + 172, theirs.begin() + 176, mine.begin() + 172);
            if (mine != theirs) std::fprintf(stderr, "Fast state on Exact, point %zu: %s differs\n", k, loaded.sections[n].name().c_str());
            assert(mine == theirs);
        }
        // Exact from here on, so everything matches the Exact run, its next save included.
        match("Fast state on Exact", false, k, "engine changed", runs[0][k + 1], x.run(calls[k]));
        ++checked;
    }
    assert(checked >= 5);
}

// A GPIO interrupt routed to FIQ is raised while F is masked, held over single steps, then taken.
void pendingFiq(bool fast) {
    constexpr uint32_t pins = 0x0021c000, bit = 1u << 13;
    Guest g{"pending FIQ", setup(), {}};
    auto& words = g.words;
    load(words, 0, pins); store(words, 0, 0x28, 3u << 26);
    load(words, 0, 0x00223000);
    store(words, 0, 0x1c, 1u << 11);
    store(words, 0, 8, 11);
    words.push_back(0xe3a07000);
    load(words, 0, pins);
    store(words, 0, 0x30, bit);
    for (unsigned i = 0; i < 12; ++i) words.push_back(0xe1a00000);
    words.push_back(0xe321f013);
    words.push_back(0xe1a00000); words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 9, pins);
    words.push_back(0xe599a034);
    words.push_back(0xe3a0b000); words.push_back(0xe589b030);
    words.push_back(0xe589a034); words.push_back(0xe599b034);
    words.push_back(0xe2877001); words.push_back(0xe25ef004);
    g.options.bootstrapPeripherals = g.options.rtcInitialTimeKnown = true;
    g.options.gpioAInputKnown = true; g.options.gpioAInput = 0;
    unsigned taken = 0;
    const auto r = roundTrip(g, fast, std::vector<Call>(80, slice(1, 10000)),
        [&taken](const std::vector<Record>& all, size_t k) {
            return all[k].run.result.interruptPending.fast && !all[k].run.result.fiqEntries && taken++ % 3 == 0;
        });
    unsigned pending = 0;
    for (const auto& x : r) pending += x.run.result.interruptPending.fast && !x.run.result.fiqEntries;
    const auto& end = r.back().run;
    if (pending < 8 || end.status != Imx1MachineStatus::terminal || end.result.fiqEntries != 1)
        std::fprintf(stderr, "FIQ guest pending=%u stop=%s pc=%08x address=%08x status=%s fiq=%llu\n", pending,
                     imx1ProbeStopName(end.result.stop), end.result.trace.pc, end.result.address,
                     imx1BootstrapAccessName(end.result.peripheralStatus), (unsigned long long)end.result.fiqEntries);
    assert(pending >= 8 && taken >= pending);
    assert(end.status == Imx1MachineStatus::terminal && end.result.registers[6] == 0x55 && end.result.registers[7] == 1 &&
           end.result.fiqEntries == 1 && end.result.irqEntries == 0);
}

// Every way the container, the identity and the values can be wrong, against one machine.
// state is a save of the same guest at another point, so a partly applied load would show.
void rejectMatrix(const char* name, Target& t, const Bytes& state, bool bus) {
    const Parsed good(state);
    assert(good.build() == state);
    assert(imx1MachineCheckState(t.live.machine, state.data(), state.size()).status == S::ok);
    assert(state != t.before);
    const unsigned start = t.refusals;

    // Cut at every section edge, around the header and inside the table.
    std::vector<size_t> cuts{0, 1, 7, 8, 9, stateContainerHeaderBytes - 1, stateContainerHeaderBytes,
                             size_t(good.tableOffset), size_t(good.tableOffset) + 1, state.size() - 1};
    for (const auto& s : good.sections) {
        cuts.push_back(size_t(s.offset));
        cuts.push_back(size_t(s.offset + s.bytes.size()));
        if (!s.bytes.empty()) cuts.push_back(size_t(s.offset + s.bytes.size() / 2));
    }
    for (const size_t cut : cuts) {
        assert(cut < state.size());
        t.refuse("cut", Bytes(state.begin(), state.begin() + cut), S::truncated);
    }
    t.refuseRaw("no data", nullptr, 0, S::truncated, nullptr, nullptr);
    // Longer than the header says.
    { Bytes b = state; b.push_back(0); t.refuse("extra byte", b, S::corruptSection, "HEAD"); }

    // One flipped byte in every section, in the header and in the table.
    for (const auto& s : good.sections) {
        if (s.bytes.empty()) continue;
        for (const size_t at : {size_t(0), s.bytes.size() / 2, s.bytes.size() - 1}) {
            Bytes b = state; b[size_t(s.offset) + at] ^= 0x04;
            t.refuse("flipped byte", b, S::corruptSection, s.name().c_str(), "crc");
        }
    }
    for (const size_t at : {size_t(12), size_t(16), size_t(24), size_t(28), size_t(32), size_t(40), size_t(48), size_t(52),
                            size_t(good.tableOffset), size_t(good.tableOffset) + 9, state.size() - 1}) {
        Bytes b = state; b[at] ^= 0x04;
        t.refuse("flipped header or table byte", b, S::corruptSection, "HEAD");
    }
    for (const size_t at : {size_t(0), size_t(7)}) { Bytes b = state; b[at] ^= 0x04; t.refuse("magic", b, S::badMagic); }
    for (const uint32_t version : {0u, 2u, 0xffffffffu}) {
        Bytes b = state; put32(b, 8, version); sealHeader(b);
        t.refuse("format version", b, S::badFormatVersion);
    }

    // The revision of the core that saved it, the layout and the flags.
    for (const uint32_t revision : {imx1MachineStateCoreRevision - 1, imx1MachineStateCoreRevision + 1}) {
        auto p = good; p.coreRevision = revision;
        const Bytes b = p.build();
        t.refuse("core revision", b, S::coreRevisionMismatch);
        const auto allowed = imx1MachineCheckState(t.live.machine, b.data(), b.size(), imx1StateAllowCoreRevision);
        assert(allowed.status == S::ok && allowed.coreRevisionDiffers && allowed.coreRevision == revision);
        // The flag lets only the revision through.
        p.layout ^= 1;
        t.refuse("core revision and layout", p.build(), S::layoutMismatch, nullptr, "layout fingerprint", imx1StateAllowCoreRevision);
    }
    { auto p = good; p.layout ^= 0x100; t.refuse("layout", p.build(), S::layoutMismatch, nullptr, "layout fingerprint"); }
    { auto p = good; p.flags |= 8; t.refuse("flags", p.build(), S::invalidValue, nullptr, "flags"); }
    { auto p = good; p.flags ^= 4; t.refuse("terminal flag", p.build(), S::invalidValue, "PROB", "terminal flag"); }

    // Each section missing, and each with another version.
    for (const auto& s : good.sections) {
        auto p = good; p.drop(s.fourcc);
        t.refuse("missing section", p.build(), S::missingSection, s.name().c_str());
        p = good;
        for (auto& x : p.sections) if (x.fourcc == s.fourcc) ++x.version;
        t.refuse("section version", p.build(), S::layoutMismatch, s.name().c_str(), "section version");
        // One byte more and one byte less than the fields take.
        p = good;
        for (auto& x : p.sections) if (x.fourcc == s.fourcc) x.bytes.push_back(0);
        const Bytes longer = p.build();
        const auto info = imx1MachineCheckState(t.live.machine, longer.data(), longer.size());
        assert(info.status == S::invalidValue || info.status == S::optionsMismatch);
        t.refuse("longer section", longer, info.status, s.name().c_str());
        if (s.bytes.empty()) continue;
        p = good;
        for (auto& x : p.sections) if (x.fourcc == s.fourcc) x.bytes.pop_back();
        const Bytes shorter = p.build();
        const auto less = imx1MachineCheckState(t.live.machine, shorter.data(), shorter.size());
        assert(less.status == S::invalidValue || less.status == S::romMismatch || less.status == S::optionsMismatch);
        t.refuse("shorter section", shorter, less.status, s.name().c_str());
    }
    // The ring and the Fast counters go with their flags.
    if (good.flags & 2) { auto p = good; p.flags &= ~2u; t.refuse("ring flag", p.build(), S::invalidValue, "RECN", "ring flag"); }
    if (good.flags & 1) { auto p = good; p.flags &= ~1u; t.refuse("engine flag", p.build(), S::invalidValue, "JITC", "engine flag"); }

    // Identity: ROM size and digest, every option by name, the bus.
    {
        auto p = good; put64(p.at("IDNT"), 0, get64(p.at("IDNT"), 0) + 4);
        t.refuse("ROM size", p.build(), S::romMismatch, "IDNT", "rom");
        for (const size_t at : {size_t(8), size_t(23)}) {
            p = good; p.at("IDNT")[at] ^= 1;
            t.refuse("ROM digest", p.build(), S::romMismatch, "IDNT", "rom");
        }
        const Bytes& identity = good.sections[0].bytes;
        assert(good.sections[0].fourcc == stateFourcc("IDNT"));
        const uint32_t fields = get32(identity, 24);
        assert(fields >= 60);
        p = good; put32(p.at("IDNT"), 24, fields - 1);
        t.refuse("option count", p.build(), S::optionsMismatch, "IDNT", "option count");
        size_t at = 28;
        for (uint32_t n = 0; n < fields; ++n) {
            const uint32_t length = get32(identity, at);
            const std::string option(identity.begin() + at + 4, identity.begin() + at + 4 + length);
            const size_t value = at + 4 + length;
            for (const uint64_t change : {uint64_t(1), uint64_t(1) << 40}) {
                p = good; put64(p.at("IDNT"), value, get64(identity, value) ^ change);
                t.refuse(option.c_str(), p.build(), S::optionsMismatch, "IDNT", option.c_str());
            }
            p = good; p.at("IDNT")[at + 4] ^= 0x20;
            t.refuse("option name", p.build(), S::optionsMismatch, "IDNT", option.c_str());
            at = value + 8;
        }
        // The bus identity follows the options, then the build note.
        assert((get32(identity, at) != 0) == bus && get32(identity, at + 4) == identity.size() - at - 8);
        p = good; put32(p.at("IDNT"), at, get32(identity, at) ^ 0x10000);
        t.refuse("bus identity", p.build(), S::busMismatch, "IDNT", "bus identity");
        p = good; put32(p.at("IDNT"), at, bus ? 0 : 0x1234);
        t.refuse("bus identity", p.build(), S::busMismatch, "IDNT", "bus identity");
        p = good; put32(p.at("IDNT"), at + 4, 5000);
        t.refuse("note length", p.build(), S::invalidValue, "IDNT", "text length");
        // Another build note is only reported.
        const auto noted = imx1MachineCheckState(t.live.machine, state.data(), state.size(), 0, "another build");
        assert(noted.status == S::ok && noted.buildNoteDiffers);
        assert(!imx1MachineCheckState(t.live.machine, state.data(), state.size(), 0, "").buildNoteDiffers);
    }

    // Values out of range. PROB starts retired, work, blPrefix, blNextPc, prefix count, three
    // bools, pin, offset, I2C issue step and PC, irqRaised, fiqRaised, wire status, stop.
    {
        assert(get32(good.sections[1].bytes, 21) == 0 && good.sections[1].fourcc == stateFourcc("PROB"));
        auto p = good; p.at("PROB")[16] = 2;
        t.refuse("bool", p.build(), S::invalidValue, "PROB", "bool");
        p = good; put32(p.at("PROB"), 21, 257);
        t.refuse("prefix count", p.build(), S::invalidValue, "PROB", "interrupted prefix count");
        p = good; put32(p.at("PROB"), 50, 12);
        t.refuse("wire status", p.build(), S::invalidValue, "PROB", "enum");
        p = good; put32(p.at("PROB"), 54, 11);
        t.refuse("stop", p.build(), S::invalidValue, "PROB", "enum");
        p = good; p.at("PROB")[26] ^= 1;
        t.refuse("terminal", p.build(), S::invalidValue, "PROB", "terminal stop");
        for (const size_t line : {size_t(48), size_t(49)}) {
            p = good; p.at("PROB")[line] ^= 1;
            t.refuse("interrupt line", p.build(), S::invalidValue, "CPU ", "interrupt lines");
        }
        // BOOT starts with the reference time: ticks, numerator, denominator, then overflow and a fixed bool.
        const uint64_t denominator = get64(good.sections[6].bytes, 16);
        assert(good.sections[6].fourcc == stateFourcc("BOOT") && denominator);
        p = good; put64(p.at("BOOT"), 16, 0);
        t.refuse("zero denominator", p.build(), S::invalidValue, "BOOT", "reference time");
        p = good; put64(p.at("BOOT"), 8, denominator);
        t.refuse("fraction of one", p.build(), S::invalidValue, "BOOT", "reference time");
        p = good; put64(p.at("BOOT"), 8, UINT64_MAX);
        t.refuse("fraction above one", p.build(), S::invalidValue, "BOOT", "reference time");
        p = good; p.at("BOOT")[24] = 0x80;
        t.refuse("overflow bool", p.build(), S::invalidValue, "BOOT", "bool");
        p = good; p.at("BOOT")[25] ^= 1;
        t.refuse("fixed bool", p.build(), S::optionsMismatch, "BOOT", "fixed field");
        // Memory lengths.
        p = good; put64(p.at("SRAM"), 0, get64(p.at("SRAM"), 0) - 1);
        t.refuse("SRAM length", p.build(), S::invalidValue, "SRAM");
        p = good; put64(p.at("SRAM"), 0, uint64_t(1) << 40);
        t.refuse("SRAM length", p.build(), S::invalidValue, "SRAM", "vector length");
        p = good; put64(p.at("SDR0"), 0, 1); p.at("SDR0").resize(9);
        // More than the bank holds, which is nothing until the guest sets it up.
        t.refuse("SDRAM length", p.build(), S::invalidValue, "SDR0");
        p = good; put64(p.at("FRAM"), 0, uint64_t(1) << 21);
        t.refuse("frame length", p.build(), S::invalidValue, "FRAM", "vector length");
        if (bus) {
            assert(good.sections.size() > 15 && p.find("I2CB") && p.at("I2CB").size() == 8);
            p = good; put32(p.at("I2CB"), 0, 257);
            t.refuse("bus cursor", p.build(), S::invalidValue, "I2CB", "cursor");
            p = good; put32(p.at("I2CB"), 4, 8);
            t.refuse("bus protocol", p.build(), S::invalidValue, "I2CB", "enum");
        } else {
            p = good;
            Section extra; extra.fourcc = stateFourcc("I2CB"); extra.version = 1; extra.bytes.assign(8, 0);
            p.sections.push_back(extra);
            t.refuse("bus section without a bus", p.build(), S::invalidValue, "I2CB", "no bus");
        }
    }
    std::printf("PASS: %s, %u refused states left the machine's next save unchanged\n", name, t.refusals - start);
}

// A bus that keeps the default identity.
struct PlainBus : Imx1I2cBus {
    Imx1I2cReply transfer(const Imx1I2cTransfer&) override { return {}; }
};

void rejects() {
    I2cGuest i2c;
    const auto bytes = image(3);
    std::vector<std::string> sections;
    Bytes early, noBus;
    {
        // Fast with the diagnostic ring, so every optional section but the graphics memory is there.
        Imx1I2cReadWindow source(bytes), window(bytes);
        auto g = i2c.guest; g.ring = true; g.options.i2cBus = &source;
        {
            Live a(g, true);
            for (unsigned n = 0; n < 12; ++n) a.run(slice(7, 10000));
            early = a.save();
            assert(busProtocol(early) != 0);
        }
        // One machine at a time: the others come before and after the main one.
        auto none = g; none.options.i2cBus = nullptr;
        {
            Live y(none, true);
            Target ty(y);
            ty.refuse("state with a bus, machine without", early, S::busMismatch, "IDNT", "bus identity");
            noBus = ty.before;
        }
        g.options.i2cBus = &window;
        {
            Live m(g, true);
            // On to the second data byte, so the window is reading and its cursor has moved.
            for (unsigned n = 0; n < 400 && !(busProtocol(m.save()) == 5 && busCursor(m.save()) == 0x21); ++n)
                m.run(slice(7, 10000));
            Target t(m);
            assert(busProtocol(t.before) == 5 && busCursor(t.before) != busCursor(early));
            rejectMatrix("I2C guest, Fast, ring", t, early, true);
            for (const auto& s : Parsed(early).sections) sections.push_back(s.name());
            t.refuse("state without a bus, machine with", noBus, S::busMismatch, "IDNT", "bus identity");

            // The allow flag loads a state from another core revision and says so.
            auto p = Parsed(early); p.coreRevision += 7;
            const Bytes b = p.build();
            const auto info = imx1MachineLoadState(m.machine, b.data(), b.size(), imx1StateAllowCoreRevision);
            assert(info.status == S::ok && info.coreRevisionDiffers && info.coreRevision == imx1MachineStateCoreRevision + 7);
            assert(m.save() == early);

            // A stopped machine neither saves nor loads.
            imx1MachineStop(m.machine);
            Bytes out;
            assert(imx1MachineSaveState(m.machine, out) == S::notAtBoundary);
            assert(imx1MachineCheckState(m.machine, early.data(), early.size()).status == S::notAtBoundary);
            assert(imx1MachineLoadState(m.machine, early.data(), early.size()).status == S::notAtBoundary);
            assert(imx1MachineSaveState(nullptr, out) == S::notAtBoundary);
            assert(imx1MachineLoadState(nullptr, early.data(), early.size()).status == S::notAtBoundary);
        }
        {
            // Another image.
            auto other = bytes; other[200] ^= 1;
            Imx1I2cReadWindow changed(other);
            auto h = g; h.options.i2cBus = &changed;
            Live x(h, true);
            Target tx(x);
            tx.refuse("another image", early, S::busMismatch, "IDNT", "bus identity");
        }
        {
            // A bus without an identity.
            PlainBus plain;
            auto p = g; p.options.i2cBus = &plain;
            Live z(p, true);
            Bytes out{1, 2, 3};
            assert(imx1MachineSaveState(z.machine, out) == S::unsupportedConfiguration && out.empty());
            assert(imx1MachineCheckState(z.machine, early.data(), early.size()).status == S::unsupportedConfiguration);
            assert(imx1MachineLoadState(z.machine, early.data(), early.size()).status == S::unsupportedConfiguration);
            assert(imx1MachineRunSlice(z.machine, 7, 10000).retiredInstructions == 7);
        }
    }
    {
        // Exact with graphics memory: a display with a frame pending, offered to a machine before its setup.
        GraphicsGuest display;
        display.setup(); display.display(); display.end();
        const Guest shown{"graphics", display.words, graphicsOptions(false), false, Imx1ProbeEntry::linkedAlias};
        Bytes pending;
        {
            Live a(shown, false);
            imx1MachineRunSlice(a.machine, uint32_t(display.retired - 1), 0);
            pending = a.save();
        }
        Live m(shown, false);
        imx1MachineRunSlice(m.machine, 40, 0);
        Target t(m);
        rejectMatrix("graphics guest, Exact", t, pending, false);
        for (const auto& s : Parsed(pending).sections)
            if (std::find(sections.begin(), sections.end(), s.name()) == sections.end()) sections.push_back(s.name());
    }
    // Between them the two states held every section there is.
    for (const char* name : {"IDNT", "PROB", "CPU ", "ICAC", "DCAC", "TLB ", "BOOT", "SRAM", "SDR0", "SDR1", "CS4 ", "CS5R",
                             "CS5C", "GCMD", "GMEM", "GINT", "GEXT", "FRAM", "I2CB", "RECN", "JITC"})
        assert(std::find(sections.begin(), sections.end(), name) != sections.end());
    assert(sections.size() == 21);

    // Machines made with another ROM or other options, one option at a time.
    const auto timer = timerGuest();
    Bytes state;
    {
        Live a(timer, false);
        imx1MachineRunSlice(a.machine, 500, 1);
        state = a.save();
    }
    {
        auto changed = timer; changed.words.back() ^= 1;
        {
            Live m(changed, false);
            Target t(m);
            t.refuse("another ROM", state, S::romMismatch, "IDNT", "rom");
        }
        changed = timer; changed.words.push_back(0xe1a00000);
        Live longer(changed, false);
        Target tl(longer);
        tl.refuse("a longer ROM", state, S::romMismatch, "IDNT", "rom");
    }
    struct Change { const char* option; std::function<void(Guest&)> apply; };
    const Change changes[] = {
        {"entry", [](Guest& g) { g.entry = Imx1ProbeEntry::linkedAlias; }},
        {"ramMiB", [](Guest& g) { g.options.ramMiB = 32; }},
        {"rtcInitialTimeKnown", [](Guest& g) { g.options.rtcInitialTimeKnown = true; }},
        {"aspPx1HighFromReset", [](Guest& g) { g.options.aspPx1HighFromReset = true; }},
        {"aspPenInputs.contactKnown", [](Guest& g) { g.options.aspPenInputs = pen(false); }},
        {"aspUSampleKnown", [](Guest& g) { g.options.aspUSampleKnown = true; g.options.aspUSample = 0x1234; }},
        {"assumeI2cSingleMaster", [](Guest& g) { g.options.assumeI2cSingleMaster = true; }},
        {"arm920IcacheIndexCompatibility", [](Guest& g) { g.options.arm920IcacheIndexCompatibility = true; }},
        {"lcdcContrastOnly", [](Guest& g) { g.options.lcdcContrastOnly = true; }},
        {"mmcsdEmptySlot", [](Guest& g) { g.options.mmcsdEmptySlot = true; }},
        {"assumeMpllSettled", [](Guest& g) { g.options.assumeMpllSettled = true; }},
        {"assumeSpllSettled", [](Guest& g) { g.options.assumeSpllSettled = true; }},
        {"cs4Port0InputKnown", [](Guest& g) { g.options.cs4Port0InputKnown = true; g.options.cs4Port0Input = 0x5a; }},
        {"cs5ResetStatusInputKnown", [](Guest& g) { g.options.cs5ResetStatusInputKnown = true; }},
        {"gpioAInputKnown", [](Guest& g) { g.options.gpioAInputKnown = true; }},
        {"gpioBInputKnown", [](Guest& g) { g.options.gpioBInputKnown = true; g.options.gpioBInput = 1; }},
        {"gpioDInputKnown", [](Guest& g) { g.options.gpioDInputKnown = true; }},
        {"graphicsProfile", [](Guest& g) { g.options.graphicsProfile = Imx1GraphicsProfile::virtual5744Compatible8MiB; }},
    };
    unsigned refused = 0;
    for (const auto& change : changes) {
        auto changed = timer;
        change.apply(changed);
        Live m(changed, false);
        imx1MachineRunSlice(m.machine, 20, 1);
        Target t(m);
        t.refuse(change.option, state, S::optionsMismatch, "IDNT", change.option);
        ++refused;
    }
    std::printf("PASS: another ROM and %u machines with one option changed refuse the state by name\n", refused);
}
}
int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (bool fast : {false, true}) {
        i2cRoundTrip(fast);
        loadInPlace(fast);
        pendingFiq(fast);
        std::printf("PASS: %s I2C saves in every wait and protocol step continue identically, a state loads in place "
                    "into a machine that ran further, and a pending FIQ is kept\n", fast ? "Fast" : "Exact");
    }
    fastIntoExact();
    std::puts("PASS: Fast states continue on an Exact machine like the Exact run");
    rejects();
    std::puts("PASS: every refusal left the machine unchanged");
}
