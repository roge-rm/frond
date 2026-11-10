// The Zodiac's view, pen, stick and EEPROM identity as the normal runtime works them out, its input
// holds and releases, and SocIMX1 on made-up ROMs: the buttons from reset, a pen held over a save, a
// stopped machine.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <string>

#include "soc_IMX1.h"
#include "zodiac_input.h"
#include "zodiac_panel.h"

namespace {
void rotation() {
    std::vector<uint32_t> panel(size_t(zodiacPanelWidth) * zodiacPanelHeight), view(size_t(zodiacViewWidth) * zodiacViewHeight, ~0u);
    for (uint32_t sy = 0; sy < zodiacPanelHeight; ++sy)
        for (uint32_t sx = 0; sx < zodiacPanelWidth; ++sx) panel[size_t(sy) * zodiacPanelWidth + sx] = sy << 16 | sx;
    zodiacRotateFrame(panel.data(), view.data());
    // A quarter turn clockwise: the panel's top left is the view's top right, its top row the right column.
    auto at = [&](uint32_t x, uint32_t y) { return view[size_t(y) * zodiacViewWidth + x]; };
    assert(at(479, 0) == 0 && at(479, 319) == 319 && at(0, 0) == (479u << 16) && at(0, 319) == (479u << 16 | 319));
    for (uint32_t y = 0; y < zodiacViewHeight; ++y)
        for (uint32_t x = 0; x < zodiacViewWidth; ++x) assert(at(x, y) == ((479 - x) << 16 | y));
}

void pen() {
    uint32_t x = 0, y = 0;
    // The corners of the virtual panel, and the tap that pressed Next at Welcome.
    zodiacPenRaw(0, 0, x, y); assert(x == 366 && y == 32);
    zodiacPenRaw(480, 320, x, y); assert(x == 36 && y == 382);
    zodiacPenRaw(440, 305, x, y); assert(x == 51 && y == 353);
    zodiacPenRaw(-50, 900, x, y); assert(x == 36 && y == 32);
    const auto down = zodiacPenInputs(true, 440, 305), up = zodiacPenInputs(false, 440, 305);
    assert(down.contactKnown && down.normalKnown && down.referenceKnown && down.contactDown && !up.contactDown);
    assert(down.normal[0] == 0x1100 && down.normal[1] == 0x1100 + (51 << 7) && down.normal[2] == 0x1100 + (353 << 7) &&
           down.normal[3] == 0x1234);
    assert(down.reference[0] == 0x0180 && down.reference[1] == 0x3280 && down.reference[2] == 0x7100);
    assert(up.normal == down.normal && up.reference == down.reference);
}

void stick() {
    assert(zodiacStickOffset(0) == 0 && zodiacStickOffset(1) == 511 && zodiacStickOffset(-1) == -511);
    assert(zodiacStickOffset(7) == 511 && zodiacStickOffset(-7) == -511 && zodiacStickOffset(0.5f) == 256);
    assert(zodiacStickOffset(0.0f / 0.0f) == 0);
}

void put32le(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (8 * i)); }
void put32be(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (24 - 8 * i)); }

// A made-up ROM with the layout the identity is read from: packed globals at 0x400 and their
// metadata at 0x300 past the big card, the data in literal blocks.
std::vector<uint8_t> rom() {
    constexpr size_t card = 0x20000;
    std::vector<uint8_t> rom(card + 0x1000);
    put32le(rom, card + 0x78, 0x400);
    put32le(rom, card + 0xac, 0x300);
    put32be(rom, card + 0x300 + 4, 0x9a24);
    put32be(rom, card + 0x400 + 4, 0u - 0x9a24);
    size_t at = card + 0x408, value = 0;
    for (size_t length : {100, 128, 128, 128, 92}) {
        rom[at++] = uint8_t(0x80 | (length - 1));
        for (size_t i = 0; i < length; ++i, ++value) rom[at++] = uint8_t(value * 7 + 3);
    }
    return rom;
}

void eeprom() {
    auto good = rom();
    std::array<uint8_t, 256> image{};
    assert(zodiacEepromIdentity(good.data(), good.size(), image));
    for (size_t i = 0; i < 256; ++i)
        assert(image[i] == (i >= 0x50 && i < 0xd0 ? uint8_t((0x1c0 + i - 0x50) * 7 + 3) : 0xff));
    // Anything else is refused and the image left alone.
    const auto kept = image;
    auto refused = [&](const std::vector<uint8_t>& bad) {
        return !zodiacEepromIdentity(bad.data(), bad.size(), image) && image == kept;
    };
    assert(!zodiacEepromIdentity(nullptr, 0, image));
    auto bad = good; bad.resize(0x20000 + 0x40); assert(refused(bad));
    bad = good; put32be(bad, 0x20000 + 0x304, 0x9a28); assert(refused(bad));
    bad = good; put32be(bad, 0x20000 + 0x404, 0x9a24); assert(refused(bad));
    bad = good; put32le(bad, 0x20000 + 0x78, 0x7fffffff); assert(refused(bad));
    bad = good; put32le(bad, 0x20000 + 0xac, 0xfffffff0); assert(refused(bad));
    // A block that isn't a literal, and literal blocks that run off the end.
    bad = good; bad[0x20000 + 0x408] = 0x20; assert(refused(bad));
    bad = good; bad.resize(0x20000 + 0x408 + 200); assert(refused(bad));
}

