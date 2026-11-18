#ifndef FROND_IMX1_GRAPHICS_USB_H
#define FROND_IMX1_GRAPHICS_USB_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include "imx1_bootstrap.h"
#include "UsbPacketResult.h"

// The W4200's USB function (Z10.3), as the ROM's 'W4200 USB Driver' (code 20177400-20178c00) drives it. Facts
// from that code: buffer RAM at 16200000 in words (setup packet words 0-1, EP0 OUT words 2-3, EP0 IN words
// 4-5, each bulk endpoint two 64-byte buffers whose word indexes are in its second register), registers at
// 16201200-162013e0. EP0 packets are 8 bytes. EP1 and EP3 are bulk IN, EP2 and EP4 bulk OUT; EP n's
// registers are 1310 + 16 * (n - 1): configuration, buffers (low half A, high half B), control.
//   1234 bit 0 any enabled event; 1238 bit 0 interrupt out on; 123c written 1 by the handler and the off path.
//   1300 address. 1304 bit 0 soft reset (polled until clear), bits 17-20 written at SET_CONFIGURATION.
//   1308 EP0 commands: 0x10 stall, 0x20 status stage, 0x10000 IN packet ready, 0x20000 OUT buffer free,
//        0x40000 setup buffer free; reads bit 17 OUT packet held, bit 18 setup held. 130c bits 0-3 OUT
//        length, bits 6-9 IN length.
//   IN control (1318, 1338): bits 0-15 packets left (polled to 0 before a new transfer), bits 22-31 packet
//        length. OUT control (1328, 1348): bits 22-31 length of a short packet, written 0xffff to arm.
//   13a0 buffers held (bit n buffer A, bit 16 + n buffer B), 13a4 sets IN buffers ready, 13a8 frees OUT ones.
//   13b0 events: bits 1-4 endpoint n, 12 EP0 IN sent, 13 EP0 OUT, 14 setup, 15, 16 + n a short OUT packet on
//        n; 13b4 enable (0xf01e), 13b8 acknowledge. 13c0 bus events (bit 0 reset), 13c4 enable, 13c8 acknowledge.
// The chip side: 160100a8 bits 0-1 pulsed at start, b30 bits 0 and 10 set while on, b1c and b10 written once,
// and interrupt group 0x50 at 16010200/204 (bit 6 enabled by the driver) calls the driver's handler.
// ASSUMPTION "W4200 USB timing": every packet moves at once, between guest slices, and a bus reset or soft
// reset ends at once. ASSUMPTION "W4200 USB one OUT packet": an OUT endpoint takes a packet only while neither
// of its buffers is held, and arming it (a control write) starts again at buffer A. ASSUMPTION "W4200 USB
// pull-up": the function is on the bus (listening) while b30 bit 0, 1200 bit 0 and 1238 bit 0 are set.
class Imx1GraphicsUsb {
   public:
    static constexpr uint32_t ramWords = 256;  // ASSUMPTION: 1 KiB of buffer RAM; the driver uses words 0-0x85.
    static bool owns(uint32_t address) {
        return (address >= 0x16201200 && address <= 0x162013e0) || (address >= 0x16200000 && address < 0x16200000 + ramWords * 4);
    }
    static bool chipWord(uint32_t address) {
        return address == 0x160100a8 || address == 0x16010b10 || address == 0x16010b1c || address == 0x16010b30;
    }
    static bool driverPc(uint32_t pc) { return pc >= 0x20177400 && pc < 0x20178c00; }
    void reset() { *this = {}; }
    // True once the driver has touched it: then the state goes in its own section.
    bool used() const { return touched; }
    bool clocked() const { return b30 & 1; }
    bool listening() const { return clocked() && (control & 1) && (interruptOn & 1); }
    uint32_t enableCount() const { return enables; }
    uint8_t address() const { return uint8_t(busAddress); }
    bool line() const { return (interruptOn & 1) && summary(); }

    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(ram.data(), ram.size()).DoBuffer32(endpoint.data(), endpoint.size());
        a.DoBuffer16(sizes.data(), sizes.size());
        a.Do32(b30).Do32(b1c).Do32(b10).Do32(pulse).Do32(control).Do32(r1210).Do32(r1230).Do32(interruptOn);
        a.Do32(addressValue).Do32(ep0Status).Do32(ep0Length).Do32(held).Do32(events).Do32(eventEnable);
        a.Do32(busEvents).Do32(busEnable).Do32(enables).Do32(pingPong).Do32(busAddress);
        a.DoBool(touched).DoBool(stalled).DoBool(statusReady).DoBool(ep0InReady).DoBool(setupIn).DoBool(wasListening);
    }

    // The chip words around the block, from the driver only. a8's other bits belong to the clocks model.
    Imx1BootstrapAccess chipAccess(uint32_t pc, uint32_t address, bool write, uint32_t& value, uint32_t a8) {
        using A = Imx1BootstrapAccess;
        if (!driverPc(pc)) return A::externalInput;
        touched = true;
        if (address == 0x160100a8) {
            if (!write) { value = a8 | pulse; return A::ok; }
            if ((value & ~3u) != a8) return A::sideEffect;
            // ASSUMPTION "a8 bits 0-1 reset the USB block": set then cleared before anything else is written.
            if ((pulse & ~value) && !(value & 3)) resetBlock();
            pulse = value & 3;
            return A::ok;
        }
        uint32_t* word = address == 0x16010b30 ? &b30 : address == 0x16010b1c ? &b1c : &b10;
        if (write) {
            if (address == 0x16010b30 && (value & ~0x401u)) return A::sideEffect;
            *word = value;
            noteListening();
        } else value = *word;
        return A::ok;
    }
    // b30 from the display off code (200c5348 read, 200c537c write back with bit 0 clear).
    uint32_t displayRead() const { return b30; }
    bool displayWrite(uint32_t value) {
        if (value != (b30 & ~1u)) return false;
        b30 = value;
        noteListening();
        return true;
    }

    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (!driverPc(pc)) return A::externalInput;
        if (size != 4) return A::width;
        touched = true;
        if (address < 0x16201200) {
            uint32_t& word = ram[(address - 0x16200000) / 4];
            if (write) word = value; else value = word;
            return A::ok;
        }
        const uint32_t offset = address - 0x16200000;
        if (offset >= 0x1310 && offset < 0x1350) {
            uint32_t& word = endpoint[(offset - 0x1310) / 4];
            const unsigned n = (offset - 0x1310) / 16 + 1, part = (offset & 15) / 4;
            if (part == 3) return A::externalInput;
            if (!write) { value = word; return A::ok; }
            word = value;
            // Arming an OUT endpoint starts again at buffer A.
            if (part == 2 && !in(n)) pingPong &= ~(1u << n);
            return A::ok;
        }
        if (!write) {
            switch (offset) {
                case 0x1200: value = control; break;
                case 0x1210: value = r1210; break;
                case 0x1234: value = summary() ? 1 : 0; break;
                case 0x1238: value = interruptOn; break;
                case 0x1300: value = addressValue; break;
                case 0x1304: value = 0; break;  // a soft reset ends at once
                case 0x1308: value = ep0Status; break;
                case 0x130c: value = ep0Length; break;
                case 0x13a0: value = held; break;
                case 0x13b0: value = events; break;
                case 0x13b4: value = eventEnable; break;
                case 0x13c0: value = busEvents; break;
                case 0x13c4: value = busEnable; break;
                // ASSUMPTION "W4200 USB 13e0 reads 0": read once at start and dropped.
                case 0x13e0: value = 0; break;
                default: return A::externalInput;
            }
            return A::ok;
        }
        switch (offset) {
            case 0x1200: control = value; break;
            case 0x1210: r1210 = value; break;
            case 0x1230: r1230 = value; break;
            case 0x1238: if (value & ~1u) return A::sideEffect; interruptOn = value; break;
            case 0x123c: if (value != 1) return A::sideEffect; break;
            case 0x1300: if (value & ~0x7fu) return A::sideEffect; addressValue = value; break;
            case 0x1304:
                if (value & ~0x1e0001u) return A::sideEffect;
                if (value & 1) softReset();
                for (unsigned n = 1; n <= 4; ++n) if (value & (1u << (16 + n))) resetEndpoint(n);
                break;
            case 0x1308:
                if (value & ~0x70030u) return A::sideEffect;
                if (value & 0x10) stalled = true;
                if (value & 0x20) statusReady = true;
                if (value & 0x10000) ep0InReady = true;
                if (value & 0x20000) ep0Status &= ~0x20000u;
                if (value & 0x40000) ep0Status &= ~0x40000u;
                break;
            case 0x130c: ep0Length = (ep0Length & 15) | (value & 0x3c0); break;
            case 0x13a4:
                if (value & ~0xa000au) return A::sideEffect;
                for (unsigned bit = 0; bit < 32; ++bit)
                    if (value & (1u << bit)) sizes[bufferIndex(bit)] = uint16_t(std::min<uint32_t>(endpoint[((bit & 15) - 1) * 4 + 2] >> 22, 64));
                held |= value;
                break;
            case 0x13a8: if (value & ~0x140014u) return A::sideEffect; held &= ~value; break;
            case 0x13b4: if (value & ~0xf01eu) return A::sideEffect; eventEnable = value; break;
            case 0x13b8: events &= ~value; break;
            case 0x13c4: if (value & ~0x1fu) return A::sideEffect; busEnable = value; break;
            case 0x13c8: busEvents &= ~value; break;
            default: return A::externalInput;
        }
        noteListening();
        return A::ok;
    }

    // The host side, between guest slices.
    bool busReset() {
        if (!listening()) return false;
        for (unsigned n = 1; n <= 4; ++n) resetEndpoint(n);
        resetControl();
        addressValue = busAddress = 0;
        busEvents |= 1;
        return true;
    }
    UsbPacketResult setup(const uint8_t* data) {
        if (!listening()) return UsbPacketResult::Timeout;
        resetControl();
        std::memcpy(&ram[0], data, 4);
        std::memcpy(&ram[1], data + 4, 4);
        setupIn = data[0] & 0x80;
        ep0Status |= 0x40000;
        events |= 1u << 14;
        return UsbPacketResult::Ack;
    }
    UsbPacketResult ep0Out(const uint8_t* data, unsigned size) {
        if (!listening()) return UsbPacketResult::Timeout;
        if (size > 8) return UsbPacketResult::Unsupported;
        if (stalled) return UsbPacketResult::Stall;
        if (setupIn) {
            // The status stage of a control read, once the driver has written 0x20. The driver also arms an
            // empty IN packet after a last packet of exactly 8 bytes, which the host never asks for; the status
            // stage drops it.
            if (size || !statusReady) return UsbPacketResult::Nak;
            statusReady = ep0InReady = false;
            return UsbPacketResult::Ack;
        }
        if (ep0Status & 0x60000) return UsbPacketResult::Nak;
        uint8_t bytes[8]{};
        if (size) std::memcpy(bytes, data, size);
        std::memcpy(&ram[2], bytes, 4);
        std::memcpy(&ram[3], bytes + 4, 4);
        ep0Length = (ep0Length & ~15u) | size;
        ep0Status |= 0x20000;
        events |= 1u << 13;
        return UsbPacketResult::Ack;
    }
    UsbPacketResult ep0In(uint8_t* data, unsigned* size, bool take) {
        if (!listening()) return UsbPacketResult::Timeout;
        if (stalled) return UsbPacketResult::Stall;
        if (ep0InReady) {
            const unsigned length = std::min<unsigned>((ep0Length >> 6) & 15, 8);
            *size = length;
            if (!take) return UsbPacketResult::Ack;
            uint8_t bytes[8];
            std::memcpy(bytes, &ram[4], 4);
            std::memcpy(bytes + 4, &ram[5], 4);
            if (data && length) std::memcpy(data, bytes, length);
            ep0InReady = false;
            // In a control write or a request with no data, an IN packet is the status stage.
            if (!setupIn) statusReady = false;
            events |= 1u << 12;
            // ASSUMPTION "W4200 USB address after status": the driver writes 1300 before it arms the status
            // packet of SET_ADDRESS, and the function answers on the new address once that packet has gone.
            busAddress = addressValue & 0x7f;
            return UsbPacketResult::Ack;
        }
        // The status stage of a control write: an empty packet once the driver has written 0x20.
        if (!setupIn && statusReady) {
            *size = 0;
            if (take) statusReady = false;
            return UsbPacketResult::Ack;
        }
        return UsbPacketResult::Nak;
    }
    UsbPacketResult endpointIn(unsigned n, uint8_t* data, unsigned* size, bool take) {
        if (!listening()) return UsbPacketResult::Timeout;
        if (n < 1 || n > 4 || !in(n)) return UsbPacketResult::Unsupported;
        // Buffer B only when A isn't ready; the driver fills one buffer at a time.
        const uint32_t a = 1u << n, b = 1u << (16 + n);
        const uint32_t bit = held & a ? a : held & b ? b : 0;
        if (!bit) return UsbPacketResult::Nak;
        const unsigned index = bufferIndex(bit == a ? n : 16 + n), length = sizes[index];
        *size = length;
        if (!take) return UsbPacketResult::Ack;
        copyOut(bufferWord(n, bit == b), data, length);
        held &= ~bit;
        uint32_t& control = endpoint[(n - 1) * 4 + 2];
        if (control & 0xffff) --control;
        events |= a;
        return UsbPacketResult::Ack;
    }
    bool endpointOutReady(unsigned n) const { return n >= 1 && n <= 4 && !in(n) && !(held & (0x10001u << n)); }
    UsbPacketResult endpointOut(unsigned n, const uint8_t* data, unsigned size) {
        if (!listening()) return UsbPacketResult::Timeout;
        if (n < 1 || n > 4 || in(n) || size > 64) return UsbPacketResult::Unsupported;
        if (!endpointOutReady(n)) return UsbPacketResult::Nak;
        const bool second = pingPong & (1u << n);
        copyIn(bufferWord(n, second), data, size);
        held |= second ? 1u << (16 + n) : 1u << n;
        pingPong ^= 1u << n;
        uint32_t& control = endpoint[(n - 1) * 4 + 2];
        control = (control & 0x3fffff) | (size << 22);
        events |= 1u << n;
        if (size < 64) events |= 1u << (16 + n);
        return UsbPacketResult::Ack;
    }

   private:
    bool summary() const { return (events & eventEnable) || (busEvents & busEnable); }
    bool in(unsigned n) const { return endpoint[(n - 1) * 4] & 8; }
    static unsigned bufferIndex(unsigned bit) { return ((bit & 15) - 1) * 2 + (bit >= 16); }
    uint32_t bufferWord(unsigned n, bool second) const {
        const uint32_t buffers = endpoint[(n - 1) * 4 + 1];
        return second ? buffers >> 16 : buffers & 0xffff;
    }
    void copyOut(uint32_t word, uint8_t* data, unsigned length) const {
        for (unsigned i = 0; i < length && data; ++i) {
            const uint32_t w = word + i / 4;
            data[i] = w < ram.size() ? uint8_t(ram[w] >> (8 * (i & 3))) : 0;
        }
    }
    void copyIn(uint32_t word, const uint8_t* data, unsigned length) {
        for (unsigned i = 0; i < length; ++i) {
            const uint32_t w = word + i / 4;
            if (w >= ram.size()) break;
            const unsigned shift = 8 * (i & 3);
            ram[w] = (ram[w] & ~(0xffu << shift)) | (uint32_t(data[i]) << shift);
        }
    }
    void resetControl() {
        stalled = statusReady = ep0InReady = false;
        ep0Status = 0;
        ep0Length &= 0x3c0;
    }
    void resetEndpoint(unsigned n) {
        held &= ~(0x10001u << n);
        pingPong &= ~(1u << n);
    }
    void softReset() {
        for (unsigned n = 1; n <= 4; ++n) resetEndpoint(n);
        resetControl();
        events = busEvents = 0;
        addressValue = busAddress = 0;
    }
    void resetBlock() {
        softReset();
        control = r1210 = r1230 = interruptOn = eventEnable = busEnable = 0;
        endpoint = {};
        sizes = {};
    }
    void noteListening() {
        const bool now = listening();
        if (now && !wasListening) ++enables;
        wasListening = now;
    }
    std::array<uint32_t, ramWords> ram{};
    std::array<uint32_t, 16> endpoint{};
    std::array<uint16_t, 8> sizes{};
    uint32_t b30 = 0, b1c = 0, b10 = 0, pulse = 0, control = 0, r1210 = 0, r1230 = 0, interruptOn = 0;
    uint32_t addressValue = 0, ep0Status = 0, ep0Length = 0, held = 0, events = 0, eventEnable = 0;
    uint32_t busEvents = 0, busEnable = 0, enables = 0, pingPong = 0, busAddress = 0;
    bool touched = false, stalled = false, statusReady = false, ep0InReady = false, setupIn = false, wasListening = false;
};

#endif
