#ifndef FROND_IMX1_UART_H
#define FROND_IMX1_UART_H

#include <cstdint>

enum class Imx1UartAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };

// One character that left the transmitter, with IREN as it was when the frame ended.
using Imx1UartTransmit = void (*)(void* context, uint8_t character, bool infrared);

// MC9328MX1RM chapter 27. The caller supplies elapsed PERCLK1 edges. A peer attaches with
// setPeer() and receive(); with none the receiver sees an idle line and sent characters are dropped.
// Automatic baud detection, escape detection, DMA requests, BREAK sending and loopback are refused.
class Imx1Uart {
   public:
    // UART2 and UART3 have the DTR input and the DSR, DCD and RI outputs.
    explicit Imx1Uart(bool modemPins = false) : modem(modemPins) {}
    enum Output : uint32_t { rxOutput = 1, txOutput = 2, controlOutput = 4, errorOutput = 8, rtsOutput = 16, dtrOutput = 32 };

    void setPeer(Imx1UartTransmit callback, void* context) { transmit = callback; transmitContext = context; }
    // A complete frame from the peer. Returns false when the receiver is off and the frame is lost.
    bool receive(uint8_t character, bool frameError = false, bool parityError = false, bool breakFrame = false) {
        if (!running() || !(ucr2 & 2) || resetCycles) return false;
        usr2 |= 0x80;
        if (ucr1 & 0x80) usr2 |= 0x100;
        idleBits = 0; idleArmed = true;
        if (rxCount == 32) { usr2 |= 2; return true; }
        uint16_t entry = (ucr2 & 0x20) ? character : character & 0x7f;
        if (breakFrame) { entry = 0x0800; frameError = true; usr2 |= 4; if ((ucr2 & 0x180) == 0x180) parityError = true; }
        if (frameError) { entry |= 0x1000; usr1 |= 0x400; }
        if (parityError && (ucr2 & 0x100)) { entry |= 0x0400; usr1 |= 0x8000; }
        if (rxCount == 31) entry |= 0x2000;
        if (entry & 0x3c00) entry |= 0x4000;
        rxFifo[(rxHead + rxCount++) & 31] = entry;
        return true;
    }
    // Pin levels: low is active. Until RTS is set USR1 can't be read, nor a character sent without IRTS.
    void setRts(bool low) {
        if (rtsKnown && low != rtsLow) {
            usr1 |= 0x1000;
            const unsigned edge = (ucr2 >> 9) & 3;
            if ((ucr2 & 0x10) && (edge >= 2 || (edge == 1) == low)) usr2 |= 0x10;
        }
        rtsKnown = true; rtsLow = low;
    }
    void setDtr(bool low) {
        if (modem && dtrKnown && low != dtrLow) {
            const unsigned edge = (ucr3 >> 14) & 3;
            if ((ucr3 & 0x2000) && (edge >= 2 || (edge == 1) == low)) usr2 |= 0x2000;
        }
        dtrKnown = true; dtrLow = low;
    }
    bool ctsLow() const { return (ucr2 & 0x2000) ? rxCount < ((ucr4 >> 10) & 63) : (ucr2 & 0x1000) != 0; }
    bool infrared() const { return ucr1 & 0x80; }
    uint64_t transmittedCharacters() const { return transmitted; }

