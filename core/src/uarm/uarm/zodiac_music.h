#ifndef FROND_ZODIAC_MUSIC_H
#define FROND_ZODIAC_MUSIC_H

// The Zodiac's music hooks: code hooks at the ROM's Tapwave Midi Library (type 'libr', creator 'twMD') entries
// catch a game's SMF and real-time MIDI calls and turn them into host events, timed in guest time. The hooks only
// read registers and guest memory; the ROM runs every call as it would without them. Nothing here touches the
// machine: guest memory comes through the caller's read function.
//
// ROM facts (1.1; zodiac MIDI out design in the plan): the library runs in place from ROM, its code resource
// starts with a branch, then the entry table's offset, its globals' size, a version, the entry count (21) and
// 'libr' 'twMD' 'amdc'. Every entry is a B to its function, which the TwGlue stubs reach through the table and
// TwMidiPlaySmf (entry 80) with bl, so a hook at each function catches both ways in.
// TwSmfOpen(&handle, smf, outP or 0): on success the handle goes to [r0] (a 24-byte object, magic ff051962 at
// +8, driver id at +12, playing flag at +13). TwSmfPlay(handle, rangeP or 0, byteP or 0, callbackP or 0).
// TwSmfStop(handle), TwSmfClose(handle). TwMidiSetMasterVolume(volume, 0-127). The real-time calls pass r1-r3
// to the DAL driver unchanged; their order (channel, note or controller, velocity or value) is the Tapwave SDK's,
// not checked against a game: none of the 58 calls them.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "zodiac_smf.h"

