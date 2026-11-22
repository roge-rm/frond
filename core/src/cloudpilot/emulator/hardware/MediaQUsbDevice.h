#ifndef FROND_MEDIAQ_USB_DEVICE_H
#define FROND_MEDIAQ_USB_DEVICE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "UsbDevicePort.h"

class MediaQUsbDevice {
   public:
    static constexpr uint32_t registers = 0x1000;
    // Bulk IN uses the first 4 KiB window and bulk OUT alternates between the last two.
    static constexpr uint32_t packetRam = 0x3bc00;
    static constexpr unsigned packetRamSize = 0x4000;
    static constexpr unsigned registerSize = 0x80;
    static constexpr uint32_t interruptStatus = 0x10a;

    explicit MediaQUsbDevice(std::function<void(bool)> irq = {}) : interrupt(std::move(irq)) {}

    void SetCable(bool attached) {
        if (cable == attached) return;
        cable = attached;
        ClearTransfers();
        if (Listening()) ++enableCount;
    }

    void Reset() {
        words = {};
        ClearTransfers();
        if (interrupt) interrupt(false);
    }

    bool Handles(uint32_t offset, unsigned size) const {
        return (offset >= registers && offset < registers + registerSize && size <= registers + registerSize - offset) ||
               (offset >= packetRam && offset < packetRam + packetRamSize && size <= packetRam + packetRamSize - offset);
    }

    uint32_t Read(uint32_t offset, unsigned size, bool swapped) {
        if (offset >= packetRam) {
            uint32_t value = 0;
            for (unsigned i = 0; i < size; ++i)
                value = (value << 8) | ram[(offset - packetRam + i) ^ (swapped ? 0 : 1)];
            return value;
        }
        const unsigned reg = offset - registers;
        if (reg == 0x1c || reg == 0x1e) {
            uint32_t value = 0;
            for (unsigned i = 0; i < size; ++i) {
                const unsigned index = i ^ (swapped ? 0 : 1);
                value = (value << 8) | (rxCursor + index < rx.size() ? rx[rxCursor + index] : 0);
            }
            rxCursor += size;
            return value;
        }
        if (size == 4) return (uint32_t(Read(offset, 2, swapped)) << 16) | Read(offset + 2, 2, swapped);
        const uint16_t word = ReadWord(reg & ~1u);
        const uint16_t bus = swapped ? Swap(word) : word;
        return size == 2 ? bus : (bus >> ((1 - (reg & 1)) * 8)) & 0xff;
    }

    void Write(uint32_t offset, unsigned size, uint32_t value, bool swapped) {
        if (offset >= packetRam) {
            for (unsigned i = 0; i < size; ++i)
                ram[(offset - packetRam + i) ^ (swapped ? 0 : 1)] = value >> ((size - i - 1) * 8);
            return;
        }
        const unsigned reg = offset - registers;
        if (reg == 0x20 || reg == 0x22) {
            for (unsigned i = 0; i < size; ++i) {
                const unsigned index = txStage + (i ^ (swapped ? 0 : 1));
                if (index < tx.size()) tx[index] = value >> ((size - i - 1) * 8);
            }
            txStage = std::min<unsigned>(txStage + size, tx.size());
            return;
        }
        if (size == 4) {
            Write(offset, 2, value >> 16, swapped);
            Write(offset + 2, 2, value & 0xffff, swapped);
            return;
        }
        const unsigned wordOffset = reg & ~1u;
        uint16_t mask = 0xffff;
        if (size == 1) {
            const unsigned shift = (1 - (reg & 1)) * 8;
            value <<= shift;
            mask = uint16_t(0xffu << shift);
        }
        WriteWord(wordOffset, swapped ? Swap(value) : uint16_t(value), swapped ? Swap(mask) : mask);
    }

    uint16_t InterruptStatus() const { return events ? 1 : 0; }

    // The bridge reattaches the cable on load. Keep the registers, then discard the old transfers.
    template <typename Chunk>
    void Save(Chunk& chunk) const {
        chunk.Put32(1);
        for (auto word : words) chunk.Put16(word);
    }

