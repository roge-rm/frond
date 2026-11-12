#ifndef FROND_IMX1_CS5_BUS_H
#define FROND_IMX1_CS5_BUS_H

#include "imx1_eim.h"

// AN2422: generic DTACK captures a falling edge; CF/PCMCIA WAIT completes high.
class Imx1Cs5Bus {
   public:
    enum class State { waiting, complete, unknown, timeout };
    static constexpr uint32_t timeoutClocks = 1022, minimumClocks = 3;
    explicit Imx1Cs5Bus(Imx1Cs5Mode selected, bool initialHigh = true)
        : mode(selected), previousHigh(initialHigh) {}
    State sample(uint32_t elapsed, bool known, bool high) {
        if (state != State::waiting) return state;
        if (mode == Imx1Cs5Mode::unsupported || !known) return state = State::unknown;
        if (mode != Imx1Cs5Mode::fixed && elapsed > timeoutClocks && !acknowledged) return state = State::timeout;
        if (mode == Imx1Cs5Mode::dtack && previousHigh && !high) acknowledged = true;
        if (mode == Imx1Cs5Mode::wait && high) acknowledged = true;
        previousHigh = high;
        // Fixed WSC=62 is 63 HCLK periods. External cycles retain the 3T pulse minimum.
        if ((mode == Imx1Cs5Mode::fixed && elapsed >= 63) ||
            (acknowledged && elapsed >= minimumClocks)) return state = State::complete;
        if (elapsed >= timeoutClocks) return state = State::timeout;
        return state;
    }
    State status() const { return state; }
   private:
    Imx1Cs5Mode mode;
    bool previousHigh, acknowledged = false;
    State state = State::waiting;
};
#endif