namespace zodiac_music {

// The calls hooked, with their offset in the entry table.
enum class Call : uint8_t {
    smfOpen, smfClose, smfPlay, smfStop, noteOn, noteOff, programChange, pitchBend, sysEx, setMasterVolume,
    controlChange
};
constexpr unsigned callCount = 11;
constexpr uint32_t callOffsets[callCount] = {0, 4, 8, 16, 28, 32, 36, 40, 44, 48, 60};
constexpr uint32_t libraryEntries = 21;
// An SMF bigger than this isn't copied (the games' biggest is under 100 KB).
constexpr uint32_t maxSmfBytes = 1u << 20;
constexpr uint32_t maxSysExBytes = 4096;
// Events kept for the player; older ones go first when it falls behind.
constexpr size_t maxEvents = 4096;
constexpr size_t maxOpens = 16, maxHandles = 32;

struct Library {
    uint32_t header = 0, table = 0;
    uint32_t pcs[callCount]{};  // each call's function, ARM
};

inline uint32_t le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// Finds twMD's code resource in a ROM image whose byte 0 is at guest address base. False, with out untouched,
// unless the header, the 21 branches and an STMDB sp! at each hooked function are all there.
inline bool findLibrary(const uint8_t* rom, size_t size, uint32_t base, Library& out) {
    constexpr uint32_t libr = 0x6c696272, twMD = 0x74774d44, amdc = 0x616d6463;
    for (size_t at = 0; at + 32 <= size; at += 4) {
        if (le32(rom + at + 20) != libr || le32(rom + at + 24) != twMD || le32(rom + at + 28) != amdc) continue;
        const uint32_t offset = le32(rom + at + 4);
        if ((rom[at + 3] != 0xea) || le32(rom + at + 16) != libraryEntries || offset & 3 || offset < 32 ||
            offset > size - at || size - at - offset < libraryEntries * 4)
            continue;
        Library found;
        found.header = base + uint32_t(at);
        found.table = found.header + offset;
        bool good = true;
        for (unsigned i = 0; i < callCount && good; ++i) {
            const size_t entry = at + offset + callOffsets[i];
            const uint32_t word = le32(rom + entry);
            if ((word >> 24) != 0xea) { good = false; break; }
            const int32_t delta = int32_t(word << 8) >> 6;
            const int64_t target = int64_t(entry) + 8 + delta;
            if (target < 0 || uint64_t(target) + 4 > size || (le32(rom + target) & 0xffff0000) != 0xe92d0000) {
                good = false;
                break;
            }
            found.pcs[i] = base + uint32_t(target);
        }
        for (uint32_t i = 0; i < libraryEntries && good; ++i) good = rom[at + offset + i * 4 + 3] == 0xea;
        if (!good) continue;
        out = found;
        return true;
    }
    return false;
}

// A file the guest opened: its bytes, copied at TwSmfOpen, and the song read from them.
struct Smf {
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
    zodiac_smf::Song song;
};

enum class Kind : uint8_t {
    play,          // handle, smf, range, flags, callback (r1-r3 as given)
    stop,          // handle
    close,         // handle
    message,       // handle (TwMidi's), bytes[0..length)
    sysEx,         // handle, data
    masterVolume,  // value
    reset,         // the machine was reset or loaded: every sound stops
};

struct Event {
    uint64_t timeNs = 0;
    Kind kind = Kind::reset;
    uint32_t handle = 0;
    std::shared_ptr<const Smf> smf;
    uint32_t range = 0, flags = 0, callback = 0, value = 0;
    uint8_t bytes[3]{};
    uint8_t length = 0;
    std::vector<uint8_t> data;
};

struct Stats {
    uint64_t calls = 0, opens = 0, badFiles = 0, unmatchedPlays = 0, dropped = 0;
};

// Reads size bytes at a guest address; false when any of them can't be read.
using Read = std::function<bool(uint32_t address, void* data, uint32_t size)>;

// The bytes of the SMF at address, walked by its chunk headers through guest reads: header, then chunks until the
// header's track count is found. 0 when it isn't an SMF, can't be read, or passes maxSmfBytes.
inline uint32_t guestSmfLength(uint32_t address, const Read& read) {
    uint8_t head[14];
    if (!read(address, head, 14) || zodiac_smf::be32(head) != 0x4d546864) return 0;
    const uint32_t headerSize = zodiac_smf::be32(head + 4);
    if (headerSize < 6 || headerSize > maxSmfBytes) return 0;
    const uint16_t tracks = zodiac_smf::be16(head + 10);
    uint64_t at = 8 + uint64_t(headerSize);
    for (uint32_t found = 0; found < tracks;) {
        uint8_t chunk[8];
        if (at + 8 > maxSmfBytes || !read(address + uint32_t(at), chunk, 8)) return 0;
        at += 8 + uint64_t(zodiac_smf::be32(chunk + 4));
        if (at > maxSmfBytes) return 0;
        if (zodiac_smf::be32(chunk) == 0x4d54726b) ++found;
    }
    return uint32_t(at);
}

// The hooks' host side. hit takes each code hook stop at one of the library's pcs; the events wait in order for
// the player (take). Host only, never saved: a reset or load calls reset, which drops the opens and handles.
class Hooks {
   public:
    // False for a pc that isn't one of the library's. Never changes the guest.
    bool hit(const Library& library, uint32_t pc, const uint32_t (&r)[16], uint64_t timeNs, const Read& read) {
        unsigned call = 0;
        while (call < callCount && library.pcs[call] != (pc & ~1u)) ++call;
        if (call == callCount) return false;
        ++stats_.calls;
        switch (Call(call)) {
            case Call::smfOpen: open(r[0], r[1], read); break;
            case Call::smfPlay: {
                const auto smf = bound(r[0], read);
                if (!smf) { ++stats_.unmatchedPlays; break; }
                Event e = at(timeNs, Kind::play, r[0]);
                e.smf = smf;
                e.range = r[1]; e.flags = r[2]; e.callback = r[3];
                push(std::move(e));
                break;
            }
            case Call::smfStop:
                if (bound(r[0], read)) push(at(timeNs, Kind::stop, r[0]));
                break;
            case Call::smfClose:
                if (bound(r[0], read)) push(at(timeNs, Kind::close, r[0]));
                forget(r[0], read);
                break;
            case Call::noteOn: message(timeNs, r[0], 0x90 | (r[1] & 15), r[2], r[3], 3); break;
            case Call::noteOff: message(timeNs, r[0], 0x80 | (r[1] & 15), r[2], r[3], 3); break;
            case Call::programChange: message(timeNs, r[0], 0xc0 | (r[1] & 15), r[2], 0, 2); break;
            case Call::controlChange: message(timeNs, r[0], 0xb0 | (r[1] & 15), r[2], r[3], 3); break;
            case Call::pitchBend: {
                // The value's range isn't known: 14 bits are sent as is, centre 0x2000.
                Event e = at(timeNs, Kind::message, r[0]);
                e.bytes[0] = uint8_t(0xe0 | (r[1] & 15));
                e.bytes[1] = uint8_t(r[2] & 0x7f);
                e.bytes[2] = uint8_t((r[2] >> 7) & 0x7f);
                e.length = 3;
                e.value = r[2];
                push(std::move(e));
                break;
            }
            case Call::sysEx: {
                // TwMidiSysEx(handle, data, length), as the SDK has it.
                if (!r[1] || !r[2] || r[2] > maxSysExBytes) break;
                Event e = at(timeNs, Kind::sysEx, r[0]);
                e.data.resize(r[2]);
                if (!read(r[1], e.data.data(), r[2])) break;
                push(std::move(e));
                break;
            }
            case Call::setMasterVolume: {
                Event e = at(timeNs, Kind::masterVolume, 0);
                e.value = r[0];
                push(std::move(e));
                break;
            }
        }
        return true;
    }

