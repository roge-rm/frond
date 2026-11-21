#ifndef FROND_ZODIAC_INPUT_H
#define FROND_ZODIAC_INPUT_H

#include <array>
#include <cstdint>

#include "keys.h"

// Host input on its way to the Zodiac's machine: buttons as pin levels, the pen and the stick.
// It keeps what the host holds, gives a key or the pen its shortest time down and sends the
// releases a load or a reset owes. The machine is reached through the port.

struct ZodiacInputPort {
    virtual ~ZodiacInputPort() = default;
    // Each returns false when the machine refuses the change for now. It is offered again.
    virtual bool setControl(unsigned control, bool down) = 0;
    virtual bool releaseControls() = 0;
    virtual bool setPen(bool down, int x, int y) = 0;
    // Lifts the pen where the machine has it, when the machine has it down.
    virtual bool liftPen() = 0;
    virtual bool setStick(int x, int y) = 0;
};

// The machine's control for a shared key id, -1 for a key the Zodiac doesn't have. The numbers are
// Imx1ZodiacControl's, in the order of its pin table: Home A7, the triggers A8 and A9, power A10,
// stick click D7, Action right D9, up D10, left D14, Function D12, Bluetooth D13, Action down D8.
constexpr int zodiacControlFor(KeyId key) {
    switch (key) {
        case keyIdHome: return 0;
        case keyIdTriggerLeft: return 1;
        case keyIdTriggerRight: return 2;
        case keyIdPower: return 3;
        case keyIdStickPress: return 4;
        case keyIdHard2: return 5;
        case keyIdHard4: return 6;
        case keyIdHard3: return 7;
        case keyIdFunction: return 8;
        case keyIdBluetooth: return 9;
        case keyIdHard1: return 10;
        default: return -1;
    }
}

class ZodiacInput {
   public:
    static constexpr unsigned controls = 11;
    static constexpr uint64_t ticksPerSecond = 32768;
    // A key or the pen stays down at least this long, so a quick click is still seen.
    static constexpr uint64_t minimumHoldTicks = ticksPerSecond / 10;
    // POLICY, host input only: the pen stays down 125 ms at least. A pen that lands inside the ROM's
    // battery poll (20088588, every 5 s or so) has its routine (20096d58) start 99 ms late, and a tap
    // there is seen from 108 ms of guest time on, measured on the ROM.
    static constexpr uint64_t minimumPenHoldTicks = ticksPerSecond / 8;
    // POLICY, host input only: a key or the pen stays down for as much guest time as the host held it,
    // up to this much, so a tap is as long for a guest that runs slower than the host's clock.
    static constexpr uint64_t longestOwedHoldTicks = ticksPerSecond / 2;
    // The stick goes straight to where the host has it. The ROM's stick driver (20095de6) samples every
    // 30 ms, drops a sample more than 200 counts from the raw one before (and keeps that raw sample to
    // compare the next with), then averages the last three it took with weights 27, 46 and 27. So a jump
    // from rest to full loses one sample and the driver reports full travel on the fourth, which a stick
    // stepped 200 counts a sample at a time only reaches on the fifth. A real thumb that fast does the same.

    // After a load or a reset the host's keys and pen are up, whatever the machine had held.
    void machineChanged() {
        releasePending = liftPending = true;
        pressed.fill(false);
        jammed = pendingJam = -1;
        penIsDown = stickSet = false;
    }

    // The clock the holds are timed on, in CLK32 ticks. The owner adds guest time while the guest
    // runs and the time it was asked to run while the guest waits, so it never stands still.
    void advance(uint64_t ticks) { now += ticks; }
    uint64_t time() const { return now; }
    // The host's own clock in milliseconds, set before events are handed over. It only measures how
    // long the host held a key or the pen.
    void hostTime(uint64_t msec) { hostNow = msec; }

    void stick(int x, int y) { stickX = x; stickY = y; }

    // Presses a control for a set time. False when it can't be done just now.
    bool jam(ZodiacInputPort& port, unsigned control, uint32_t durationMsec) {
        if (control >= controls || releasePending || jammed >= 0 || !port.setControl(control, true)) return false;
        jammed = int(control);
        jamUntil = now + uint64_t(durationMsec) * ticksPerSecond / 1000;
        return true;
    }