    template <typename Chunk>
    bool Load(Chunk& chunk) {
        if (chunk.Get32() != 1) return false;
        for (auto& word : words) word = chunk.Get16();
        ClearTransfers();
        return true;
    }

    UsbDevicePort Port() {
        static const UsbDevicePort::Operations operations{
            [](void* p) { return Self(p).BusReset(); },
            [](void*) { return false; },
            [](void* p) { return Self(p).Listening(); },
            [](void* p) { return Self(p).Listening(); },
            [](void*) { return false; },
            [](void* p) { return Self(p).enableCount; },
            [](void* p) { return Self(p).address; },
            [](void* p, void (*notify)(void*), void* context) {
                Self(p).notify = notify;
                Self(p).notifyContext = context;
            },
            [](void* p, const uint8_t* data) { return Self(p).HostOut(0, data, 8, true); },
            [](void* p, const uint8_t* data, unsigned size) { return Self(p).HostOut(0, data, size, false); },
            [](void* p, uint8_t* data, unsigned* size) { return Self(p).HostIn(0, data, size); },
            [](void* p, uint8_t ep, const uint8_t* data, unsigned size) { return Self(p).HostOut(ep & 0x7f, data, size, false); },
            [](void* p, uint8_t ep, uint8_t* data, unsigned* size) { return Self(p).HostIn(ep & 0x7f, data, size); },
            [](void* p, uint8_t ep, unsigned* size) { return Self(p).HostIn(ep & 0x7f, nullptr, size); },
        };
        return {this, operations};
    }

   private:
    using Result = UsbPacketResult;
    static constexpr uint16_t controlInDone = 0x02, controlOutDone = 0x04, bulkInDone = 0x40,
                              bulkOutDone = 0x80, busReset = 0x800;
    static constexpr unsigned packetSize = 64, bankSize = 4096;
    static MediaQUsbDevice& Self(void* context) { return *static_cast<MediaQUsbDevice*>(context); }
    static uint16_t Swap(uint16_t value) { return (value >> 8) | (value << 8); }

    // The m520 ROM uses 0x0936 while disconnected and 0x08c4/0x08c6 while connected.
    bool Listening() const { return cable && (words[1] & 0x0840) == 0x0840; }
    void Notify() { if (notify) notify(notifyContext); }
    void UpdateInterrupt() {
        const bool asserted = events != 0;
        if (asserted == irqAsserted) return;
        irqAsserted = asserted;
        if (interrupt) interrupt(asserted);
    }

    void ClearTransfers() {
        events = rxStatus = 0;
        rxCursor = txStage = txLength = bulkInOffset = bulkInLength = 0;
        rxPending = txPending = addressPending = false;
        rxBanks = {};
        nextOutBank = 0;
        address = 0;
        words[0x0c / 2] = 0;
        words[0x60 / 2] = 0;
        UpdateInterrupt();
    }

    bool BusReset() {
        if (!Listening()) return false;
        ClearTransfers();
        events = busReset;
        UpdateInterrupt();
        return true;
    }

    uint16_t ReadWord(unsigned reg) const {
        switch (reg) {
            case 0x18: return rxStatus;
            case 0x40: return events;
            case 0x60: return bulkInLength ? 0x100 : 0;
            case 0x6c: return (rxBanks[0].ready ? 0x100 : 0) | (rxBanks[1].ready ? 0x200 : 0);
            case 0x78: return rxBanks[0].length;
            case 0x7a:  // ROM debug driver reads packed counts.
            case 0x7c: return rxBanks[1].length;
            default: return words[reg / 2];
        }
    }