    uint32_t interruptOutputs() const {
        const uint32_t s1 = status1(), s2 = status2();
        uint32_t out = 0;
        if (((ucr1 & 0x200) && (s1 & 0x200)) || ((ucr1 & 0x1000) && (s2 & 0x1000)) ||
            ((ucr4 & 1) && (s2 & 1)) || ((ucr3 & 0x40) && (s1 & 0x40))) out |= rxOutput;
        if (((ucr1 & 0x40) && txEmptyInterrupt) || ((ucr1 & 0x2000) && (s1 & 0x2000)) ||
            ((ucr4 & 8) && (s2 & 8))) out |= txOutput;
        if (((ucr4 & 2) && (s2 & 2)) || ((ucr4 & 4) && (s2 & 4)) || ((ucr4 & 0x80) && (s2 & 0x80)) ||
            ((ucr1 & 0x8000) && (s2 & 0x8000)) || ((ucr2 & 0x8000) && (s1 & 0x800)) ||
            ((ucr4 & 0x100) && (s2 & 0x100)) || ((ucr3 & 0x20) && (s1 & 0x20)) ||
            ((ucr3 & 0x10) && (s1 & 0x10))) out |= controlOutput;
        if (((ucr3 & 0x800) && (s1 & 0x400)) || ((ucr3 & 0x1000) && (s1 & 0x8000))) out |= errorOutput;
        if (((ucr1 & 0x20) && (s1 & 0x1000)) || ((ucr2 & 0x10) && (s2 & 0x10))) out |= rtsOutput;
        if (modem && (ucr3 & 0x2000) && (s2 & 0x2000)) out |= dtrOutput;
        return out;
    }
    // True while elapsed time alone can change a flag.
    bool busy() const { return idleQuietCycles() != 0; }
    // PERCLK1 cycles until the next flag change, 0 when nothing is under way.
    uint64_t idleQuietCycles() const {
        if (!(ucr1 & 4)) return 0;
        if (resetCycles) return resetCycles;
        if (!running()) return 0;
        uint64_t bits = 0;
        if (shifting) bits = shiftBits;
        else if (txCount && transmitterOn()) bits = 1;
        if (idlePending()) {
            const uint64_t limit = idleLimit();
            const uint64_t left = (idleBits < limit ? limit - idleBits : 0) + 1;
            bits = bits ? (left < bits ? left : bits) : left;
        }
        if (!bits) return 0;
        const __uint128_t need = __uint128_t(bits) * bitPeriod();
        if (need <= bitPhase) return 1;
        const __uint128_t cycles = (need - bitPhase + ubirActive) / (uint64_t(ubirActive) + 1);
        return cycles > UINT64_MAX ? UINT64_MAX : uint64_t(cycles);
    }
    void advance(uint64_t perclk1Cycles) {
        if (!(ucr1 & 4) || !perclk1Cycles) return;
        if (resetCycles) {
            if (perclk1Cycles < resetCycles) { resetCycles -= uint32_t(perclk1Cycles); return; }
            perclk1Cycles -= resetCycles; resetCycles = 0;
        }
        if (!running()) return;
        const __uint128_t total = __uint128_t(perclk1Cycles) * (uint64_t(ubirActive) + 1) + bitPhase;
        const uint64_t period = bitPeriod();
        __uint128_t bits = total / period;
        bitPhase = uint64_t(total % period);
        if (idlePending()) {
            const uint64_t limit = idleLimit();
            // A count already at or past the limit ends on the next bit time.
            if (bits > (idleBits < limit ? limit - idleBits : 0)) { usr2 |= 0x1000; idleArmed = false; idleBits = 0; }
            else idleBits += uint32_t(bits);
        }
        while (bits) {
            if (!shifting) {
                if (!txCount || !transmitterOn()) break;
                // The character moves to the shift register on a transmit clock edge.
                shiftCharacter = txFifo[txHead]; txHead = (txHead + 1) & 31; --txCount;
                if (!txCount && txSeveral) txEmptyInterrupt = true;
                shifting = true;
                shiftBits = 1 + ((ucr2 & 0x20) ? 8 : 7) + ((ucr2 & 0x100) ? 1 : 0) + ((ucr2 & 0x40) ? 2 : 1);
                --bits;
                continue;
            }
            const uint32_t step = bits < shiftBits ? uint32_t(bits) : shiftBits;
            shiftBits -= step; bits -= step;
            if (shiftBits) break;
            shifting = false;
            ++transmitted;
            if (transmit) transmit(transmitContext, (ucr2 & 0x20) ? shiftCharacter : shiftCharacter & 0x7f, infrared());
            if (!txCount) { txEmptyInterrupt = true; txSeveral = false; break; }
            if (!transmitterOn()) break;
            // The next character follows on the same edge.
            shiftCharacter = txFifo[txHead]; txHead = (txHead + 1) & 31; --txCount;
            if (!txCount && txSeveral) txEmptyInterrupt = true;
            shifting = true;
            shiftBits = 1 + ((ucr2 & 0x20) ? 8 : 7) + ((ucr2 & 0x100) ? 1 : 0) + ((ucr2 & 0x40) ? 2 : 1);
        }
    }

