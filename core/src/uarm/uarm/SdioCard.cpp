#include "SdioCard.h"

#include <algorithm>

namespace {
constexpr uint32_t addressLimit = 0x20000;
constexpr uint32_t voltageWindow = 0x00300000;  // 3.2 to 3.4 V
constexpr uint32_t rca = 3u << 16;
constexpr uint32_t writeFlag = 1u << 31;
constexpr uint32_t modeFlag = 1u << 27;
constexpr uint32_t incrementFlag = 1u << 26;
constexpr uint32_t r5Command = 0x1000;
constexpr uint32_t r5Illegal = 0x4000;
constexpr uint32_t r5Error = 0x0800;
constexpr uint32_t r5Function = 0x0200;
constexpr uint32_t r5Range = 0x0100;
}

SdioCard::SdioCard(SdioFunctions& functions, unsigned count, uint32_t cisAddress)
    : functions(functions),
      count(std::clamp(count, 1u, 7u)),
      cisAddress(cisAddress),
      functionMask((2u << this->count) - 2u) {}

void SdioCard::Reset() {
    enabled = interruptEnable = pending = busWidth = 0;
    ready = addressed = selected = false;
    blockSize.fill(0);
    AbortTransfer();
    functions.Reset();
}

void SdioCard::SetPending(uint8_t mask) { pending = mask & functionMask; }

bool SdioCard::IrqAsserted() const {
    return selected && (interruptEnable & 1) && (interruptEnable & pending & enabled);
}

void SdioCard::AbortTransfer() { transfer = {}; }

bool SdioCard::StateValid() const {
    if ((enabled & ~functionMask) || (interruptEnable & ~(functionMask | 1)) ||
        (pending & ~functionMask) || (busWidth & ~0x82) || (addressed && !ready) ||
        (selected && !addressed)) return false;
    if (!transfer.blocks) return true;

    return selected && transfer.function <= count && transfer.length > 0 &&
           transfer.length <= 2048 && transfer.blocks <= 511 &&
           transfer.address < addressLimit &&
           (!transfer.increment || transfer.blocks * transfer.length <= addressLimit - transfer.address);
}

bool SdioCard::Read(unsigned function, uint32_t address, uint8_t& value) {
    if (function) return functions.Read(function, address, value);

    switch (address) {
        case 0x00:
            value = 0x11;  // CCCR 1.10, SDIO 1.10
            return true;
        case 0x01:
            value = 0x01;  // SD Physical 1.10
            return true;
        case 0x02:
        case 0x03:
            value = enabled;  // Functions become ready immediately.
            return true;
        case 0x04:
            value = interruptEnable;
            return true;
        case 0x05:
            value = pending & enabled;
            return true;
        case 0x06:
            value = 0;
            return true;
        case 0x07:
            value = busWidth;
            return true;
        case 0x08:
            value = 2;  // SMB, multi-block CMD53
            return true;
        case 0x09:
        case 0x0a:
        case 0x0b:
            value = cisAddress >> (8 * (address - 9));
            return true;
    }

    const unsigned fbr = address >> 8;
    const unsigned offset = address & 0xff;
    if (fbr <= count && (offset == 0x10 || offset == 0x11)) {
        value = blockSize[fbr] >> (8 * (offset - 0x10));
        return true;
    }
    if (address >= 0x100 && functions.Read(0, address, value)) return true;
    value = 0;  // Reserved common registers.
    return true;
}

bool SdioCard::Write(unsigned function, uint32_t address, uint8_t value) {
    if (function) return functions.Write(function, address, value);

    switch (address) {
        case 0x02:
            enabled = value & functionMask;
            return true;
        case 0x04:
            interruptEnable = value & (functionMask | 1);
            return true;
        case 0x06:
            if (value & 8)
                Reset();
            else if (transfer.function == (value & 7))
                AbortTransfer();
            return true;
        case 0x07:
            busWidth = value & 0x82;
            return true;
    }

    const unsigned fbr = address >> 8;
    const unsigned offset = address & 0xff;
    if (fbr <= count && offset == 0x10)
        blockSize[fbr] = (blockSize[fbr] & 0xff00) | value;
    else if (fbr <= count && offset == 0x11)
        blockSize[fbr] = (blockSize[fbr] & 0xff) | (uint16_t(value) << 8);
    return true;
}