    void WriteWord(unsigned reg, uint16_t value, uint16_t mask) {
        const bool wasListening = Listening();
        uint16_t& word = words[reg / 2];
        word = (word & ~mask) | (value & mask);
        value &= mask;
        switch (reg) {
            case 0x04:
                nextAddress = word & 0x7f;
                addressPending = true;
                break;
            case 0x0c:
                if (value & 0x100) txStage = 0;
                if (value & 2) {
                    const unsigned lanes = (word >> 4) & 15;
                    const unsigned tail = (lanes & 1) + ((lanes >> 1) & 1) +
                                          ((lanes >> 2) & 1) + (lanes >> 3);
                    txLength = txStage >= 4 && tail ? txStage - 4 + tail : 0;
                    txStage = 0;
                    txPending = true;
                }
                break;
            case 0x18:
                if (value & 0xff) rxPending = false;
                break;
            case 0x40:
                events &= ~value;
                UpdateInterrupt();
                break;
            case 0x60:
                if (value & 1) {
                    bulkInOffset = 0;
                    bulkInLength = std::min<unsigned>(unsigned(words[0x66 / 2]) + 1, bankSize);
                }
                break;
            case 0x6c:
                if (value & 0x100) rxBanks[0] = {};
                if (value & 0x200) rxBanks[1] = {};
                break;
        }
        const bool listening = Listening();
        if (!wasListening && listening) ++enableCount;
        if (wasListening && !listening) ClearTransfers();
        Notify();
    }

    Result HostOut(unsigned endpoint, const uint8_t* data, unsigned size, bool setup) {
        if (!Listening()) return Result::Timeout;
        if (endpoint == 0) {
            if (size > rx.size()) return Result::Unsupported;
            if (!setup && rxPending) return Result::Nak;
            if (setup) {
                words[0x0c / 2] = 0;
                txPending = addressPending = false;
                txStage = 0;
            }
            rx.fill(0);
            if (size) std::memcpy(rx.data(), data, size);
            rxCursor = 0;
            rxStatus = (((size + 3) / 4) << 9) | (((4 - size % 4) % 4) << 12) |
                       (setup ? 0xa0 : 0x20);
            rxPending = true;
            events |= controlOutDone;
            UpdateInterrupt();
            return Result::Ack;
        }
        if (endpoint != 3 || !(words[0x38 / 2] & 1)) return Result::Timeout;
        if (size > 32) return Result::Unsupported;
        auto& bank = rxBanks[nextOutBank];
        if (bank.ready) return Result::Nak;
        if (size) std::memcpy(ram.data() + (nextOutBank + 2) * bankSize, data, size);
        bank = {uint16_t(size), true};
        events |= bulkOutDone << nextOutBank;
        nextOutBank ^= 1;
        UpdateInterrupt();
        return Result::Ack;
    }

    Result HostIn(unsigned endpoint, uint8_t* data, unsigned* size) {
        if (!Listening()) return Result::Timeout;
        if (endpoint == 0) {
            if (words[0x0c / 2] & 4) return Result::Stall;
            if (!txPending) return Result::Nak;
            *size = txLength;
            if (data) {
                std::memcpy(data, tx.data(), *size);
                txPending = false;
                words[0x0c / 2] &= ~2;
                events |= controlInDone;
                UpdateInterrupt();
                if (addressPending) {
                    address = nextAddress;
                    addressPending = false;
                }
            }
            return Result::Ack;
        }
        if (endpoint != 2 || !(words[0x30 / 2] & 1)) return Result::Timeout;
        if (!bulkInLength) return Result::Nak;
        *size = std::min(bulkInLength, packetSize);
        if (data) {
            std::memcpy(data, ram.data() + bulkInOffset, *size);
            bulkInOffset += *size;
            bulkInLength -= *size;
            if (!bulkInLength) {
                events |= bulkInDone;
                UpdateInterrupt();
            }
        }
        return Result::Ack;
    }

    struct Bank { uint16_t length{0}; bool ready{false}; };
    std::array<uint16_t, registerSize / 2> words{};
    std::array<uint8_t, packetRamSize> ram{};
    std::array<uint8_t, 16> rx{}, tx{};
    std::array<Bank, 2> rxBanks{};
    uint16_t events{0}, rxStatus{0};
    unsigned rxCursor{0}, txStage{0}, txLength{0}, bulkInOffset{0}, bulkInLength{0}, nextOutBank{0};
    uint32_t enableCount{0};
    uint8_t address{0}, nextAddress{0};
    bool cable{false}, rxPending{false}, txPending{false}, addressPending{false};
    std::function<void(bool)> interrupt;
    bool irqAsserted{false};
    void (*notify)(void*){nullptr};
    void* notifyContext{nullptr};
};

#endif
