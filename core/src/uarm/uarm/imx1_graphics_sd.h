#ifndef FROND_IMX1_GRAPHICS_SD_H
#define FROND_IMX1_GRAPHICS_SD_H

#include <algorithm>
#include <cstring>
#include <vector>
#include "imx1_bootstrap.h"
#include "vSD.h"

// The W4200's MMC/SD block, slot 2 (doc/step-zodiac-finish-plan.md, Z8.1). Time is in the shared 200 MHz
// guest domain. Until a card has been in the slot it is the empty slot: the native driver's pcs, CMD0 and
// the CMD55/CMD1 time-outs only, the answers of the machines saved before slot 2 had cards. With a card
// (Card::used) it takes every command, PIO block transfers and card detect, from the driver's whole code
// (204d2140-204d3b20). The card itself (vSD) is only called through runCardWork, on the committed model,
// since access and advance also run on copies that can be dropped.
class Imx1GraphicsSd {
   public:
    static constexpr bool owns(uint32_t address) {
        return address == 0x1601090c || address == 0x16010910 ||
               (address >= 0x16202000 && address < 0x16202200);
    }
    // The card interrupt handler's read of 16200204, whose value it drops.
    static bool ownsHandlerRead(uint32_t pc, uint32_t address) { return address == 0x16200204 && pc == 0x204d21aa; }
    // The card, its detect and write-protect inputs and whether the slot has had a card stay.
    void reset() {
        const Card kept = card;
        *this = {};
        card.card = kept.card; card.command = kept.command; card.fromCard = kept.fromCard;
        card.toCard = kept.toCard; card.save = kept.save; card.load = kept.load;
        card.used = kept.used; card.present = kept.present; card.protect = kept.protect;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(interface).Do32(clock).Do32(status1).Do32(status2);
        a.Do32(command).Do32(argumentLow).Do32(argumentHigh).Do32(interruptMask).Do32(option).Do32(port);
        a.Do64(clockRemaining).Do64(commandRemaining).Do64(resetElapsed);
        a.DoBool(commandPending).DoBool(resetHeld).DoBool(released);
    }
    void advance(uint64_t ticks) {
        if (resetHeld) resetElapsed = std::min<uint64_t>(32, resetElapsed + std::min<uint64_t>(ticks,32));
        if (clockRemaining) {
            const auto step = std::min(ticks, clockRemaining);
            clockRemaining -= step;
            ticks -= step;
        }
        uint64_t left = ticks;
        if (!clockRemaining && (clock & 0x100) && commandPending && !resetHeld) {
            const auto step = std::min(ticks, commandRemaining);
            commandRemaining -= step;
            left = ticks - step;
            if (!commandRemaining) {
                commandPending = false;
                status1 |= 1;
                if (card.used) completeCardCommand();
                else status2 = 0x80 | ((command & 0x700) == 0x300 ? 0 : 0x40);
            }
        }
        if (card.used && !clockRemaining && (clock & 0x100) && !commandPending && !resetHeld && card.dataRemaining) {
            card.dataRemaining -= std::min(left, card.dataRemaining);
            if (!card.dataRemaining) dataDue();
        }
    }
    // Ticks until the clock settles, the pending command ends or the next block or stop is due.
    uint64_t idleQuietTicks() const {
        if (clockRemaining) return clockRemaining;
        if (!(clock & 0x100) || resetHeld) return UINT64_MAX;
        if (commandPending) return commandRemaining;
        return card.used && card.dataRemaining ? card.dataRemaining : UINT64_MAX;
    }
    uint32_t controlValue() const { return control; }
    uint32_t clockValue() const { return clockRemaining ? clock & ~0x100u : clock; }
    uint32_t statusValue() const { return status2; }
    bool pending() const { return commandPending; }
    bool cardEventsKnown() const { return released && (interruptMask == 0x31d || interruptMask == 0x305); }
    VSD* attachedCard() const { return card.card; }
    bool cardUsed() const { return card.used; }
    bool cardPresent() const { return card.present; }
    bool cardWorkPending() const { return card.work != workNone; }
    // Write-protect input (16010910 bit 0), 1 = protected.
    void setWriteProtect(bool on) { card.protect = on; }
    // Puts a card in the slot, or takes it out with null. Returns true when the change latches the
    // insert or remove bit in status1, which raises the chip's SD interrupt (16010204 bit 25).
    // ASSUMPTION "masked card events": a change with its status1 bit masked in 40 latches nothing.
    bool attachCard(VSD* in) {
        if (in && !card.used) {
            // A command the empty slot started ends as the empty slot would have ended it.
            card.result = 0x80 | ((command & 0x700) == 0x300 ? 0 : 0x40);
        }
        if (in) { attachPointers(in); card.used = true; }
        card.card = in;
        const bool was = card.present;
        card.present = in != nullptr;
        if (was == card.present) return false;
        // A transfer cut short by the card going ends with an error.
        if (!card.present && card.phase != phaseNone) dataError();
        if (!cardEventsKnown() || resetHeld) return false;
        const uint32_t bit = card.present ? 0x10 : 0x08;
        if (interruptMask & bit) return false;
        status1 |= bit;
        return true;
    }
    // After a state load: the card in the slot now, with the loaded card section's card bytes (resume) when
    // the state had one. A card that takes them on carries on. Otherwise the transfer and response under
    // way are dropped with an error the driver sees (status2 bit 15 and status1 bit 2, as the card going
    // does), and a change of the detect input latches its event like attachCard. Returns true when it
    // carried on; event, when given, is set when the change latches an event (raise the chip's SD interrupt).
    bool reattachCard(VSD* in, const std::vector<uint8_t>* resume, bool* event = nullptr) {
        if (event) *event = false;
        if (in) attachPointers(in);
        card.card = in;
        if (in && resume && card.present && card.load(in, resume->data(), resume->size())) return true;
        const bool was = card.present;
        card.present = in != nullptr;
        if (in) card.used = true;
        if (card.phase != phaseNone || card.work != workNone) {
            dataError();
            status1 |= 4;
            card.work = workNone;
        }
        if (was != card.present && cardEventsKnown() && !resetHeld) {
            const uint32_t bit = card.present ? 0x10 : 0x08;
            if (!(interruptMask & bit)) { status1 |= bit; if (event) *event = true; }
        }
        return false;
    }
    // The card's bytes for a state, empty with no card in.
    void saveCard(std::vector<uint8_t>& out) const {
        out.clear();
        if (card.card) card.save(card.card, out);
    }
    // The card work access or advance left for the committed model: the command to the card, a block from it,
    // a block to it or the stop (CMD12) at the end of a multi-block transfer.
    void runCardWork() {
        const uint8_t work = card.work;
        card.work = workNone;
        switch (work) {
            case workCommand: sendCardCommand(); break;
            case workRead:
                if (!card.card || card.fromCard(card.card, card.block, 512) != SdDataOk) { dataError(); break; }
                status2 |= 0x100; card.blockReady = true; card.word = 0;
                break;
            case workWrite:
                if (!card.card || card.toCard(card.card, card.block, 512) != SdDataOk) { dataError(); break; }
                if (--card.blocksLeft) card.dataRemaining = blockTicks();
                else if (multiBlock()) { card.stopping = true; card.dataRemaining = blockTicks() + stopTicks(); }
                else card.phase = phaseNone;
                break;
            case workStop:
                stopCard();
                status1 |= 4;
                card.phase = phaseNone; card.stopping = false;
                break;
            default: break;
        }
    }
    // The section of a machine state saved once the slot has had a card: the card inputs, the block, count and
    // response registers and the transfer under way. The card's own bytes go with it, saved by the caller.
    static constexpr uint32_t cardStateVersion = 1;
    template <class A> void stateCard(A& a) {
        a.DoBool(card.used).DoBool(card.present).DoBool(card.protect).DoBool(card.blockReady).DoBool(card.stopping);
        a.DoBool(card.dataCommand).Do8(card.phase).Do8(card.work).Do16(card.word);
        a.Do32(card.blockLength).Do32(card.multi).Do32(card.count).Do32(card.blocksLeft).Do32(card.result);
        a.Do64(card.dataRemaining).DoBuffer16(card.response, 8).DoBuffer(card.block, sizeof(card.block));
        a.Require(card.used && card.phase <= phaseWrite && card.work <= workStop && card.word <= 128 &&
                  card.count <= 0xffff && card.blocksLeft <= 0xffff, "graphics sd card");
    }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (ownsHandlerRead(pc, address)) {
            // ASSUMPTION "16200204 reads 0": the handler drops the value.
            if (!card.used) return A::externalInput;
            if (write || size != 2) return A::width;
            value = 0;
            return A::ok;
        }
        if (!owns(address)) return A::unmapped;
        const bool config = address < 0x16200000;
        if (size != 4 && !(size == 2 && !write && !config)) return A::width;
        if (config) {
            if (address == 0x16010910) {
                if (!write) {
                    if (pc != 0x204d26a2 && pc != 0x204d26b4 && pc != 0x204d3618 && pc != 0x204d3728) return A::externalInput;
                    // Bit 2 card present and bit 0 write protect, 1 = yes, both 0 for the empty slot.
                    value = control | (card.present ? 4 : 0) | (card.present && card.protect ? 1 : 0);
                } else {
                    if (pc != 0x204d26b2 && pc != 0x204d26ba && pc != 0x204d273e) return A::externalInput;
                    // The driver writes back what it read, inputs included.
                    if (card.used) value &= ~5u;
                    if (value & ~0x438000u) return A::sideEffect;
                    control = value;
                    if (value & 0x8000) {
                        commandPending = false; commandRemaining = 0; clock = 0; clockRemaining = 0;
                        if (card.used) endCardTransfer();
                    }
                }
            } else {
                if (pc != (write ? 0x204d26c2u : 0x204d26bcu)) return A::externalInput;
                if (write) {
                    if (value & ~0x08000000u) return A::sideEffect;
                    interface = value;
                } else value = interface;
            }
            return A::ok;
        }
        const uint32_t offset = address - 0x16202000;
        if (offset & 3) return A::unmapped;
        if (offset == 0x1c0) {
            if (!write || (pc != 0x204d26cc && pc != 0x204d26d8)) return A::externalInput;
            if (value != 0 && value != 7) return A::sideEffect;
            if (value == 7 && (!resetHeld || resetElapsed < 32)) return A::sequence;
            if (!value) {
                resetHeld = true; resetElapsed = 0; released = false;
                status1 = 0; status2 = 0x80; clock = 0; clockRemaining = 0;
                commandPending = false; commandRemaining = 0; argumentLow = argumentHigh = command = 0;
                interruptMask = option = port = 0;
                if (card.used) {
                    endCardTransfer();
                    card.blockLength = card.multi = card.count = 0;
                    std::memset(card.response, 0, sizeof(card.response));
                }
            } else { resetHeld = false; released = true; }
            return A::ok;
        }
        // With a card the block stays reachable while the slot is off (16010910 = 0x8000), as the card
        // interrupt handler can run then.
        if (card.used && released && !resetHeld && interface == 0x08000000) return cardAccess(pc, offset, size, write, value);
        if (!released || resetHeld || (control & 0x438000) != 0x430000 || interface != 0x08000000) return A::sequence;
        const bool native = (pc >= 0x204d2174 && pc <= 0x204d2190) ||
                            (pc >= 0x204d226e && pc <= 0x204d22b2) ||
                            (pc >= 0x204d22b4 && pc <= 0x204d2326) ||
                            (pc >= 0x204d2456 && pc <= 0x204d269c) ||
                            (pc >= 0x204d2748 && pc <= 0x204d294a) ||
                            (pc >= 0x204d363e && pc <= 0x204d3656) || pc == 0x204d2736;
        if (!native) return A::externalInput;
        if (!write) {
            switch (offset) {
                case 0x38: value = status1; break;
                case 0x3c: value = status2; break;
                case 0x40: value = interruptMask; break;
                case 0x48: value = clockValue(); break;
                case 0x50: value = option; break;
                default: return A::externalInput;
            }
            return A::ok;
        }
        if (value >> 16) return A::sideEffect;
        switch (offset) {
            case 0x00:
                // No card supplies data or responses. Other command formats stay guarded.
                if (value != 0x300 && value != 0x437 && value != 0x701) return A::externalInput;
                if (commandPending) return A::sequence;
                command = value; commandPending = true;
                commandRemaining = uint64_t((value == 0x300 ? 48 : 64)) * 2 * ((clock & 0xff) + 1);
                status1 &= ~1u; status2 = 0x4000;
                break;
            case 0x04: if (value != 0x200) return A::sideEffect; port = value; break;
            case 0x08: if (commandPending) return A::sequence; argumentLow = value; break;
            case 0x0c: if (commandPending) return A::sequence; argumentHigh = value; break;
            case 0x38: status1 &= value; break;
            case 0x3c: if (value & ~0x7fu) return A::sideEffect; status2 = (status2 & ~0x7fu) | (status2 & value & 0x7f); break;
            case 0x40:
                if (pc == 0x204d22a0) {
                    if (value != ((interruptMask & ~0x18u) | 0x305u)) return A::sideEffect;
                } else if (pc != 0x204d22f0 || value != (interruptMask | 0x31du)) return A::sideEffect;
                if (value != 0x31d && value != 0x305) return A::externalInput;
                interruptMask = value;
                break;
            case 0x48:
                if (value & ~0x1ffu) return A::sideEffect;
                if ((value & 0xff) != 0 && (value & 0xff) != 1 && (value & 0xff) != 0x40) return A::sideEffect;
                if ((value & 0x100) && (!(clock & 0x100) || (value & 0xff) != (clock & 0xff))) clockRemaining = 20000;
                if (!(value & 0x100)) clockRemaining = 0;
                clock = value;
                break;
            case 0x50: if (value & ~0x8000u) return A::sideEffect; option = value; break;
            case 0x6c: case 0x1f0: if (value) return A::sideEffect; break;
            case 0x1e8: if (value != 0x700) return A::sideEffect; break;
            default: return A::externalInput;
        }
        return A::ok;
    }
   private:
    static constexpr uint8_t phaseNone = 0, phaseRead = 1, phaseWrite = 2;
    static constexpr uint8_t workNone = 0, workCommand = 1, workRead = 2, workWrite = 3, workStop = 4;
    // Through pointers, so only the code that attaches a card links vSD.
    void attachPointers(VSD*) {
        card.command = vsdCommand;
        card.fromCard = vsdDataXferBlockFromCard;
        card.toCard = vsdDataXferBlockToCard;
        card.save = vsdMachineSave;
        card.load = vsdMachineLoad;
    }
    // Card clock ticks: 2 guest ticks per divider step per bit, as the empty slot's command time.
    uint64_t bitTicks(uint64_t bits) const { return bits * 2 * ((clock & 0xff) + 1); }
    // A 512-byte block with its start, CRC and end bits and two clocks of access time. 16202050 bit 15
    // set is taken as a 1-bit bus, clear as 4 bits (ASSUMPTION in the Z8.1 notes).
    uint64_t blockTicks() const { return bitTicks(512 * 8 / ((option & 0x8000) ? 1 : 4) + 18 + 2); }
    // CMD12 and its R1b response.
    uint64_t stopTicks() const { return bitTicks(48 + 2 + 48); }
    bool multiBlock() const { return (command & 0x7800) == 0x3800 || (command & 0x7800) == 0x2800; }
    bool readCommand() const { return command & 0x1000; }
    void endCardTransfer() {
        card.phase = phaseNone; card.work = workNone; card.blockReady = card.stopping = card.dataCommand = false;
        card.dataRemaining = 0; card.word = 0; card.blocksLeft = 0;
    }
    // A failed block, or one cut short: status2 bit 15 (ASSUMPTION: the ROM only says it is an error, 0x7001),
    // and status1 bit 2 for a multi-block transfer, whose end the driver waits for with no limit.
    void dataError() {
        const bool multi = multiBlock() && card.phase != phaseNone;
        if (multi && card.work != workStop) stopCard();
        endCardTransfer();
        status2 |= 0x8000;
        if (multi) status1 |= 4;
    }
    // CMD12 to the card, sent by the block itself at the end of a multi-block transfer (ASSUMPTION in the notes).
    void stopCard() {
        uint8_t reply[17]{};
        if (card.card) card.command(card.card, 12, 0, reply);
    }
    // The command reaches the card at once; its result shows once its bits and the response's have passed.
    void sendCardCommand() {
        const uint32_t type = (command >> 8) & 7;
        uint8_t reply[17]{};
        const uint8_t index = command & 0x3f;
        const auto kind = card.card ? card.command(card.card, index,
                                                   (argumentHigh << 16) | argumentLow, reply)
                                    : SdReplyNone;
        const bool longReply = kind == SdReply136bits;
        const bool answered = type != 3 && kind != SdReplyNone && longReply == (type == 6);
        card.dataCommand = answered && (command & 0x800) && !vsdWriteRejected(index, reply);
        uint64_t bits = 48;
        if (type == 3) card.result = 0x80;
        else if (!answered) { card.result = 0xc0; bits = 64; }
        else {
            card.result = 0x80;
            std::memset(card.response, 0, sizeof(card.response));
            if (longReply) {
                // R2: register i holds bits 16i+23 to 16i+8 of the 136-bit token, the header 0x3f on top.
                reply[0] = 0x3f;
                for (unsigned i = 0; i < 8; ++i) card.response[i] = uint16_t(reply[14 - 2 * i] << 8 | reply[15 - 2 * i]);
            } else {
                // R1, R1b, R3, R6: the 32-bit field in 18 (low) and 1c (high), the command byte in 20.
                card.response[0] = uint16_t(reply[3] << 8 | reply[4]);
                card.response[1] = uint16_t(reply[1] << 8 | reply[2]);
                card.response[2] = reply[0];
            }
            bits = 48 + 2 + (longReply ? 136 : 48);
        }
        commandRemaining = bitTicks(bits);
    }
    void completeCardCommand() {
        status2 = card.result;
        if (!card.dataCommand) return;
        card.dataCommand = false;
        card.blocksLeft = multiBlock() ? card.count : 1;
        card.word = 0;
        if (readCommand()) {
            card.phase = phaseRead;
            card.dataRemaining = blockTicks();
        } else {
            // The FIFO is empty and takes the first block at once.
            card.phase = phaseWrite;
            card.blockReady = true;
            status2 |= 0x200;
        }
    }
    void dataDue() {
        if (card.stopping || card.phase == phaseRead) card.work = card.stopping ? workStop : workRead;
        else if (card.phase == phaseWrite) { card.blockReady = true; card.word = 0; status2 |= 0x200; }
    }
    // The slot once it has had a card: any pc of the driver, every register it uses.
    Imx1BootstrapAccess cardAccess(uint32_t pc, uint32_t offset, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (pc < 0x204d2140 || pc >= 0x204d3b20) return A::externalInput;
        if (offset == 0x60) {
            // The data port, 32 bits, byte 0 in bits 7-0.
            if (size != 4) return A::width;
            if (!card.blockReady || card.word >= 128 || card.phase != (write ? phaseWrite : phaseRead)) return A::sequence;
            uint8_t* bytes = card.block + card.word * 4;
            if (write) { for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i)); }
            else value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
            if (++card.word < 128) return A::ok;
            card.blockReady = false;
            if (write) { card.work = workWrite; return A::ok; }
            if (--card.blocksLeft) card.dataRemaining = blockTicks();
            else if (multiBlock()) { card.stopping = true; card.dataRemaining = stopTicks(); }
            else card.phase = phaseNone;
            return A::ok;
        }
        if (!write) {
            switch (offset) {
                case 0x00: value = command; break;
                case 0x04: value = port; break;
                case 0x08: value = argumentLow; break;
                case 0x0c: value = argumentHigh; break;
                case 0x10: value = card.multi; break;
                case 0x14: value = card.count; break;
                case 0x18: case 0x1c: case 0x20: case 0x24: case 0x28: case 0x2c: case 0x30: case 0x34:
                    value = card.response[(offset - 0x18) / 4]; break;
                case 0x38: value = status1; break;
                case 0x3c: value = status2; break;
                case 0x40: value = interruptMask; break;
                case 0x48: value = clockValue(); break;
                case 0x4c: value = card.blockLength; break;
                case 0x50: value = option; break;
                default: return A::externalInput;
            }
            return A::ok;
        }
        if (value >> 16) return A::sideEffect;
        switch (offset) {
            case 0x00: {
                // Bits 0-5 index, 6 ACMD (ignored), 8-10 response type 3-7, 11-14 data: read or write, single or multi.
                const uint32_t type = (value >> 8) & 7, data = value & 0xf800;
                if (type < 3 || (data && data != 0x5800 && data != 0x3800 && data != 0x4800 && data != 0x2800))
                    return A::externalInput;
                if (commandPending || card.phase != phaseNone || card.work != workNone) return A::sequence;
                if (data && (card.blockLength != 0x200 ||
                             (data == 0x3800 || data == 0x2800 ? card.multi != 0x100 || card.count < 2 : card.multi != 0)))
                    return A::sideEffect;
                command = value; commandPending = true;
                // The time-out's time until runCardWork sets the real one.
                commandRemaining = bitTicks(64);
                status1 &= ~1u; status2 = 0x4000;
                card.work = workCommand;
                break;
            }
            case 0x04: if (value != 0x200) return A::sideEffect; port = value; break;
            case 0x08: if (commandPending) return A::sequence; argumentLow = value; break;
            case 0x0c: if (commandPending) return A::sequence; argumentHigh = value; break;
            case 0x10: if (commandPending) return A::sequence; if (value != 0 && value != 0x100) return A::sideEffect; card.multi = value; break;
            case 0x14: if (commandPending) return A::sequence; card.count = value; break;
            case 0x38: status1 &= value; break;
            case 0x3c:
                // Writing 0 clears errors, block ready and bit 15; bits 7 and 14 stay as in the empty slot.
                if (value & ~0x7fu) return A::sideEffect;
                status2 = (status2 & 0x4080) | (status2 & value & 0x7f);
                break;
            case 0x40:
                if (pc == 0x204d22a0) {
                    if (value != ((interruptMask & ~0x18u) | 0x305u)) return A::sideEffect;
                } else if (pc != 0x204d22f0 || value != (interruptMask | 0x31du)) return A::sideEffect;
                if (value != 0x31d && value != 0x305) return A::externalInput;
                interruptMask = value;
                break;
            case 0x48:
                if (value & ~0x1ffu) return A::sideEffect;
                if ((value & 0xff) != 0 && (value & 0xff) != 1 && (value & 0xff) != 0x40) return A::sideEffect;
                if ((value & 0x100) && (!(clock & 0x100) || (value & 0xff) != (clock & 0xff))) clockRemaining = 20000;
                if (!(value & 0x100)) clockRemaining = 0;
                clock = value;
                break;
            case 0x4c: if (commandPending) return A::sequence; if (value != 0x200) return A::sideEffect; card.blockLength = value; break;
            case 0x50: if (value & ~0x8000u) return A::sideEffect; option = value; break;
            case 0x6c: case 0x1f0: if (value) return A::sideEffect; break;
            case 0x1e8: if (value != 0x700) return A::sideEffect; break;
            default: return A::externalInput;
        }
        return A::ok;
    }
    uint32_t control = 0, interface = 0, clock = 0, status1 = 0, status2 = 0x80;
    uint32_t command = 0, argumentLow = 0, argumentHigh = 0, interruptMask = 0, option = 0, port = 0;
    uint64_t clockRemaining = 0, commandRemaining = 0, resetElapsed = 0;
    bool commandPending = false, resetHeld = false, released = false;
    // Not in state(): the card and everything a card brings, saved in stateCard once the slot has had one.
    struct Card {
        VSD* card = nullptr;
        SdReplyType (*command)(VSD*, uint8_t, uint32_t, void*) = nullptr;
        SdDataReplyType (*fromCard)(VSD*, void*, uint32_t) = nullptr;
        SdDataReplyType (*toCard)(VSD*, const void*, uint32_t) = nullptr;
        void (*save)(VSD*, std::vector<uint8_t>&) = nullptr;
        bool (*load)(VSD*, const uint8_t*, size_t) = nullptr;
        // Ticks until the next block is ready (read), taken (write) or the stop ends.
        uint64_t dataRemaining = 0;
        uint32_t blockLength = 0, multi = 0, count = 0, blocksLeft = 0;
        // status2 at the end of the command under way.
        uint32_t result = 0x80;
        uint16_t response[8]{};
        uint16_t word = 0;
        uint8_t phase = phaseNone, work = workNone;
        // used: a card has been in since the machine started. dataCommand: the command under way has a data phase.
        bool used = false, present = false, protect = false, blockReady = false, stopping = false, dataCommand = false;
        uint8_t block[512]{};
    } card;

   public:
    static constexpr size_t unsavedBytes = sizeof(Card);
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsSd) - Imx1GraphicsSd::unsavedBytes == 80, "update state() and stateVersion");
#endif
#endif
