#ifndef FROND_MICROWIRE64X16_H
#define FROND_MICROWIRE64X16_H

#include <array>
#include <cstdint>

class Microwire64x16 {
 public:
    enum Phase : uint32_t { start, command, read, write, writeAll, done, aborted };
    enum Operation : uint32_t { none, programWord, eraseWord, programAll, eraseAll };
    // Serial state and write time use integers so untrusted snapshots can be checked before adoption.
    struct State {
        std::array<uint16_t, 64> words{};
        uint32_t selected{}, clock{}, enabled{}, phase{}, commandBits{}, commandValue{};
        uint32_t address{}, readBit{}, dataBits{}, data{}, queued{}, polling{};
        uint32_t pending{}, pendingAddress{}, pendingValue{};
        uint64_t remainingNs{};
    };
    struct Output { bool driven, high; };
    // Vendor maximum write-cycle time; this is a bounded model, not measured TG50 timing.
    static constexpr uint64_t writeTimeNs = 6000000;
    static constexpr uint64_t writeAllTimeNs = 15000000;

    Microwire64x16() { state.words.fill(0xffff); }
    const State& Save() const { return state; }
    bool Load(const State& candidate) {
        if (!Valid(candidate)) return false;
        state = candidate;
        return true;
    }
    static bool Valid(const State& s) {
        if (s.selected > 1 || s.clock > 1 || s.enabled > 1 || s.polling > 1 ||
            s.phase > aborted || s.commandBits > 9 || s.commandValue > 0x1ff ||
            s.address >= 64 || s.readBit > 16 || s.dataBits > 16 || s.data > 0xffff ||
            s.queued > eraseAll || s.pending > eraseAll || s.pendingAddress >= 64 ||
            s.pendingValue > 0xffff || s.remainingNs >
                (s.pending == programAll ? writeAllTimeNs : writeTimeNs)) return false;
        if (bool(s.remainingNs) != bool(s.pending)) return false;
        if (!s.pending && (s.pendingAddress || s.pendingValue)) return false;
        if (!s.selected && (s.phase != start || s.commandBits || s.commandValue ||
                            s.readBit || s.dataBits || s.data || s.queued || s.polling)) return false;
        if (s.phase == start && (s.commandBits || s.commandValue || s.address || s.readBit || s.dataBits || s.data || s.queued)) return false;
        if (s.phase == command && (s.address || !s.commandBits || s.commandBits >= 9 ||
                                   s.commandValue < (1u << (s.commandBits - 1)) ||
                                   s.commandValue >= (1u << s.commandBits))) return false;
        if (s.phase >= read && s.phase <= done &&
            (s.commandBits != 9 || s.commandValue < 0x100)) return false;
        if (s.phase != read && s.readBit) return false;
        if (s.phase != write && s.phase != writeAll && s.phase != done && (s.dataBits || s.data)) return false;
        if (s.dataBits < 16 && s.data >= (1u << s.dataBits)) return false;
        if ((s.phase == write || s.phase == writeAll) && s.dataBits == 16) return false;
        if (s.queued && (s.phase != done || !s.enabled || s.pending)) return false;
        if ((s.queued == programWord || s.queued == programAll) && s.dataBits != 16) return false;
        if (s.polling && (s.phase != start || s.commandBits || s.queued)) return false;
        if (s.phase == aborted && (s.commandBits || s.commandValue || s.address ||
                                   s.readBit || s.dataBits || s.data || s.queued || s.polling)) return false;
        if (s.phase == read && ((s.commandValue >> 6) != 6 || s.dataBits || s.data || s.queued)) return false;
        if (s.phase == write && ((s.commandValue >> 6) != 5 || s.address != (s.commandValue & 63))) return false;
        if (s.phase == writeAll && ((s.commandValue & 0x1f0) != 0x110 || s.address != (s.commandValue & 63))) return false;
        if (s.phase == done && s.commandValue) {
            const auto op = (s.commandValue >> 6) & 3;
            const auto control = (s.commandValue & 63) >> 4;
            if (s.address != (s.commandValue & 63) || op == 2) return false;
            if (op == 0 && ((control == 0 && s.enabled) || (control == 3 && !s.enabled))) return false;
            if ((op == 1 || (op == 0 && control == 1)) && s.dataBits != 16) return false;
            if ((op == 3 || (op == 0 && control != 1)) && (s.dataBits || s.data)) return false;
            const auto expected = op == 1 ? programWord : op == 3 ? eraseWord :
                                  control == 1 ? programAll : control == 2 ? eraseAll : none;
            if (s.queued != (s.enabled ? expected : none)) return false;
        }
        if (s.pending && (s.queued || (!s.polling && s.selected) || !s.enabled)) return false;
        if ((s.pending == eraseWord || s.pending == eraseAll) && s.pendingValue) return false;
        return true;
    }

