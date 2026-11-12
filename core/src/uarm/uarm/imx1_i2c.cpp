#include "imx1_i2c.h"

uint32_t Imx1I2c::clockDivider() const {
    // MC9328MX1RM table 29-5, HCLK clocks per SCL period.
    static constexpr uint16_t dividers[] = {
        30,32,36,42,48,52,60,72,80,88,104,128,144,160,192,240,
        288,320,384,480,576,640,768,960,1152,1280,1536,1920,2304,2560,3072,3840,
        22,24,26,28,32,36,40,44,48,56,64,72,80,96,112,128,
        160,192,224,256,320,384,448,512,640,768,896,1024,1280,1536,1792,2048
    };
    return dividers[frequency];
}

const char* Imx1I2c::registerName(uint32_t address) {
    if (address < 0x00217000 || address > 0x00217013) return nullptr;
    static const char* const names[] = {"I2C.IADR", "I2C.IFDR", "I2C.I2CR", "I2C.I2SR", "I2C.I2DR"};
    return names[(address - 0x00217000) / 4];
}
void Imx1I2c::notify(const Imx1I2cTransfer& transfer, Imx1I2cResponse response) const {
    if (bus && (transfer.phase == Imx1I2cPhase::start || transfer.phase == Imx1I2cPhase::repeatedStart ||
                transfer.phase == Imx1I2cPhase::stop)) bus->signal(transfer);
    if (observer) observer(observerContext, transfer, response);
}
void Imx1I2c::launch(Imx1I2cTransfer transfer) {
    pending = transfer;
    remaining = uint64_t(clockDivider()) * 9;
    inFlight = true;
    status &= ~0x80;
}
void Imx1I2c::advance(uint64_t hclkCycles) {
    if (!inFlight || failure != Imx1I2cAccess::ok) return;
    if (hclkCycles < remaining) { remaining -= hclkCycles; return; }
    remaining = 0;
    const auto reply = bus ? bus->transfer(pending) : Imx1I2cReply{};
    auto completed = pending;
    if (pending.phase == Imx1I2cPhase::receive && reply.response == Imx1I2cResponse::ack)
        completed.byte = reply.byte;
    if (reply.response == Imx1I2cResponse::unknown) {
        failure = Imx1I2cAccess::externalInput;
        notify(completed, reply.response);
        return;
    }
    // Electrical arbitration and stretching need a qualified multi-master bus.
    if (reply.response == Imx1I2cResponse::arbitrationLost ||
        (pending.phase == Imx1I2cPhase::receive && reply.response != Imx1I2cResponse::ack)) {
        failure = Imx1I2cAccess::sideEffect;
        notify(completed, reply.response);
        return;
    }
    inFlight = false;
    status |= 0x82;
    if (pending.phase == Imx1I2cPhase::receive) {
        data = reply.byte;
        receivedNack = !pending.acknowledge;
    } else {
        if (reply.response == Imx1I2cResponse::ack) status &= ~1;
        else status |= 1;
        if (pending.phase == Imx1I2cPhase::address) {
            addressed = reply.response == Imx1I2cResponse::ack;
            readDirection = pending.read;
        }
    }
    notify(completed, reply.response);
}

