#ifndef IMX1_I2C_READ_WINDOW_H
#define IMX1_I2C_READ_WINDOW_H

#include <array>

#include "imx1_i2c.h"
#include "machine_state_archive.h"

// Explicit diagnostic read window, without a physical part or capacity claim.
class Imx1I2cReadWindow : public Imx1I2cBus {
   public:
    using Image = std::array<uint8_t, 256>;
    explicit Imx1I2cReadWindow(const Image& image) : bytes(image) {}

    void signal(const Imx1I2cTransfer& transfer) override {
        if (transfer.phase == Imx1I2cPhase::start) state = State::writeAddress;
        else if (transfer.phase == Imx1I2cPhase::stop) state = State::idle;
        else if (transfer.phase == Imx1I2cPhase::repeatedStart)
            state = state == State::offsetSet ? State::readAddress : State::blocked;
    }

    Imx1I2cReply transfer(const Imx1I2cTransfer& transfer) override {
        if (transfer.address != 0x50) return unknown();
        if (transfer.phase == Imx1I2cPhase::address) {
            if (state == State::writeAddress && !transfer.read && transfer.byte == 0xa0) {
                state = State::offset;
                return {Imx1I2cResponse::ack, 0};
            }
            if (state == State::readAddress && transfer.read && transfer.byte == 0xa1) {
                state = State::reading;
                return {Imx1I2cResponse::ack, 0};
            }
        } else if (transfer.phase == Imx1I2cPhase::transmit && state == State::offset && !transfer.read) {
            cursor = transfer.byte;
            state = State::offsetSet;
            return {Imx1I2cResponse::ack, 0};
        } else if (transfer.phase == Imx1I2cPhase::receive && state == State::reading && transfer.read &&
                   cursor < bytes.size()) {
            const uint8_t value = bytes[cursor++];
            if (!transfer.acknowledge) state = State::ended;
            return {Imx1I2cResponse::ack, value};
        }
        return unknown();
    }

    // FNV-1a of the image, so a state made with other bytes is refused.
    uint32_t stateIdentity() const override {
        uint32_t hash = 2166136261u;
        for (const uint8_t byte : bytes) hash = (hash ^ byte) * 16777619u;
        return hash ? hash : 1;
    }
    void saveState(StateWriter& a) const override {
        uint32_t position = cursor;
        State protocol = state;
        a.Do32(position).DoEnum(protocol, 8);
    }
    bool loadState(StateReader& a, bool commit) override {
        uint32_t position = 0;
        State protocol = State::idle;
        // The cursor stops one past the last byte.
        a.Do32(position).DoEnum(protocol, 8).Require(position <= bytes.size(), "cursor");
        if (!a.complete()) return false;
        if (commit) { cursor = position; state = protocol; }
        return true;
    }

   private:
    enum class State { idle, writeAddress, offset, offsetSet, readAddress, reading, ended, blocked };
    const Image bytes;
    unsigned cursor = 0;
    State state = State::idle;
    Imx1I2cReply unknown() {
        state = State::blocked;
        return {};
    }
};

#endif
