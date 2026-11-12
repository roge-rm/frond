#ifndef FROND_MEMORY_STICK_PRO_CARD_H
#define FROND_MEMORY_STICK_PRO_CARD_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// Memory Stick Pro as the TG50's Sony driver drives it: type 1, category 0, class 0, commands
// through EX_SET_CMD (TPC 9) or SET_CMD (TPC 0xe) with the count and address in the parameter
// registers, 512-byte sectors through READ/WRITE_LONG_DATA. The media are flat sectors; the card
// does its own block mapping, so there are no extra bytes. INT is CED 0x80, ERR 0x40, BREQ 0x20,
// CMDNK 0x01.
struct MsProStore {
    std::function<bool(uint32_t sector, uint8_t* out)> read;
    std::function<bool(uint32_t sector, const uint8_t* in)> write;
    uint32_t sectors{};
    // READ_ATRB data, whole 512-byte sectors, built for this card's size.
    std::vector<uint8_t> attribute;
    // FORMAT: writes the factory file system; none leaves the sectors as they are.
    std::function<bool()> format;
    // Frees the card store behind the stick, when a session load leaves the slot empty.
    std::function<void()> release;
};

class MemoryStickProCard {
 public:
    enum : uint8_t { CED = 0x80, ERR = 0x40, BREQ = 0x20, CMDNK = 0x01 };
    enum : uint32_t { Idle = 0, Reading = 1, Writing = 2, Attribute = 3 };
    enum : uint8_t { ReadData = 0x20, WriteData = 0x21, ReadAttribute = 0x24, Stop = 0x25, Format = 0x10 };
    struct State {
        std::array<uint8_t, 32> reg{};
        std::array<uint8_t, 512> page{};
        uint32_t readStart{}, readSize{8}, writeStart{16}, writeSize{8};
        // Transfer in progress: the next sector's address, sectors left (0 with `open` is until STOP),
        // and whether `page` holds the next sector to read.
        uint32_t operation{}, address{}, remaining{}, open{}, loaded{};
    } state;
    MsProStore store;
    bool writeProtected{};

    bool Present() const { return store.sectors != 0; }
    uint32_t Sectors() const { return store.sectors; }
    uint32_t AttributeSectors() const { return uint32_t(store.attribute.size() / 512); }
    void Attach(MsProStore s, bool protect) { store = std::move(s); writeProtected = protect; Reset(); }
    void Detach() { store = {}; writeProtected = false; Reset(); }

    void Reset() {
        state = {};
        state.reg[1] = CED;
        state.reg[2] = writeProtected ? 1 : 0;
        state.reg[4] = 0x01;  // type: Pro
        state.reg[6] = 0x00;  // category: storage
        state.reg[7] = 0x00;  // class: read/write
        state.reg[16] = 0x80; // system parameter: serial interface
    }
    uint8_t Interrupt() const { return state.reg[1]; }
    void Interrupt(uint8_t value) { state.reg[1] = value; }

    static uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

    bool Load() {
        if (state.operation == Attribute) {
            if (state.address >= AttributeSectors()) return false;
            std::copy_n(store.attribute.begin() + size_t(state.address) * 512, 512, state.page.begin());
        } else if (state.address >= store.sectors || !store.read || !store.read(state.address, state.page.data())) {
            return false;
        }
        state.loaded = 1;
        return true;
    }
    void Finish(uint8_t interrupt) { state.operation = Idle; state.remaining = state.open = state.loaded = 0; Interrupt(interrupt); }
    // After a sector moved: the next one, or the end of the command.
    void Advance() {
        ++state.address;
        state.loaded = 0;
        if (!state.open && !--state.remaining) { Finish(CED); return; }
        const uint32_t limit = state.operation == Attribute ? AttributeSectors() : store.sectors;
        if (state.address >= limit) { Finish(CED | ERR); return; }
        if (state.operation != Writing && !Load()) { Finish(CED | ERR); return; }
        Interrupt(BREQ);
    }