    static const char* registerName(uint32_t address) {
        static constexpr const char* names[2][21] = {
            {"UART1.UCR1", "UART1.UCR2", "UART1.UCR3", "UART1.UCR4", "UART1.UFCR", "UART1.USR1", "UART1.USR2",
             "UART1.UESC", "UART1.UTIM", "UART1.UBIR", "UART1.UBMR", "UART1.UBRC", "UART1.BIPR1", "UART1.BIPR2",
             "UART1.BIPR3", "UART1.BIPR4", "UART1.BMPR1", "UART1.BMPR2", "UART1.BMPR3", "UART1.BMPR4", "UART1.UTS"},
            {"UART2.UCR1", "UART2.UCR2", "UART2.UCR3", "UART2.UCR4", "UART2.UFCR", "UART2.USR1", "UART2.USR2",
             "UART2.UESC", "UART2.UTIM", "UART2.UBIR", "UART2.UBMR", "UART2.UBRC", "UART2.BIPR1", "UART2.BIPR2",
             "UART2.BIPR3", "UART2.BIPR4", "UART2.BMPR1", "UART2.BMPR2", "UART2.BMPR3", "UART2.BMPR4", "UART2.UTS"}};
        const uint32_t base = address & ~0xfffu;
        if (base != 0x00206000 && base != 0x00207000) return nullptr;
        const uint32_t offset = address - base;
        const bool second = base == 0x00207000;
        if (offset < 0x40) return second ? "UART2.URXD" : "UART1.URXD";
        if (offset < 0x80) return second ? "UART2.UTXD" : "UART1.UTXD";
        if (offset >= 0xd4) return nullptr;
        return names[second][(offset - 0x80) >> 2];
    }