Imx1I2cAccess Imx1I2c::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1I2cAccess::unmapped;
    // This bootstrap subset qualifies aligned word register accesses only.
    if (size != 4 || (address & 3)) return Imx1I2cAccess::width;
    const unsigned index = (address - 0x00217000) / 4;
    constexpr uint32_t masks[] = {0xfe, 0x3f, 0xfc, 0xf7, 0xff};
    // Reserved fields are read only. Writes cannot latch them.
    const uint32_t written = value & masks[index];
    if (!write) {
        if (index == 3 && (control & 0x80) && !singleMaster) return Imx1I2cAccess::externalInput;
        if (index == 4 && (control & 0x80) && !(control & 0x10) && active()) {
            if (!addressed || !readDirection || inFlight || receivedNack) return Imx1I2cAccess::sideEffect;
            const uint8_t previous = data;
            launch({Imx1I2cPhase::receive, pending.address, 0, true, !(control & 8)});
            value = previous;
            return Imx1I2cAccess::ok;
        }
        const uint32_t values[] = {ownAddress, frequency, control, status, data};
        value = values[index];
        return Imx1I2cAccess::ok;
    }
    if (index == 0) ownAddress = written;
    else if (index == 1) {
        // A mid-byte divider change needs bit-level phase qualification.
        if (inFlight && written != frequency) return Imx1I2cAccess::sideEffect;
        frequency = written;
    } else if (index == 2) {
        const uint8_t next = written & ~4u;
        if (!(next & 0x80)) {
            // IEN=0 resets the transfer engine; programming registers remain accessible.
            if (active() || inFlight) return Imx1I2cAccess::sideEffect;
            control = 0; status = 0x81; data = 0;
            awaitingAddress = addressed = readDirection = receivedNack = false;
            failure = Imx1I2cAccess::ok;
            return Imx1I2cAccess::ok;
        }
        // IEN must be set before the other controls have an effect (§29.6.3).
        if (!(control & 0x80)) {
            if (written != 0x80) return Imx1I2cAccess::sideEffect;
            control = 0x80;
            return Imx1I2cAccess::ok;
        }
        if (inFlight) return Imx1I2cAccess::sideEffect;
        const bool start = !(control & 0x20) && (next & 0x20);
        const bool stop = (control & 0x20) && !(next & 0x20);
        const bool repeat = written & 4;
        if ((start || repeat || stop) && !singleMaster) return Imx1I2cAccess::externalInput;
        if (repeat && !active()) {
            control = next & ~0x20;
            status |= 0x12;
            return Imx1I2cAccess::ok;
        }
        if ((start || repeat) && (!(next & 0x10) || stop)) return Imx1I2cAccess::sideEffect;
        if (start || repeat) {
            awaitingAddress = true; addressed = false; receivedNack = false;
            status |= 0x20;
            control = next;
            notify({repeat ? Imx1I2cPhase::repeatedStart : Imx1I2cPhase::start, 0, 0, false, false}, Imx1I2cResponse::unknown);
        } else if (stop) {
            status &= ~0x20;
            awaitingAddress = addressed = false;
            control = next;
            notify({Imx1I2cPhase::stop, pending.address, 0, readDirection, false}, Imx1I2cResponse::unknown);
        }
        control = next;
        status &= ~0x40;
    } else if (index == 3) {
        // IAL and IIF are W0C, all other implemented status bits are read only.
        status &= uint8_t(written | ~0x12u);
    } else {
        if (!(control & 0x80)) { data = written; return Imx1I2cAccess::ok; }
        if (!active() || !(control & 0x10) || inFlight) return Imx1I2cAccess::sideEffect;
        if (!awaitingAddress && (!addressed || readDirection || (status & 1))) return Imx1I2cAccess::sideEffect;
        data = written;
        if (awaitingAddress) {
            launch({Imx1I2cPhase::address, uint8_t(written >> 1), uint8_t(written), bool(written & 1), false});
            awaitingAddress = false;
        } else launch({Imx1I2cPhase::transmit, pending.address, uint8_t(written), false, false});
    }
    return Imx1I2cAccess::ok;
}
const char* imx1I2cPhaseName(Imx1I2cPhase phase) {
    switch (phase) {
        case Imx1I2cPhase::start: return "start";
        case Imx1I2cPhase::repeatedStart: return "repeated-start";
        case Imx1I2cPhase::address: return "address";
        case Imx1I2cPhase::transmit: return "transmit";
        case Imx1I2cPhase::receive: return "receive";
        case Imx1I2cPhase::stop: return "stop";
    }
    return "unknown";
}
const char* imx1I2cResponseName(Imx1I2cResponse response) {
    switch (response) {
        case Imx1I2cResponse::unknown: return "unknown";
        case Imx1I2cResponse::ack: return "ack";
        case Imx1I2cResponse::nack: return "nack";
        case Imx1I2cResponse::arbitrationLost: return "arbitration-lost";
    }
    return "unknown";
}