// A machine that takes what it is given unless told to refuse, and keeps count.
struct FakePort : ZodiacInputPort {
    bool refuse = false, penDown = false;
    std::array<bool, ZodiacInput::controls> down{};
    int releases = 0, lifts = 0, penX = 0, penY = 0, stickX = 99, stickY = 99;
    bool setControl(unsigned control, bool pressed) override { if (refuse) return false; down[control] = pressed; return true; }
    bool releaseControls() override { if (refuse) return false; down.fill(false); ++releases; return true; }
    bool setPen(bool pressed, int x, int y) override { if (refuse) return false; penDown = pressed; penX = x; penY = y; return true; }
    bool liftPen() override { if (refuse) return false; if (penDown) ++lifts; penDown = false; return true; }
    bool setStick(int x, int y) override { if (refuse) return false; stickX = x; stickY = y; return true; }
};

// The ROM's stick driver samples every 30 ms and drops a sample over 200 counts from the last one
// (20095de6). Whatever the host does and however often input is pumped, no sample is dropped.
void stickRamp() {
    constexpr uint64_t sample = ZodiacInput::ticksPerSecond * 30 / 1000;
    const int moves[][2] = {{511, 0}, {0, 0}, {-511, 511}, {511, -511}, {0, 300}, {37, -420}, {0, 0}};
    for (const uint64_t pumpTicks : {1u, 33u, 200u, 328u, 491u, 492u, 546u, 700u, 983u, 2000u})
        for (uint64_t phase = 0; phase < sample; phase += 97) {
            FakePort port;
            ZodiacInput input;
            input.pump(port);
            assert(port.stickX == 0 && port.stickY == 0 && input.stickSettled());
            int lastX = 0, lastY = 0, steps = 0;
            uint64_t nextSample = phase, started = 0;
            // The driver's samples up to now, or only the ones before it.
            const auto sampled = [&](bool now) {
                for (; nextSample + (now ? 0 : 1) <= input.time(); nextSample += sample) {
                    assert(std::abs(port.stickX - lastX) <= 200 && std::abs(port.stickY - lastY) <= 200);
                    lastX = port.stickX; lastY = port.stickY;
                }
            };
            for (const auto& move : moves) {
                input.stick(move[0], move[1]);
                started = input.time();
                while (!input.stickSettled()) {
                    sampled(false);
                    input.pump(port);
                    sampled(true);
                    assert(std::abs(port.stickX) <= 511 && std::abs(port.stickY) <= 511);
                    if (!input.stickSettled()) input.advance(pumpTicks);
                    assert(++steps < 100000);
                }
                assert(port.stickX == move[0] && port.stickY == move[1]);
                // Rest to full in six steps, 75 ms from the first, when input is pumped often enough.
                if (pumpTicks == 1 && &move == &moves[0])
                    assert(input.time() - started >= 5 * ZodiacInput::stickStepTicks &&
                           input.time() - started <= 6 * ZodiacInput::stickStepTicks);
                // The driver's next samples see the stick where it now is.
                input.advance(2 * sample);
                sampled(true);
                assert(lastX == move[0] && lastY == move[1]);
            }
        }
    // A machine that refuses the stick for now gets the same step later.
    FakePort port;
    ZodiacInput input;
    input.pump(port);
    input.stick(511, 0);
    port.refuse = true; input.advance(1000); input.pump(port); assert(port.stickX == 0);
    port.refuse = false; input.pump(port); assert(port.stickX == 100);
}

// Guest time the pen or key 3 stays down when the host holds it hostMsec, with input pumped every
// 12 ms of host time and the guest passing guestTicks in each.
uint64_t guestHold(bool pen, uint64_t hostMsec, uint64_t guestTicks) {
    FakePort port;
    ZodiacInput input;
    uint64_t host = 5000, downAt = 0;
    input.hostTime(host); input.pump(port);
    assert(pen ? input.pen(port, true, 10, 20) : input.key(port, 3, true));
    downAt = input.time();
    for (;;) {
        host += 12; input.advance(guestTicks); input.hostTime(host); input.pump(port);
        if (host - 5000 < hostMsec) { assert(pen ? port.penDown : port.down[3]); continue; }
        if (pen ? input.pen(port, false, 0, 0) : input.key(port, 3, false)) break;
        assert(input.time() - downAt < 10 * ZodiacInput::ticksPerSecond);
    }
    assert(pen ? !port.penDown : !port.down[3]);
    return input.time() - downAt;
}