    Imx1UartAccess access(uint32_t offset, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1UartAccess;
        if (offset >= 0xd4) return A::unmapped;
        if (size != 4 || (offset & 3)) return A::width;
        if (write && (value & 0xffff0000u)) return A::reservedBits;
        if (offset < 0x40) {
            if (write) return A::sideEffect;
            if (rxCount) {
                lastReceived = rxFifo[rxHead]; rxHead = (rxHead + 1) & 31; --rxCount;
                receivedRead = true;
                value = 0x8000u | lastReceived;
                return A::ok;
            }
            // The data bits are undefined until a first character was read.
            if (!receivedRead) return A::sideEffect;
            value = lastReceived & 0x7fffu;
            return A::ok;
        }
        if (offset < 0x80) {
            if (!write) { value = 0; return A::ok; }
            if (value & ~0xffu) return A::reservedBits;
            // The manual doesn't say what a write to a full FIFO or one under reset leaves behind.
            if (txCount == 32 || resetCycles || !(ucr1 & 4)) return A::sideEffect;
            if (rtsNeeded(ucr2)) return A::externalInput;
            txFifo[(txHead + txCount++) & 31] = uint8_t(value);
            if (txCount >= 2) txSeveral = true;
            txEmptyInterrupt = false;
            return A::ok;
        }
        const unsigned index = (offset - 0x80) >> 2;
        if (!write) {
            switch (index) {
                case 0: value = ucr1; break;
                case 1: value = ucr2 | (resetCycles ? 0 : 1); break;
                case 2: value = ucr3; break;
                case 3: value = ucr4; break;
                case 4: value = ufcr; break;
                case 5:
                    if (!rtsKnown) return A::externalInput;
                    value = status1();
                    break;
                case 6: value = status2(); break;
                case 7: value = uesc; break;
                case 8: value = utim; break;
                case 9: value = ubir; break;
                case 10: value = ubmr; break;
                case 11: value = 8; break;
                case 20:
                    value = (txCount ? 0 : 0x40) | (rxCount ? 0 : 0x20) | (txCount == 32 ? 0x10 : 0) |
                            (rxCount == 32 ? 8 : 0) | (resetCycles ? 1 : 0);
                    break;
                default:
                    // The preset registers have no reset value.
                    if (!(presetWritten & (1u << (index - 12)))) return A::sideEffect;
                    value = presets[index - 12];
                    break;
            }
            return A::ok;
        }
        switch (index) {
            case 0: {
                if (value & 0x4118) return A::sideEffect;
                if ((value & 3) == 3) return A::sideEffect;
                if (idleCountPast(value, ucr2)) return A::sideEffect;
                const bool was = running();
                ucr1 = value;
                if (was && !running()) stopTransmitter();
                if (!was && running()) bitPhase = 0;
                break;
            }
            case 1: {
                if (value & 8) return A::reservedBits;
                if (value & 0x0800) return A::sideEffect;
                if (txCount && (value & 1) && rtsNeeded(value)) return A::externalInput;
                if (!(value & 1) && !(ucr1 & 4)) return A::sideEffect;
                if ((value & 1) && idleCountPast(ucr1, value)) return A::sideEffect;
                ucr2 = value & 0xfff6;
                if (!(value & 4)) stopTransmitter();
                if (!(value & 1)) softwareReset();
                break;
            }
            case 2:
                if (value & (modem ? 0x0080u : 0xe780u)) return A::reservedBits;
                ucr3 = value;
                break;
            case 3:
                if ((value & 0x10) || (value >> 10) > 32) return A::reservedBits;
                ucr4 = value;
                break;
            case 4: {
                const unsigned level = value >> 10;
                if ((value & 0x40) || level < 2 || level > 32 || (value & 63) > 32 || ((value >> 7) & 7) == 7)
                    return A::reservedBits;
                if (((value ^ ufcr) & 0x380) && active()) return A::sideEffect;
                if ((value ^ ufcr) & 0x380) bitPhase = 0;
                ufcr = value;
                break;
            }
            case 5: usr1 &= ~(value & 0x9c30u); break;
            case 6: usr2 &= ~(value & 0xb196u); break;
            case 7:
                if (value & ~0xffu) return A::reservedBits;
                uesc = value;
                break;
            case 8:
                if (value & ~0xfffu) return A::reservedBits;
                utim = value;
                break;
            case 9:
                // UBIR goes first; the pair takes effect with UBMR.
                if (modulatorAlone) return A::sideEffect;
                ubir = value; incrementWritten = true;
                break;
            case 10:
                if (incrementWritten && (ubir != ubirActive || value != ubmrActive) && active()) return A::sideEffect;
                ubmr = value;
                if (!incrementWritten) { modulatorAlone = true; break; }
                incrementWritten = false;
                if (ubir != ubirActive || ubmr != ubmrActive) bitPhase = 0;
                ubirActive = ubir; ubmrActive = ubmr;
                break;
            case 11: return A::sideEffect;
            case 20:
                // FRCPERR and LOOP aren't modelled. Bit 10 is writable in the manual's table with no description.
                if (value & 0x3400) return A::sideEffect;
                break;
            default:
                presets[index - 12] = uint16_t(value);
                presetWritten |= 1u << (index - 12);
                break;
        }
        return A::ok;
    }

    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.FixedBool(modem).Do32(ucr1).Do32(ucr2).Do32(ucr3).Do32(ucr4).Do32(ufcr).Do32(usr1).Do32(usr2);
        a.Do32(uesc).Do32(utim).Do32(ubir).Do32(ubmr).Do32(ubirActive).Do32(ubmrActive);
        a.DoBuffer16(presets, 8).Do32(presetWritten).DoBool(incrementWritten).DoBool(modulatorAlone);
        a.DoBuffer16(rxFifo, 32).Do32(rxHead).Do32(rxCount).Do16(lastReceived).DoBool(receivedRead);
        a.DoBuffer(txFifo, 32).Do32(txHead).Do32(txCount).Do8(shiftCharacter).DoBool(shifting).Do32(shiftBits);
        a.DoBool(txEmptyInterrupt).DoBool(txSeveral).Do64(bitPhase).Do32(resetCycles);
        a.Do32(idleBits).DoBool(idleArmed).Do64(transmitted);
        a.DoBool(rtsKnown).DoBool(rtsLow).DoBool(dtrKnown).DoBool(dtrLow);
        a.Require(rxHead < 32 && rxCount <= 32 && txHead < 32 && txCount <= 32, "uart fifo");
    }

   private:
    bool running() const { return (ucr1 & 5) == 5; }
    // A waiting character needs the RTS pin's level unless IRTS is set.
    bool rtsNeeded(uint32_t control2) const { return (control2 & 0x4004) == 4 && !rtsKnown; }
    bool transmitterOn() const { return (ucr2 & 4) && ((ucr2 & 0x4000) || (rtsKnown && rtsLow)); }
    bool active() const { return running() && (shifting || txCount || idlePending()); }
    bool idlePending() const { return idleArmed && rxCount && (ucr2 & 2); }
    // Bit times of 4, 8, 16 or 32 frames.
    uint64_t idleLimit() const { return idleLimit(ucr1, ucr2); }
    static uint64_t idleLimit(uint32_t control1, uint32_t control2) {
        const uint32_t frame = 1 + ((control2 & 0x20) ? 8 : 7) + ((control2 & 0x100) ? 1 : 0) + 1;
        return uint64_t(frame) << (2 + ((control1 >> 10) & 3));
    }
    // A shorter idle condition or frame than the idle time already counted. The manual doesn't say what follows.
    bool idleCountPast(uint32_t control1, uint32_t control2) const {
        return idleArmed && rxCount && idleBits > idleLimit(control1, control2);
    }
    // One bit time is 16 BRM clocks; in units of PERCLK1 cycles times UBIR + 1.
    uint64_t bitPeriod() const {
        static constexpr unsigned dividers[]{6, 5, 4, 3, 2, 1, 7, 1};
        return uint64_t(16) * dividers[(ufcr >> 7) & 7] * (uint64_t(ubmrActive) + 1);
    }
    uint32_t status1() const {
        return usr1 | (rtsKnown && rtsLow ? 0x4000 : 0) | (txCount <= (ufcr >> 10) ? 0x2000 : 0) |
               (rxCount >= (ufcr & 63) ? 0x200 : 0) | (running() && (ucr2 & 2) && !resetCycles ? 0x40 : 0);
    }
    uint32_t status2() const {
        return usr2 | (txCount ? 0 : 0x4000) | (!txCount && !shifting ? 8 : 0) | (rxCount ? 1 : 0);
    }
    void stopTransmitter() { shifting = false; shiftBits = 0; }
    // Resets both state machines, the FIFOs and the status registers, for four reference clocks.
    void softwareReset() {
        static constexpr unsigned dividers[]{6, 5, 4, 3, 2, 1, 7, 1};
        resetCycles = 4 * dividers[(ufcr >> 7) & 7];
        usr1 = usr2 = 0;
        rxHead = rxCount = txHead = txCount = 0;
        receivedRead = false;
        stopTransmitter();
        txEmptyInterrupt = true; txSeveral = false;
        idleBits = 0; idleArmed = false;
        bitPhase = 0;
    }

    bool modem;
    // The manual's UFCR table gives 0x0881 as the reset value and draws the bits as 0x0801 (RFDIV / 6, not / 5).
    uint32_t ucr1 = 4, ucr2 = 0, ucr3 = 0, ucr4 = 0x8040, ufcr = 0x0881;
    // Latched flags only; the level flags come from the FIFOs.
    uint32_t usr1 = 0, usr2 = 0;
    uint32_t uesc = 0x2b, utim = 0, ubir = 0, ubmr = 0, ubirActive = 0, ubmrActive = 0;
    uint16_t presets[8] = {};
    uint32_t presetWritten = 0;
    bool incrementWritten = false, modulatorAlone = false;
    uint16_t rxFifo[32] = {};
    uint32_t rxHead = 0, rxCount = 0;
    uint16_t lastReceived = 0;
    bool receivedRead = false;
    uint8_t txFifo[32] = {};
    uint32_t txHead = 0, txCount = 0;
    uint8_t shiftCharacter = 0;
    bool shifting = false;
    uint32_t shiftBits = 0;
    // The TXFE interrupt is held back between writes, 27.5.2.
    bool txEmptyInterrupt = true, txSeveral = false;
    uint64_t bitPhase = 0;
    uint32_t resetCycles = 0;
    uint32_t idleBits = 0;
    bool idleArmed = false;
    uint64_t transmitted = 0;
    bool rtsKnown = false, rtsLow = false, dtrKnown = false, dtrLow = false;
    Imx1UartTransmit transmit = nullptr;
    void* transmitContext = nullptr;
};

#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Uart) == 264, "update state() and stateVersion");
#endif

#endif