    // A reset or load: the guest's handles are gone, and the player stops every sound.
    void reset(uint64_t timeNs) {
        opens_.clear();
        handles_.clear();
        push(at(timeNs, Kind::reset, 0));
    }

    bool take(Event& e) {
        if (events_.empty()) return false;
        e = std::move(events_.front());
        events_.pop_front();
        return true;
    }
    size_t pending() const { return events_.size(); }
    const Stats& stats() const { return stats_; }

   private:
    struct Open { uint32_t handleAt; std::shared_ptr<const Smf> smf; };
    struct Handle { uint32_t handle; std::shared_ptr<const Smf> smf; };
    std::vector<Open> opens_;
    std::vector<Handle> handles_;
    std::deque<Event> events_;
    Stats stats_;

    static Event at(uint64_t timeNs, Kind kind, uint32_t handle) {
        Event e;
        e.timeNs = timeNs;
        e.kind = kind;
        e.handle = handle;
        return e;
    }
    void push(Event&& e) {
        if (events_.size() >= maxEvents) { events_.pop_front(); ++stats_.dropped; }
        events_.push_back(std::move(e));
    }
    void message(uint64_t timeNs, uint32_t handle, uint32_t status, uint32_t a, uint32_t b, uint8_t length) {
        // Data bytes keep up to 8 bits; the player clamps them.
        Event e = at(timeNs, Kind::message, handle);
        e.bytes[0] = uint8_t(status);
        e.bytes[1] = uint8_t(a > 0xff ? 0xff : a);
        e.bytes[2] = length > 2 ? uint8_t(b > 0xff ? 0xff : b) : 0;
        e.length = length;
        push(std::move(e));
    }
    // TwSmfOpen's entry: copies the file. The handle isn't known until the call writes it to handleAt, so the
    // open waits for the Play whose handle is there. A file that can't be read or parsed opens nothing here.
    void open(uint32_t handleAt, uint32_t address, const Read& read) {
        if (!handleAt || !address) return;
        for (size_t i = 0; i < opens_.size(); ++i)
            if (opens_[i].handleAt == handleAt) { opens_.erase(opens_.begin() + long(i)); break; }
        auto smf = std::make_shared<Smf>();
        smf->address = address;
        const uint32_t length = guestSmfLength(address, read);
        smf->bytes.resize(length);
        if (!length || !read(address, smf->bytes.data(), length) ||
            zodiac_smf::smfLength(smf->bytes.data(), length) != length ||
            zodiac_smf::parseSmf(smf->bytes.data(), length, smf->song) != zodiac_smf::Error::none) {
            ++stats_.badFiles;
            return;
        }
        ++stats_.opens;
        if (opens_.size() >= maxOpens) opens_.erase(opens_.begin());
        opens_.push_back({handleAt, std::move(smf)});
    }
    // The file behind a handle: one played before, or the open whose handle word now holds it.
    std::shared_ptr<const Smf> bound(uint32_t handle, const Read& read) {
        if (!handle) return nullptr;
        for (const auto& h : handles_)
            if (h.handle == handle) return h.smf;
        for (size_t i = opens_.size(); i-- > 0;) {
            uint32_t word = 0;
            if (!read(opens_[i].handleAt, &word, 4) || word != handle) continue;
            auto smf = opens_[i].smf;
            opens_.erase(opens_.begin() + long(i));
            if (handles_.size() >= maxHandles) handles_.erase(handles_.begin());
            handles_.push_back({handle, smf});
            return smf;
        }
        return nullptr;
    }
    void forget(uint32_t handle, const Read& read) {
        for (size_t i = 0; i < handles_.size(); ++i)
            if (handles_[i].handle == handle) { handles_.erase(handles_.begin() + long(i)); break; }
        for (size_t i = opens_.size(); i-- > 0;) {
            uint32_t word = 0;
            if (read(opens_[i].handleAt, &word, 4) && word == handle) opens_.erase(opens_.begin() + long(i));
        }
    }
};

}  // namespace zodiac_music

#endif
