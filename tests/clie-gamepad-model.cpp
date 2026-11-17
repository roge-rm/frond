// CLIÉ game controller model without a SoC: plug-in phases, the ID and its reply, press and
// release bytes at 9600 baud, Hold, the idle drop and the saved state.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <vector>
#include "ClieGamepad.h"

static constexpr uint64_t ms = 1000000;

// Bytes the pad sends from now until it has nothing, reading as fast as it lets.
static std::vector<uint8_t> drain(ClieGamepad& pad, uint64_t& now) {
    std::vector<uint8_t> out;
    for (int i = 0; i < 1000 && pad.Pending(); ++i, now += 100000) {
        const auto v = pad.Read(now);
        if (v != 0x100) out.push_back(uint8_t(v));
    }
    return out;
}

static void identify(ClieGamepad& pad, uint64_t& now) {
    assert(pad.LineLow());
    now += ClieGamepad::pullNs;
    pad.Tick(now, true);
    assert(!pad.LineLow() && pad.GetPhase() == ClieGamepad::Phase::identifying);
    pad.Tick(now, true);
    const auto id = drain(pad, now);
    assert(id.size() == 8 && id[0] == 0x55 && id[1] == 0x01 && id[2] == 0x04);
    for (uint8_t b : {0xAA, 0x01, 0x04, 0, 0, 0, 0, 0}) pad.Write(b, now);
    now += ClieGamepad::byteNs;
}

int main() {
    uint64_t now = 1000 * ms;
    ClieGamepad pad;
    assert(!pad.Key(keyIdPadA, true, now));  // detached: nothing
    pad.Attach(now, true);
    identify(pad, now);
    assert(pad.GetPhase() == ClieGamepad::Phase::identified && pad.Replies() == 1);

    // Press and release: 0x80 | code, then code, one per 10 bit times.
    assert(pad.Key(keyIdPadA, true, now));
    const uint64_t start = now;
    assert(pad.Read(now) == 0xDF);
    assert(pad.Read(now + 1000) == 0x100);
    assert(pad.Key(keyIdPadA, false, now));
    assert(pad.Read(start + ClieGamepad::byteNs - 1) == 0x100);
    now = start + ClieGamepad::byteNs;
    assert(pad.Read(now) == 0x5F);
    for (KeyId k : {keyIdPadUp, keyIdPadDown, keyIdPadLeft, keyIdPadRight, keyIdPadB, keyIdPadX, keyIdPadY, keyIdPadHome, keyIdPadMenu})
        pad.Key(k, true, now), pad.Key(k, false, now);
    const auto codes = drain(pad, now);
    const std::vector<uint8_t> want = {0xFD, 0x7D, 0xFE, 0x7E, 0xE0, 0x60, 0xFB, 0x7B, 0xFC, 0x7C, 0xDE, 0x5E, 0xDD, 0x5D, 0xDB, 0x5B, 0xDC, 0x5C};
    assert(codes == want);

    // Hold on: a held key's release goes out, then nothing.
    pad.Key(keyIdPadUp, true, now);
    pad.Key(keyIdPadHold, true, now);
    pad.Key(keyIdPadHold, false, now);
    assert(pad.HoldOn());
    assert((drain(pad, now) == std::vector<uint8_t>{0xFD, 0x7D}));
    assert(!pad.Key(keyIdPadA, true, now) && !pad.Key(keyIdPadA, false, now) && !pad.Pending());
    pad.Key(keyIdPadUp, false, now);
    pad.Key(keyIdPadHold, true, now);
    assert(!pad.HoldOn());

    // 25 s with no byte: forgotten; the next press pulls the line and goes out after the ID.
    now += ClieGamepad::idleDropNs;
    pad.Tick(now, true);
    assert(pad.GetPhase() == ClieGamepad::Phase::idle && !pad.LineLow());
    pad.Key(keyIdPadB, true, now);
    assert(!pad.Pending());
    identify(pad, now);
    assert((drain(pad, now) == std::vector<uint8_t>{0xFC}));

    // The port taken by IrDA: a tap waits, then the pad identifies again and sends it.
    pad.Key(keyIdPadB, false, now);
    drain(pad, now);
    now += ClieGamepad::byteNs;
    pad.Key(keyIdPadUp, true, now);
    pad.Tick(now, false);
    pad.Key(keyIdPadUp, false, now + 150 * ms);
    pad.Key(keyIdPadB, true, now + 150 * ms);
    now += ClieGamepad::portWaitNs;
    pad.Tick(now, false);
    assert(pad.LineLow());
    identify(pad, now);
    assert((drain(pad, now) == std::vector<uint8_t>{0xFD, 0x7D, 0xFC}));

    // Save and load while attached.
    {
        Savestate<ChunkType> state;
        assert(state.Save(pad));
        SavestateLoader<ChunkType> loader;
        ClieGamepad copy;
        assert(loader.Load(state.GetBuffer(), state.GetSize(), copy));
        assert(copy.Attached() && copy.GetPhase() == ClieGamepad::Phase::identified && copy.Replies() == pad.Replies());
        assert(copy.Key(keyIdPadB, false, now) && copy.Read(now + ClieGamepad::byteNs) == 0x7C);
    }

    // Unanswered ID on an open port: the driver kept it, so the pad sends.
    pad.Detach();
    pad.Attach(now, true);
    now += ClieGamepad::pullNs;
    pad.Tick(now, true);
    pad.Tick(now, true);
    drain(pad, now);
    now += ClieGamepad::idGiveUpNs;
    pad.Tick(now, true);
    assert(pad.GetPhase() == ClieGamepad::Phase::identified && pad.Replies() == 0);

    // Unanswered ID on a closed port gives up; a closed port holds the ID back.
    pad.Detach();
    pad.Attach(now, true);
    now += ClieGamepad::pullNs;
    pad.Tick(now, false);
    pad.Tick(now, false);
    assert(!pad.Pending());
    now += ClieGamepad::idGiveUpNs;
    pad.Tick(now, false);
    assert(pad.GetPhase() == ClieGamepad::Phase::idle);

    // Detached saves nothing.
    pad.Detach();
    {
        Savestate<ChunkType> state;
        assert(state.Save(pad));
        SavestateLoader<ChunkType> loader;
        ClieGamepad copy;
        copy.Attach(now, false);
        assert(loader.Load(state.GetBuffer(), state.GetSize(), copy));
        assert(!copy.Attached());
    }
    std::puts("clie gamepad model: ok");
    return 0;
}
