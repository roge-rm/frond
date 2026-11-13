#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
#include "EmRegsMB86189.h"
#include "ExternalStorage.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"

// A stick that only records what the chip asks of it.
struct Fake : MemoryStickBackend {
    static inline Fake* last = nullptr;
    int mounts = 0, resets = 0, unmounts = 0;
    bool present = false;
    uint8_t interrupt = 0, fill = 0;
    Fake() { last = this; }
    bool SupportsImage(size_t blocks) const override { return blocks == 8192; }
    bool Mount(CardImage&) override { ++mounts; present = true; return true; }
    bool Remount(CardImage&) override { present = true; return true; }
    void Unmount() override { ++unmounts; present = false; }
    bool Present() const override { return present; }
    void Reset() override { ++resets; }
    bool TpcRead(uint8_t, uint8_t* out, size_t size) override { memset(out, fill, size); return true; }
    bool TpcWrite(uint8_t, const uint8_t*, size_t) override { return true; }
    void FinishRead(uint8_t) override {}
    uint8_t Interrupt() const override { return interrupt; }
    void SetInterrupt(uint8_t v) override { interrupt = v; }
    void SaveState(uint8_t* out) const override { memset(out, fill, STATE_BYTES); out[0] = interrupt; }
    bool LoadState(const uint8_t* in) override {
        if (in[1] != in[STATE_BYTES - 1]) return false;
        interrupt = in[0]; fill = in[1]; return true;
    }
};

static std::vector<uint8> Save(EmRegsMB86189& mb) {
    Savestate<ChunkType> s; assert(s.Save(mb));
    auto p = static_cast<uint8*>(s.GetBuffer()); return {p, p + s.GetSize()};
}
static bool Load(EmRegsMB86189& mb, std::vector<uint8> bytes) {
    SavestateLoader<ChunkType> s; return s.Load(bytes.data(), bytes.size(), mb);
}

int main() {
    auto* data = new uint8[8192 * 512]();
    auto image = std::make_shared<CardImage>(data, 8192, false, CardImage::Protocol::mmc);

    MemoryStickBackend::factory = [] { return std::make_unique<Fake>(); };
    {
        EmRegsMB86189 mb(0x10800000);
        auto* stick = Fake::last;
        assert(mb.SupportsImageInSlot(EmHAL::Slot::memorystick, 8192));
        assert(!mb.SupportsImageInSlot(EmHAL::Slot::memorystick, 8000));
        assert(!mb.SupportsImageInSlot(EmHAL::Slot::sdcard, 8192));
        mb.Mount(EmHAL::Slot::memorystick, *image);
        assert(stick->mounts == 1 && stick->present);
        mb.Mount(EmHAL::Slot::sdcard, *image);
        assert(stick->mounts == 1);
        stick->interrupt = 0x80; stick->fill = 0x5a;
        auto saved = Save(mb);

        EmRegsMB86189 other(0x10800000);
        auto* second = Fake::last;
        assert(second != stick && Load(other, saved));
        assert(second->interrupt == 0x80 && second->fill == 0x5a);
        // A damaged stick blob is refused.
        auto bad = saved;
        bad[bad.size() - 1] ^= 0xff;
        EmRegsMB86189 third(0x10800000);
        auto* damaged = Fake::last;
        (void) Load(third, bad);
        assert(damaged->interrupt == 0 && damaged->fill == 0);
        mb.Unmount(EmHAL::Slot::memorystick);
        assert(stick->unmounts == 1 && !stick->present);
    }
    // A save made without a stick backend loads into a chip that has one, and the stick resets.
    MemoryStickBackend::factory = nullptr;
    EmRegsMB86189 plain(0x10800000);
    auto old = Save(plain);
    MemoryStickBackend::factory = [] { return std::make_unique<Fake>(); };
    EmRegsMB86189 with(0x10800000);
    auto* stick = Fake::last;
    assert(Load(with, old) && stick->resets >= 1);
    return 0;
}
