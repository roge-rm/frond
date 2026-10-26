#ifndef FROND_USB_HOST_FIXTURE_H
#define FROND_USB_HOST_FIXTURE_H

// White-box investigation fixture, NOT a production USB host/controller API.
// No transport, timers, callbacks or background activity. Drive it only between
// guest execution slices. It seeds the documented bus-facing EP0 status/FIFO
// fields; address decoding, automatic requests and wire/ACK timing are absent.
// In particular, it never saves flushed data or invents a delayed completion.
#include <array>
#include <cstdint>
#include <cstring>
#include "pxa270_UDC.cpp"

struct UsbHostFixture {
    Pxa270Udc* udc; // owned by the test's SoC, never retained beyond that lifetime
    struct Packet { std::array<uint8_t, 16> bytes{}; unsigned size = 0; };
    enum class Result { Data, Nak, Stall, Unsupported };

    bool pioEnabled() const { return (udc->udccr & 1) && !(udc->ep[0].csr & 8); }
    void packetComplete() {
        udc->udcisr[0] |= udc->udcicr[0] & 1;
        pxa270UdcPrvIrq(udc);
    }
    // Minimal reset stimulus for studying the firmware IRQ handler. Retain the
    // enable/configuration/mask registers; this is not a complete USB bus reset.
    bool resetStimulus() {
        if (!pioEnabled()) return false;
        pxa270UdcPrvFlushTx0(udc); pxa270UdcPrvFlushRx0(udc);
        for (auto& ep : udc->ep) { ep.csr = 0; ep.bcr = 0; }
        udc->udccr &= 0x80000001u;
        udc->udcisr[0] = 0;
        udc->udcisr[1] = udc->udcicr[1] & (1u << 27);
        pxa270UdcPrvIrq(udc);
        return true;
    }
    bool descriptorSetup(uint16_t length) {
        if (!length || !pioEnabled()) return false;
        const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, uint8_t(length), uint8_t(length >> 8)};
        pxa270UdcPrvFlushTx0(udc); pxa270UdcPrvFlushRx0(udc);
        std::memcpy(udc->rx0, setup, 8); udc->ep[0].bcr = 8;
        udc->ep[0].csr = (udc->ep[0].csr & 0x208) | 0xc1; // SA, RNE, OPC
        udc->udccr |= 2; // active during the injected transaction
        packetComplete();
        return true;
    }
    Result in(Packet& packet) {
        packet = {};
        if (!pioEnabled()) return Result::Unsupported;
        if (udc->ep[0].csr & 0x20) return Result::Stall;
        if ((udc->ep[0].csr & 0x81) || !(udc->ep[0].csr & 2)) return Result::Nak;
        packet.size = udc->tx0Count;
        if (packet.size > packet.bytes.size()) return Result::Unsupported;
        std::memcpy(packet.bytes.data(), udc->tx0, packet.size);
        pxa270UdcPrvFlushTx0(udc);
        packetComplete(); // atomic successful IN + ACK, no inferred wire delay
        return Result::Data;
    }
    bool statusOut() {
        if (!pioEnabled() || (udc->ep[0].csr & 0x81)) return false;
        pxa270UdcPrvFlushTx0(udc); pxa270UdcPrvFlushRx0(udc);
        udc->ep[0].csr |= 1;
        udc->udccr &= ~2u;
        packetComplete();
        return true;
    }
    template<class T> void Save(T& state) { pxa270UdcSave(udc, state); }
    void Load(SavestateLoader<ChunkType>& state) { pxa270UdcLoad(udc, state); }
};
#endif
