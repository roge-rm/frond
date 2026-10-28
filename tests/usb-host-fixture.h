#ifndef FROND_USB_HOST_FIXTURE_H
#define FROND_USB_HOST_FIXTURE_H

// White-box test host for endpoint 0, NOT a production USB host. No transport,
// timers, callbacks or background activity: drive it only between guest slices.
// Every bus event goes through the UDC's host-side entry points; the fixture only
// reads model fields for assertions. It has no clock, so the caller must let at
// least inTransactionNs() of guest time pass after IPR is set before calling in().
#include <array>
#include <cstdint>
#include <cstring>
#include "pxa270_UDC.cpp"

struct UsbHostFixture {
    Pxa270Udc* udc; // owned by the test's SoC, never retained beyond that lifetime
    struct Packet { std::array<uint8_t, 16> bytes{}; unsigned size = 0; };
    enum class Result { Data, Nak, Stall, Unsupported };

    // Shortest full-speed (12 Mbit/s) IN transaction carrying `bytes`, in bit times:
    // IN token (SYNC, PID, ADDR, ENDP, CRC5: 32), DATAx (SYNC, PID, CRC16: 32 + 8 *
    // bytes), ACK (SYNC, PID: 16), a 2-bit SE0 EOP after each packet and the 2-bit
    // minimum inter-packet gap twice. No bit stuffing, so a lower bound.
    static uint64_t inTransactionNs(unsigned bytes) {
        const uint64_t bits = 32 + 32 + 8ull * bytes + 16 + 3 * 2 + 2 * 2;
        return (bits * 1000 + 11) / 12;
    }

    bool resetStimulus() { return pxa270UdcHostReset(udc); }
    bool setup(const uint8_t request[8]) { return pxa270UdcHostSetup(udc, request) == Pxa270UdcHostResult::Ack; }
    bool descriptorSetup(uint16_t length, uint8_t type = 1, uint8_t index = 0) {
        if (!length) return false;
        const uint8_t request[8] = {0x80, 6, index, type, 0, 0, uint8_t(length), uint8_t(length >> 8)};
        return setup(request);
    }
    Result in(Packet& packet) {
        packet = {};
        switch (pxa270UdcHostIn(udc, packet.bytes.data(), &packet.size)) {
            case Pxa270UdcHostResult::Ack: return Result::Data;
            case Pxa270UdcHostResult::Nak: return Result::Nak;
            case Pxa270UdcHostResult::Stall: return Result::Stall;
            default: return Result::Unsupported;
        }
    }
    Result out(const uint8_t* data, unsigned size) {
        switch (pxa270UdcHostOut(udc, data, size)) {
            case Pxa270UdcHostResult::Ack: return Result::Data;
            case Pxa270UdcHostResult::Nak: return Result::Nak;
            case Pxa270UdcHostResult::Stall: return Result::Stall;
            default: return Result::Unsupported;
        }
    }
    bool statusOut() { return out(nullptr, 0) == Result::Data; }
    template<class T> void Save(T& state) { pxa270UdcSave(udc, state); }
    void Load(SavestateLoader<ChunkType>& state) { pxa270UdcLoad(udc, state); }
};
#endif