    bool Command(uint8_t command) {
        if (!Present()) return false;
        const uint32_t count = (uint32_t(state.reg[0x11]) << 8) | state.reg[0x12];
        const uint32_t address = Be32(&state.reg[0x13]);
        switch (command) {
         case ReadData: case WriteData: case ReadAttribute: {
            Finish(CED);
            const uint32_t limit = command == ReadAttribute ? AttributeSectors() : store.sectors;
            if (address >= limit || (count && count > limit - address)) { Interrupt(CED | ERR); return true; }
            if (command == WriteData && writeProtected) { Interrupt(CED | ERR); return true; }
            state.operation = command == ReadData ? Reading : command == WriteData ? Writing : Attribute;
            state.address = address;
            state.remaining = count;
            state.open = count == 0;
            if (state.operation != Writing && !Load()) { Finish(CED | ERR); return true; }
            Interrupt(BREQ);
            return true;
         }
         case Stop:
            // The sector already requested still moves; the command ends after it.
            if (state.operation == Idle) Interrupt(CED);
            else { state.open = 0; state.remaining = 1; Interrupt(BREQ); }
            return true;
         case Format:
            // A quick format back to the factory file system; the Sony update needs the stick to do this.
            Finish(writeProtected || (store.format && !store.format()) ? CED | ERR : CED);
            return true;
         default:
            Finish(CED | CMDNK);
            return true;
        }
    }

    bool TpcWrite(uint8_t tpc, const uint8_t* in, size_t size) {
        switch (tpc) {
         case 8:
            if (size != 4 || in[0] + unsigned(in[1]) > 32 || in[2] + unsigned(in[3]) > 32) return false;
            state.readStart = in[0]; state.readSize = in[1]; state.writeStart = in[2]; state.writeSize = in[3];
            return true;
         case 0xb:
            if (size != state.writeSize || state.writeStart > 32 || size > 32 - state.writeStart) return false;
            // Only the parameter registers take writes.
            for (size_t i = 0; i < size; ++i)
                if (state.writeStart + i >= 16 && state.writeStart + i < 32) state.reg[state.writeStart + i] = in[i];
            return true;
         case 9:
            if (size != 7) return false;
            std::copy_n(in + 1, 6, state.reg.begin() + 0x11);
            return Command(in[0]);
         case 0xe:
            return size == 1 && Command(*in);
         case 0xd:
            if (size != 512 || state.operation != Writing || Interrupt() != BREQ) return false;
            if (!store.write || !store.write(state.address, in)) { Finish(CED | ERR); return true; }
            Advance();
            return true;
         default:
            return false;
        }
    }
    bool TpcRead(uint8_t tpc, uint8_t* out, size_t size) {
        switch (tpc) {
         case 4:
            if (size != state.readSize || state.readStart > 32 || size > 32 - state.readStart) return false;
            state.reg[2] = (state.reg[2] & ~1) | (writeProtected ? 1 : 0);
            std::copy_n(state.reg.begin() + state.readStart, size, out);
            return true;
         case 7:
            if (size != 1) return false;
            *out = Interrupt();
            return true;
         case 2:
            if (size != 512 || (state.operation != Reading && state.operation != Attribute) || !state.loaded) return false;
            std::copy(state.page.begin(), state.page.end(), out);
            return true;
         default:
            return false;
        }
    }
    void FinishRead(uint8_t tpc) {
        if (tpc == 2 && (state.operation == Reading || state.operation == Attribute) && state.loaded) Advance();
    }
    // Raises the controller's card interrupt after this TPC.
    static bool Event(uint8_t tpc) { return tpc == 9 || tpc == 0xe || tpc == 2 || tpc == 0xd; }

    // A saved state must address this card.
    static bool Valid(const State& s, uint32_t sectors, uint32_t attributeSectors) {
        if (s.readStart > 32 || s.readSize > 32 - s.readStart || s.writeStart > 32 || s.writeSize > 32 - s.writeStart)
            return false;
        if (s.operation > Attribute || s.open > 1 || s.loaded > 1 || s.reg[4] != 1 || s.reg[6] || s.reg[7]) return false;
        if (s.operation == Idle) return !s.remaining && !s.open && !s.loaded;
        const uint32_t limit = s.operation == Attribute ? attributeSectors : sectors;
        if (s.address >= limit || (s.open && s.remaining) || (!s.open && !s.remaining)) return false;
        if (!s.open && s.remaining > limit - s.address) return false;
        return s.operation != Writing || !s.loaded;
    }
};

#endif
