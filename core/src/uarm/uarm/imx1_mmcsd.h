#ifndef FROND_IMX1_MMCSD_H
#define FROND_IMX1_MMCSD_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vSD.h"

enum class Imx1MmcsdAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput, sequence };

// MC9328MX1RM 7.2 and 20.6: startup, the virtual slot's command timing and, with a card attached,
// the card's commands, responses and block data. The data goes through a DMA channel the board runs
// (Imx1Bootstrap), 16-bit halfwords of the FIFO taken as the card's bytes in order.
class Imx1Mmcsd {
   public:
    void setEmptySlot(bool enabled) { emptySlot = enabled; }
    // The card in the virtual slot, or null for none. Only with the virtual slot. Attaching the card already
    // in keeps the command and transfer under way. After a state load with the card's section (stateCard)
    // the next attach takes that state on into the card if it is the same size, and drops it otherwise.
    // Returns true when the slot carries on, false when it starts again with no command or data.
    bool attachCard(VSD* card) {
        card = emptySlot ? card : nullptr;
        const bool resuming = resume.valid;
        resume.valid = false;
        if (!resuming && card == slot.card) return true;
        slot.card = card;
        // Through pointers, so only the code that attaches a card links vSD.
        slot.command = vsdCommand;
        slot.fromCard = vsdDataXferBlockFromCard;
        slot.toCard = vsdDataXferBlockToCard;
        slot.saveCard = vsdMachineSave;
        const bool resumed = resuming && card && vsdMachineLoad(card, resume.card.data(), resume.card.size());
        resume.card.clear();
        if (resumed) return true;
        slot.responseCount = slot.responseRead = 0;
        endData();
        return false;
    }
    VSD* card() const { return slot.card; }
    bool interruptKnown() const { return emptySlot; }
    bool interruptOutput() const { return emptySlot && interrupt; }
    bool clockRunning() const { return running; }
    bool canInterrupt() const {
        if (!emptySlot || !(control & 4)) return false;
        if (slot.dataPhase) return !(registers[9] & dataMask());
        return remaining && (!(registers[9] & 4) || (slot.data && !(registers[9] & dataMask())));
    }
    // The data phase's clocks have passed and the bytes wait for the DMA.
    bool dataWaiting() const { return slot.dataWaiting; }
    bool dataWrite() const { return slot.dataWrite; }
    uint32_t dataLeft() const { return slot.dataBytes - slot.dataDone; }
    // Moves up to bytes of the waiting data between the card and memory. A failed card access ends the
    // transfer with a CRC error. Returns the bytes moved.
    uint32_t moveData(uint8_t* bytes, uint32_t size) {
        if (!slot.dataWaiting) return 0;
        const uint32_t length = registers[6];
        uint32_t moved = 0;
        while (moved < size && slot.dataDone < slot.dataBytes) {
            const uint32_t offset = slot.dataDone % length;
            if (!slot.dataWrite && !offset &&
                (!slot.card || slot.fromCard(slot.card, slot.block, length) != SdDataOk)) return failData(moved);
            const uint32_t n = std::min(size - moved, length - offset);
            if (slot.dataWrite) std::memcpy(slot.block + offset, bytes + moved, n);
            else std::memcpy(bytes + moved, slot.block + offset, n);
            moved += n;
            slot.dataDone += n;
            if (slot.dataWrite && offset + n == length &&
                (!slot.card || slot.toCard(slot.card, slot.block, length) != SdDataOk)) return failData(moved);
        }
        if (slot.dataDone == slot.dataBytes) {
            // DATA_TRANS_DONE, and WRITE_OP_DONE after a write: the virtual card programs at once.
            status |= slot.dataWrite ? 0x1800 : 0x800;
            if (!(registers[9] & dataMask())) interrupt = true;
            endData();
        }
        return moved;
    }
    // PERCLK2 cycles until the clock stop or command in progress ends, 0 when neither runs.
    uint64_t idleQuietPerclk2() const {
        if (!running || !(control & 4)) return 0;
        static constexpr unsigned prescalers[]{1,2,3,3,4,5,5,5};
        const uint64_t divider = prescalers[registers[2] >> 3] << (registers[2] & 7);
        const uint64_t ticks = stopRemaining ? stopRemaining : remaining;
        if (!ticks) return 0;
        return phase < divider ? ticks * divider - phase : 1;
    }
    void advancePerclk2(uint64_t clocks) {
        if (!running || !(control & 4)) return;
        static constexpr unsigned prescalers[]{1,2,3,3,4,5,5,5};
        const unsigned divider = prescalers[registers[2] >> 3] << (registers[2] & 7);
        const uint64_t fraction = clocks % divider + phase;
        const uint64_t ticks = clocks / divider + fraction / divider;
        phase = fraction % divider;
        if (stopRemaining) {
            if (ticks < stopRemaining) stopRemaining -= unsigned(ticks);
            else { stopRemaining = 0; running = false; phase = 0; }
        } else if (remaining) {
            if (ticks < remaining) { remaining -= unsigned(ticks); return; }
            const uint64_t left = ticks - remaining;
            remaining = 0;
            if (slot.dataPhase) {
                // The block clocks have passed; the bytes wait for the DMA.
                slot.dataPhase = false;
                slot.dataWaiting = true;
                return;
            }
            // Completion is the transmitter event; an absent card supplies no response bytes.
            status = 0x2000 | (responseExpected ? 2 : 0);
            interrupt = !(registers[9] & 4);
            if (slot.data && !responseExpected) {
                slot.dataPhase = true;
                remaining = slot.dataClocks;
                if (left >= remaining) { remaining = 0; slot.dataPhase = false; slot.dataWaiting = true; }
                else remaining -= unsigned(left);
            }
        }
    }
    static const char* registerName(uint32_t address) {
        if ((address & ~3u) == 0x00210000) return "AIPI2.PSR0";
        if ((address & ~3u) == 0x00210004) return "AIPI2.PSR1";
        static constexpr const char* names[]{"MMCSD.STR_STP_CLK", "MMCSD.STATUS", "MMCSD.CLK_RATE",
            "MMCSD.CMD_DAT_CONT", "MMCSD.RES_TO", "MMCSD.READ_TO", "MMCSD.BLK_LEN",
            "MMCSD.NOB", "MMCSD.REV_NO", "MMCSD.INT_MASK", "MMCSD.CMD", "MMCSD.ARGH",
            "MMCSD.ARGL", "MMCSD.RES_FIFO", "MMCSD.BUFFER_ACCESS"};
        if (address >= 0x00214000 && address < 0x0021403c) return names[(address - 0x00214000) / 4];
        return nullptr;
    }
    Imx1MmcsdAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1MmcsdAccess;
        if (!registerName(address)) return A::unmapped;
        if (size != 4 || (address & 3)) return A::width;
        if (address == 0x00210000 || address == 0x00210004) {
            auto& psr = address == 0x00210000 ? psr0 : psr1;
            if (!write) { value = psr; return A::ok; }
            // Read-only fields retain their reset values; other devices stay guarded.
            const uint32_t writable = value & 0xfffe;
            if (((writable ^ psr) & 0xffee) || ((control & 4) && ((writable ^ psr) & 0x10)))
                return A::sideEffect;
            psr = (psr & 0xffff0001) | writable;
            return A::ok;
        }
        // Only the native 32-bit AIPI2 MMCSD bus setting is qualified.
        if ((psr0 & 0x10) || !(psr1 & 0x10)) return A::sideEffect;
        const unsigned index = (address - 0x00214000) / 4;
        if (index == 0) {
            if (!write) { value = control; return A::ok; }
            if ((value & ~0x2fu) || (value & 3) == 3) return A::reservedBits;
            const bool reset = value & 8, enable = value & 4, stop = value & 1;
            if (reset) {
                if (!enable && !stop && !(value & 0x20)) {
                    resetRegisters(); resetAsserted = true; return A::ok;
                }
                if (!resetAsserted || !enable || !stop || (value & 0x20)) return A::sequence;
                control = 4; resetAsserted = false; recovery = 8; ready = false;
                return A::ok;
            }
            if (resetAsserted) return A::sequence;
            if (recovery) {
                if (!enable || !stop || (value & 0x20)) return A::sequence;
                --recovery; ready = !recovery;
            } else if (enable && !ready) return A::sequence;
            // Disabling halts internal clocks without resetting the command or stop tail.
            if (!value) { control = 0; return A::ok; }
            if (value & 2) {
                if (!emptySlot) return A::externalInput;
                if (value != 6) return A::sideEffect;
                if (!enable || !ready || running || staged != 15) return A::sequence;
                if (slot.card) return launchCardCommand();
                const uint32_t command = registers[10], format = registers[3];
                const bool resetCommand = command == 0 && format == 0x80;
                const bool responseCommand = (command == 55 && format == 1) ||
                    (command == 1 && format == 3);
                if (!resetCommand && !responseCommand) return A::sideEffect;
                if (responseCommand && !registers[4]) return A::sideEffect;
                responseExpected = responseCommand;
                remaining = 48 + (resetCommand ? 80 : registers[4]);
                staged = 0; status = 0; interrupt = false; phase = 0;
                running = true; control = 4;
                return A::ok;
            }
            if (running) {
                if (value == 4) { control = 4; return A::ok; }
                // Stopping an active transfer remains outside the command-only subset.
                if (!enable || !stop || remaining || (value & 0x20)) return A::sideEffect;
                if (!stopRemaining) stopRemaining = 8;
                control = 4; status = 0; interrupt = false;
                // A transfer still waiting on its DMA is given up.
                endData();
                return A::ok;
            }
            control = value & 0x24;
            return A::ok;
        }
        if (index == 1) {
            if (write) return A::sideEffect;
            if (!emptySlot) return A::externalInput;
            value = status | (running && (control & 4) ? 0x100 : 0); return A::ok;
        }
        if (index == 13 && slot.card) {
            // RES_FIFO: the response, a halfword a read, first bits first.
            if (write || slot.responseRead >= slot.responseCount) return A::sideEffect;
            value = slot.response[slot.responseRead++];
            return A::ok;
        }
        if (index == 8 || index >= 13) return A::externalInput;
        static constexpr uint32_t masks[]{0,0,0x3f,0x1fff,0xff,0xffff,0x3ff,0xffff,0,0x7f,0x3f,0xffff,0xffff};
        if (!write) { value = registers[index]; return A::ok; }
        if (value & ~masks[index]) return A::reservedBits;
        if (!ready) return A::sequence;
        if (running && index != 9) return A::sideEffect;
        if (index == 3 && ((value & 0xc00) || (value & 0x300) == 0x100 ||
                          (value & 0x300) == 0x300 || (value & 7) == 7)) return A::sideEffect;
        registers[index] = value;
        if (index == 9) interrupt = false;
        if (index == 3) staged |= 8;
        if (index >= 10) staged |= 1u << (index - 10);
        return A::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(psr0).Do32(psr1).DoBuffer32(registers, 13).Do32(control).Do32(recovery);
        a.Do32(status).Do32(remaining).Do32(stopRemaining).Do32(staged).Do32(phase);
        a.DoBool(running).DoBool(interrupt).DoBool(responseExpected);
        a.FixedBool(emptySlot).DoBool(resetAsserted).DoBool(ready);
        a.Require(registers[2] < 64, "mmcsd clock rate");
    }
    // The card section of a machine state, kept only while a card is in: the response, the transfer under
    // way and the card's own state. A loaded one waits for the next attachCard.
    static constexpr uint32_t cardStateVersion = 1;
    template <class A> void stateCard(A& a) {
        std::vector<uint8_t> card;
        if (!A::loading && slot.card) slot.saveCard(slot.card, card);
        a.DoBuffer16(slot.response, 8).Do8(slot.responseCount).Do8(slot.responseRead);
        a.DoBool(slot.data).DoBool(slot.dataWrite).DoBool(slot.dataPhase).DoBool(slot.dataWaiting);
        a.Do32(slot.dataBytes).Do32(slot.dataDone).Do32(slot.dataClocks).DoBuffer(slot.block, sizeof(slot.block));
        a.DoVector(card, 256);
        a.Require(emptySlot, "card without the virtual slot");
        a.Require(slot.responseRead <= slot.responseCount && slot.responseCount <= 8, "card response");
        a.Require(slot.dataDone <= slot.dataBytes && (!slot.dataPhase || !slot.dataWaiting) &&
                  (slot.data || (!slot.dataPhase && !slot.dataWaiting && !slot.dataBytes)), "card transfer");
        if (A::loading && a.ok()) { resume.card = std::move(card); resume.valid = true; }
    }
   private:
    // INT_MASK bits of the data phase's end: DATA_TRAN, and WRITE_OP_DONE for a write.
    uint32_t dataMask() const { return slot.dataWrite ? 3 : 1; }
    void endData() {
        slot.data = slot.dataWrite = slot.dataPhase = slot.dataWaiting = false;
        slot.dataBytes = slot.dataDone = slot.dataClocks = 0;
    }
    uint32_t failData(uint32_t moved) {
        status |= 0x800 | (slot.dataWrite ? 4 : 8);
        if (!(registers[9] & dataMask())) interrupt = true;
        endData();
        return moved;
    }
    // A command to the attached card. The card answers at once; the response is the guest's when the
    // command and response bits have passed on the card clock. A command the card doesn't answer
    // times out after RES_TO clocks, as in the empty slot.
    Imx1MmcsdAccess launchCardCommand() {
        using A = Imx1MmcsdAccess;
        const uint32_t command = registers[10], format = registers[3], response = format & 7;
        const bool data = format & 8;
        // Stream mode, ReadWait and the R4 to R6 formats stay outside the subset.
        if (response > 3 && response != 6) return A::sideEffect;
        if (format & 0x20) return A::sideEffect;
        if (response && !registers[4]) return A::sideEffect;
        if (data && (!registers[6] || registers[6] > 512 || !registers[7])) return A::sideEffect;
        uint8_t reply[17]{};
        const auto kind = slot.command(slot.card, uint8_t(command), (registers[11] << 16) | registers[12], reply);
        const bool longReply = kind == SdReply136bits;
        const bool answered = kind != SdReplyNone && response && longReply == (response == 2);
        unsigned bits = 48 + ((format & 0x80) ? 80 : 0);
        slot.responseCount = slot.responseRead = 0;
        if (response && !answered) bits += registers[4];
        else if (response) {
            const unsigned length = longReply ? 16 : 6;
            // An R2 starts with the start, transmission and reserved bits, 0x3f; vSD puts the command there.
            if (longReply) reply[0] = 0x3f;
            // Long responses drop the CRC byte (20.6.12); the short ones keep it in the last halfword.
            for (unsigned i = 0; i < length; i += 2)
                slot.response[slot.responseCount++] = uint16_t(reply[i] << 8 | (i + 1 < length ? reply[i + 1] : 0));
            if (!longReply) slot.response[2] &= 0xff00;
            bits += 2 + (longReply ? 136 : 48);
        }
        endData();
        if (data && answered) {
            const unsigned width = (format & 0x300) == 0x200 ? 4 : 1;
            const unsigned perBlock = registers[6] * 8 / width + 16 + 2 + ((format & 0x10) ? 16 : 0);
            slot.data = true;
            slot.dataWrite = format & 0x10;
            slot.dataBytes = registers[6] * registers[7];
            slot.dataClocks = 2 + perBlock * registers[7];
        }
        responseExpected = response && !answered;
        remaining = bits;
        staged = 0; status = 0; interrupt = false; phase = 0;
        running = true; control = 4;
        return A::ok;
    }
    void resetRegisters() {
        control = 0; recovery = 0; ready = false;
        status = remaining = stopRemaining = staged = phase = 0;
        running = interrupt = responseExpected = false;
        slot.responseCount = slot.responseRead = 0;
        endData();
        for (auto& value : registers) value = 0;
        registers[2] = 0x36; registers[4] = 0x40; registers[5] = 0xffff;
    }
    uint32_t psr0 = 0xffffc410, psr1 = 0xfffffbef;
    uint32_t registers[13]{0,0,0x36,0,0x40,0xffff,0,0,0,0,0,0,0};
    uint32_t control = 0, recovery = 0;
    uint32_t status = 0, remaining = 0, stopRemaining = 0, staged = 0, phase = 0;
    bool running = false, interrupt = false, responseExpected = false;
    bool emptySlot = false, resetAsserted = false, ready = false;
    // Not saved: the attached card and the command and transfer under way with it.
    struct Slot {
        VSD* card = nullptr;
        SdReplyType (*command)(VSD*, uint8_t, uint32_t, void*) = nullptr;
        SdDataReplyType (*fromCard)(VSD*, void*, uint32_t) = nullptr;
        SdDataReplyType (*toCard)(VSD*, const void*, uint32_t) = nullptr;
        void (*saveCard)(VSD*, std::vector<uint8_t>&) = nullptr;
        uint16_t response[8]{};
        uint8_t responseCount = 0, responseRead = 0;
        // data: the command has a data phase after its response; dataPhase: its clocks are counting;
        // dataWaiting: they have passed and the bytes wait for the DMA.
        bool data = false, dataWrite = false, dataPhase = false, dataWaiting = false;
        uint32_t dataBytes = 0, dataDone = 0, dataClocks = 0;
        uint8_t block[512]{};
    } slot;
#if defined(__GLIBCXX__) && defined(__x86_64__)
    static_assert(sizeof(Slot) == 592, "update stateCard and cardStateVersion");
#endif
    // A loaded card section's card state, for the next attachCard.
    struct Resume {
        bool valid = false;
        std::vector<uint8_t> card;
    } resume;

   public:
    static constexpr size_t unsavedBytes = sizeof(Slot) + sizeof(Resume);
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Mmcsd) - Imx1Mmcsd::unsavedBytes == 96, "update state() and stateVersion");
#endif

#endif