// A tap lasts as long in guest time as the host held it, however slowly the guest runs. The ROM needs
// the pen down for 108 ms of its own time when a tap lands in its battery poll.
void guestTimeHolds() {
    constexpr uint64_t second = ZodiacInput::ticksPerSecond, needed = second * 108 / 1000;
    // Guest speeds from a two hundredth of real time to real time (393 ticks in 12 ms).
    for (const uint64_t ticks : {2u, 7u, 40u, 131u, 393u}) {
        const uint64_t slack = ticks + second * 12 / 1000;
        // A 100 ms tap is held 125 ms of guest time, enough for the ROM.
        uint64_t held = guestHold(true, 100, ticks);
        assert(held >= ZodiacInput::minimumPenHoldTicks && held >= needed && held < ZodiacInput::minimumPenHoldTicks + slack);
        // A 300 ms tap is held 300 ms of guest time.
        held = guestHold(true, 300, ticks);
        assert(held + slack >= second * 300 / 1000 && held < second * 300 / 1000 + slack);
        // A long press is owed half a second, and ends with the host's when the guest keeps up.
        held = guestHold(true, 2000, ticks);
        assert(held >= ZodiacInput::longestOwedHoldTicks);
        if (ticks < 98) assert(held < ZodiacInput::longestOwedHoldTicks + slack);
        // Keys are held the same way, 100 ms at least.
        held = guestHold(false, 30, ticks);
        assert(held >= ZodiacInput::minimumHoldTicks && held < ZodiacInput::minimumHoldTicks + slack);
        held = guestHold(false, 300, ticks);
        assert(held + slack >= second * 300 / 1000 && held < second * 300 / 1000 + slack);
    }
}

void holds() {
    constexpr uint64_t hold = ZodiacInput::minimumHoldTicks;
    FakePort port;
    ZodiacInput input;
    // A new machine's controls are released once before any key is taken, and the stick is set.
    assert(!input.key(port, 3, true) && !input.settled());
    port.refuse = true; input.pump(port); assert(!input.settled() && port.releases == 0);
    port.refuse = false; input.pump(port);
    assert(input.settled() && port.releases == 1 && port.stickX == 0 && port.stickY == 0);
    // A quick click: the release waits for the hold, and a second key waits behind it.
    assert(input.key(port, 3, true) && port.down[3]);
    assert(!input.key(port, 3, false) && port.down[3]);
    input.advance(hold - 1); assert(!input.key(port, 3, false));
    // The clock goes on while the guest waits, as the owner adds the time it asked for, so the
    // release arrives and the next key follows.
    input.advance(1); assert(input.key(port, 3, false) && !port.down[3]);
    assert(input.key(port, 4, true) && port.down[4]);
    // A repeated down or a stray up is taken and changes nothing.
    assert(input.key(port, 4, true) && input.key(port, 5, false) && !port.down[5]);
    // A release the machine refuses stays queued.
    input.advance(hold); port.refuse = true; assert(!input.key(port, 4, false));
    port.refuse = false; assert(input.key(port, 4, false) && !port.down[4]);
    // A jam is let go when its time is up, and the key's own events are swallowed meanwhile.
    assert(input.jam(port, 7, 1000) && port.down[7] && !input.jam(port, 8, 10));
    assert(input.key(port, 7, false) && port.down[7]);
    input.advance(ZodiacInput::ticksPerSecond - 1); input.pump(port); assert(port.down[7]);
    input.advance(1); input.pump(port); assert(!port.down[7]);
    // The pen: a tap is held, moves go through, and the up is sent where the pen last was.
    assert(input.pen(port, false, 0, 0) && !port.penDown);
    assert(input.pen(port, true, 10, 20) && port.penDown && !input.pen(port, false, 0, 0));
    assert(input.pen(port, true, 30, 40) && port.penX == 30);
    input.advance(ZodiacInput::minimumPenHoldTicks - 1); assert(!input.pen(port, false, 0, 0));
    input.advance(1); assert(input.pen(port, false, 0, 0) && !port.penDown && port.penX == 30 && port.penY == 40);
    // The stick moves a step at a time towards where the host has it.
    input.stick(100, -200); input.pump(port); assert(port.stickX == 100 && port.stickY == -100 && !input.stickSettled());
    input.pump(port); assert(port.stickY == -100);
    input.advance(ZodiacInput::stickStepTicks - 1); input.pump(port); assert(port.stickY == -100);
    input.advance(1); input.pump(port); assert(port.stickX == 100 && port.stickY == -200 && input.stickSettled());
    // A load with a key and the pen down in the machine: both are let go there, the host's keys
    // count as up, and input waits until that is done.
    assert(input.key(port, 2, true) && input.pen(port, true, 5, 6));
    input.machineChanged();
    assert(!input.settled() && !input.pen(port, true, 1, 1) && !input.key(port, 2, true));
    port.refuse = true; input.pump(port); assert(port.down[2] && port.penDown);
    port.refuse = false; input.pump(port);
    assert(input.settled() && !port.down[2] && !port.penDown && port.releases == 2 && port.lifts == 1);
    assert(port.stickX == 100 && port.stickY == -200);
    assert(input.key(port, 2, false) && input.pen(port, false, 0, 0));
    assert(input.key(port, 2, true) && port.down[2]);
}