    // A jam that starts once the releases a load or reset owes are sent, so it holds from the reset on.
    void jamAfterRelease(unsigned control, uint32_t durationMsec) {
        pendingJam = control < controls ? int(control) : -1;
        pendingJamMsec = durationMsec;
    }
    bool jamPending() const { return pendingJam >= 0; }

    // Sends what is due: the releases after a load or reset, a jam after them, the end of a jam and the stick.
    void pump(ZodiacInputPort& port) {
        if (releasePending && port.releaseControls()) releasePending = false;
        if (pendingJam >= 0 && !releasePending && jam(port, unsigned(pendingJam), pendingJamMsec)) pendingJam = -1;
        if (liftPending && port.liftPen()) liftPending = false;
        if (jammed >= 0 && now >= jamUntil && port.setControl(unsigned(jammed), false)) jammed = -1;
        pumpStick(port);
    }

    // A key event from the host's queue. False leaves it there for the next pump.
    bool key(ZodiacInputPort& port, unsigned control, bool down) {
        if (control >= controls || int(control) == jammed) return true;
        if (releasePending) return false;
        if (down) {
            if (pressed[control]) return true;
            if (!port.setControl(control, true)) return false;
            pressed[control] = true;
            pressedAt[control] = now;
            pressedHostAt[control] = hostNow;
            releaseOwed[control] = 0;
            return true;
        }
        if (!pressed[control]) return true;
        if (!releaseOwed[control]) releaseOwed[control] = owedHold(pressedHostAt[control], minimumHoldTicks);
        if (now - pressedAt[control] < releaseOwed[control] || !port.setControl(control, false)) return false;
        pressed[control] = false;
        return true;
    }

    // A pen event from the host's queue. False leaves it there for the next pump.
    bool pen(ZodiacInputPort& port, bool down, int x, int y) {
        if (liftPending) return false;
        if (!down) {
            if (!penIsDown) return true;
            if (!penOwed) penOwed = owedHold(penDownHostAt, minimumPenHoldTicks);
            if (now - penDownAt < penOwed || !port.setPen(false, penX, penY)) return false;
            penIsDown = false;
            return true;
        }
        if (!port.setPen(true, x, y)) return false;
        if (!penIsDown) {
            penDownAt = now;
            penDownHostAt = hostNow;
        }
        penOwed = 0;
        penIsDown = true;
        penX = x;
        penY = y;
        return true;
    }

    bool settled() const { return !releasePending && !liftPending; }
    // True once the machine's stick is where the host's is.
    bool stickSettled() const { return stickSet && stickX == stickSetX && stickY == stickSetY; }

   private:
    // Guest time a release waits for: what the host held since downHostAt, within the two limits.
    uint64_t owedHold(uint64_t downHostAt, uint64_t least) const {
        const uint64_t held = (hostNow - downHostAt) * ticksPerSecond / 1000;
        return held < least ? least : held > longestOwedHoldTicks ? longestOwedHoldTicks : held;
    }
    // Puts the machine's stick where the host's is.
    void pumpStick(ZodiacInputPort& port) {
        if (stickSettled() || !port.setStick(stickX, stickY)) return;
        stickSet = true;
        stickSetX = stickX;
        stickSetY = stickY;
    }
    uint64_t now{0}, hostNow{0};
    bool releasePending{true}, liftPending{true};
    std::array<bool, controls> pressed{};
    std::array<uint64_t, controls> pressedAt{}, pressedHostAt{}, releaseOwed{};
    int jammed{-1}, pendingJam{-1};
    uint64_t jamUntil{0};
    uint32_t pendingJamMsec{0};
    bool penIsDown{false};
    uint64_t penDownAt{0}, penDownHostAt{0}, penOwed{0};
    int penX{0}, penY{0};
    int stickX{0}, stickY{0}, stickSetX{0}, stickSetY{0};
    bool stickSet{false};
};

#endif