uint32_t SdioCard::Direct(uint32_t arg) {
    const unsigned function = (arg >> 28) & 7;
    const uint32_t address = (arg >> 9) & (addressLimit - 1);
    const bool write = arg & writeFlag;
    const uint32_t state = selected ? r5Command : 0;
    // The IO reset is also available before enumeration.
    if (!selected && !(write && function == 0 && address == 6 && (arg & 8)))
        return state | r5Illegal;
    if (function > count) return state | r5Function;
    if (function && !(enabled & (1u << function))) return state | r5Error;

    uint8_t value = arg;
    if (write && !Write(function, address, value)) return state | r5Range;
    if ((!write || (arg & modeFlag)) && !Read(function, address, value))
        return state | r5Range;
    return state | value;
}

uint32_t SdioCard::Extended(uint32_t arg) {
    AbortTransfer();
    if (!selected) return r5Illegal;
    const unsigned function = (arg >> 28) & 7;
    if (function > count) return r5Command | r5Function;
    if (function && !(enabled & (1u << function))) return r5Command | r5Error;

    const bool blockMode = arg & modeFlag;
    const bool increment = arg & incrementFlag;
    const uint32_t address = (arg >> 9) & (addressLimit - 1);
    const unsigned amount = arg & 0x1ff;
    const unsigned blocks = blockMode ? amount : 1;
    const unsigned length = blockMode ? blockSize[function] : (amount ? amount : 512);
    // Open-ended transfers need host stop handling. This bus supports finite transfers.
    if (!blocks || !length || length > 2048) return r5Command | r5Range;
    if (increment && blocks * length > addressLimit - address) return r5Command | r5Range;
    transfer = {address, uint16_t(length), uint16_t(blocks), uint8_t(function), bool(arg & writeFlag), increment};
    return r5Command;
}

SdReplyType SdioCard::Command(uint8_t command, uint32_t arg, uint8_t* reply) {
    uint32_t response;
    auto type = SdReply48bits;
    switch (command) {
        case 5:
            if (arg & voltageWindow) ready = true;
            response = voltageWindow | (count << 28) | (ready ? 0x80000000u : 0);
            break;
        case 3:
            if (!ready || selected) return SdReplyNone;
            addressed = true;
            response = rca;
            break;
        case 7:
            if (!addressed) return SdReplyNone;
            selected = (arg & 0xffff0000) == rca;
            if (!selected) {
                AbortTransfer();
                return SdReplyNone;
            }
            response = 0x1e00;  // R1, SDIO-only current state.
            type = SdReply48bitsAndBusy;
            break;
        case 52:
            response = Direct(arg);
            break;
        case 53:
            response = Extended(arg);
            break;
        default:
            return SdReplyNone;
    }

    reply[0] = command == 5 ? 0x3f : command;
    reply[1] = response >> 24;
    reply[2] = response >> 16;
    reply[3] = response >> 8;
    reply[4] = response;
    reply[5] = command == 5 ? 0xff : vsdCRC7(reply, 5);
    return type;
}

bool SdioCard::AcceptsTransfer(bool write, uint16_t length, uint16_t blocks) const {
    return transfer.blocks && transfer.blocks == blocks && transfer.length == length && transfer.write == write;
}

SdDataReplyType SdioCard::ReadBlock(void* data, uint32_t length) {
    if (!transfer.blocks || transfer.write) return SdDataErrWrongCurrentState;
    if (length != transfer.length) return SdDataErrWrongBlockSize;
    auto* bytes = static_cast<uint8_t*>(data);
    for (uint32_t i = 0; i < length; ++i) {
        if (!Read(transfer.function, transfer.address, bytes[i])) {
            AbortTransfer();
            return SdDataErrBackingStore;
        }
        transfer.address += transfer.increment;
    }
    --transfer.blocks;
    return SdDataOk;
}

SdDataReplyType SdioCard::WriteBlock(const void* data, uint32_t length) {
    if (!transfer.blocks || !transfer.write) return SdDataErrWrongCurrentState;
    if (length != transfer.length) return SdDataErrWrongBlockSize;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < length; ++i) {
        if (!Write(transfer.function, transfer.address, bytes[i])) {
            AbortTransfer();
            return SdDataErrBackingStore;
        }
        // A write to the CCCR reset/abort register can end this transfer.
        if (!transfer.blocks) return SdDataErrWrongCurrentState;
        transfer.address += transfer.increment;
    }
    --transfer.blocks;
    return SdDataOk;
}