// The made-up ROM with one instruction at the reset entry.
std::vector<uint8_t> romWith(uint32_t instruction) {
    auto image = rom();
    put32le(image, 0, instruction);
    return image;
}

// Runs the machine for a moment of guest time, as the main loop does.
void run(SocIMX1& soc, int slices = 4) { for (int i = 0; i < slices; ++i) soc.Run(1000000, 100000000); }

void buttonsFromReset() {
    const auto o = zodiacMachineOptions(nullptr);
    // Every button pin reads high, released, from reset, and nothing is scheduled to change them.
    assert(o.gpioAInputMaskedKnown && (o.gpioAInput & 0x780) == 0x780 && o.gpioDInputKnown && (o.gpioDInput & 0x7780) == 0x7780);
    assert(!o.gpioEventAfterInstructions);
    for (unsigned port = 0; port < 4; ++port) assert(!o.gpioEventMasks[port] && !o.gpioEventValues[port]);
    for (const auto& pin : imx1ZodiacControlPins) assert((pin.port == 0 ? o.gpioAInput : o.gpioDInput) >> pin.pin & 1);
}

void machine() {
    std::string error;
    // A process has one machine, so the stop case is a second run of this test. Its ROM starts with
    // an undefined instruction, the other one loops.
    const bool stop = std::getenv("ZODIAC_TEST_STOP") != nullptr;
    const auto image = romWith(stop ? 0xe7f000f0 : 0xeafffffe);
    SocIMX1* soc = SocIMX1::Create(image.data(), image.size(), error);
    assert(soc && error.empty());
    if (stop) {
        // A stopped machine says why in one line and is not saved.
        run(*soc);
        assert(soc->Halted() && soc->StopText().find("unsupported-instruction") == 0);
        assert(!soc->Save() && soc->GetSavestate().size == 0);
        std::puts("PASS: a stopped Zodiac machine gives its reason and refuses the save");
        return;
    }
    assert(!soc->Halted() && soc->StopText().empty());
    // Input is live from the first instruction, and a session saved with the pen down loads with
    // the pen lifted in the machine.
    soc->PenDown(100, 100);
    run(*soc);
    assert(soc->PenContact() && soc->Status().retired > 0 && !soc->Halted());
    assert(soc->Save());
    const auto saved = soc->GetSavestate();
    std::vector<uint8_t> state(static_cast<const uint8_t*>(saved.data), static_cast<const uint8_t*>(saved.data) + saved.size);
    assert(soc->Load(state.size(), state.data()));
    run(*soc, 1);
    assert(!soc->PenContact());
    // The host's own pen-up after the load changes nothing, and the next tap works.
    soc->PenUp(); run(*soc, 1); assert(!soc->PenContact());
    soc->PenDown(50, 60); run(*soc, 1); assert(soc->PenContact());
    // Reset starts the machine over with input live again.
    soc->Reset();
    assert(soc->Status().retired == 0 && !soc->Halted());
    run(*soc, 1);
    assert(!soc->PenContact());
    soc->PenDown(50, 60); run(*soc, 1); assert(soc->PenContact() && soc->Status().retired > 0);
    // A redraw asked for after a resize is taken once and can be asked for again.
    uint32_t first = 0, last = 0;
    soc->GetPendingFrame(first, last);
    assert(!soc->RedrawPending());
    for (int i = 0; i < 2; ++i) {
        soc->SetFramebufferDirty();
        assert(soc->RedrawPending());
        soc->GetPendingFrame(first, last);
        assert(!soc->RedrawPending());
    }
    std::puts("PASS: Zodiac input from the first instruction, the pen lifted after a load, Reset and the redraw flag");
}
}  // namespace

int main() {
    rotation();
    pen();
    stick();
    stickRamp();
    eeprom();
    holds();
    guestTimeHolds();
    buttonsFromReset();
    std::puts("PASS: the Zodiac's view turn, pen samples, stick offsets, EEPROM identity, input holds and released buttons");
    machine();
    return 0;
}