    Output DataOut() const {
        if (!state.selected) return {false, true};
        if (state.polling) return {true, state.remainingNs == 0};
        if (state.phase != read) return {false, true};
        return {true, state.readBit != 0 && bool(state.words[state.address] & (1u << (16 - state.readBit)))};
    }
    void Pins(bool selected, bool clock, bool driven, bool data) {
        if (!selected) {
            if (state.selected && state.queued) {
                state.pending = state.queued;
                state.pendingAddress = state.address;
                state.pendingValue = state.data;
                state.remainingNs = state.queued == programAll ? writeAllTimeNs : writeTimeNs;
            }
            ClearSerial();
            state.clock = clock;
            return;
        }
        if (!state.selected) {
            ClearSerial();
            state.selected = 1;
            state.clock = clock;
            state.polling = state.remainingNs != 0;
            return;
        }
        const bool rising = !state.clock && clock;
        state.clock = clock;
        if (!rising || state.polling || state.remainingNs) return;
        if (state.phase == read) {
            if (state.readBit == 16) {
                state.address = (state.address + 1) & 63;
                state.readBit = 1;
            } else ++state.readBit;
            return;
        }
        if (state.phase == done || state.phase == aborted) return;
        if (!driven) {
            ClearSerial();
            state.selected = 1;
            state.phase = aborted;
            return;
        }
        if (state.phase == start) {
            if (data) { state.phase = command; state.commandValue = 1; state.commandBits = 1; }
        } else if (state.phase == command) {
            state.commandValue = (state.commandValue << 1) | data;
            if (++state.commandBits == 9) Decode();
        } else {
            state.data = (state.data << 1) | data;
            if (++state.dataBits == 16) {
                state.queued = state.enabled ? (state.phase == write ? programWord : programAll) : none;
                state.phase = done;
            }
        }
    }
    void Advance(uint64_t ns) {
        if (!state.remainingNs) return;
        if (ns < state.remainingNs) { state.remainingNs -= ns; return; }
        if (state.pending == programWord) state.words[state.pendingAddress] = state.pendingValue;
        else if (state.pending == eraseWord) state.words[state.pendingAddress] = 0xffff;
        else if (state.pending == programAll) state.words.fill(state.pendingValue);
        else if (state.pending == eraseAll) state.words.fill(0xffff);
        state.remainingNs = 0;
        state.pending = state.pendingAddress = state.pendingValue = 0;
    }
    // CPU reset releases the serial bus; committed EEPROM words remain nonvolatile.
    void ResetPins() { Pins(false, false, false, false); }
    // Interrupted programming keeps the previous word; physical power-failure contents are unspecified.
    void PowerCycle() {
        ClearSerial();
        state.enabled = state.pending = state.pendingAddress = state.pendingValue = 0;
        state.remainingNs = 0;
        state.clock = 0;
    }

 private:
    State state;
    void ClearSerial() {
        state.selected = state.phase = state.commandBits = state.commandValue = 0;
        state.address = state.readBit = state.dataBits = state.data = state.queued = state.polling = 0;
    }
    void Decode() {
        state.address = state.commandValue & 63;
        const auto op = (state.commandValue >> 6) & 3;
        if (op == 2) state.phase = read;
        else if (op == 1) state.phase = write;
        else if (op == 3) {
            state.phase = done;
            state.queued = state.enabled ? eraseWord : none;
        } else {
            const auto control = state.address >> 4;
            state.phase = done;
            if (control == 0) state.enabled = 0;
            else if (control == 3) state.enabled = 1;
            else if (control == 1) state.phase = writeAll;
            else state.queued = state.enabled ? eraseAll : none;
        }
    }
};
#endif
