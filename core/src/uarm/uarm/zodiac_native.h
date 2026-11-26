#ifndef FROND_ZODIAC_NATIVE_H
#define FROND_ZODIAC_NATIVE_H
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class SoC;

// Native guest calls on the Zodiac, like NativeBoundary on the T|X: from the UI task's event wait in
// EvtGetSysEvent, which both the native Tapwave launcher and 68k apps (through the ROM PACE) reach, straight to
// the Boot table's ROM routines. The machine is the i.MX1 one, so calls go through SocIMX1's pc stops.
namespace ZodiacNative {
// True on a Zodiac SoC.
bool Applies(SoC& soc);
// Runs to the boundary, at most 0.1 s of guest time, then with wake up to 1 s more after waking an idle UI task
// (SocIMX1::WakeIdle), or up to 0.9 s more without. No guest calls and no guest writes. Leaves the machine
// stopped there.
bool Prepare(SoC& soc, bool wake = true);

class Session {
 public:
    // Requires an active recovery scope (ReadOnlyGuest) and the machine on the boundary. Allocates one guest
    // scratch block, freed by Close.
    explicit Session(SoC& soc);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // A Boot table routine by its native trap (as on the T|X), arguments in r0-r3 and on the stack.
    uint32_t Call(uint16_t trap, std::array<uint32_t, 4> args = {}, std::vector<uint32_t> stackArgs = {});
    bool Read(uint32_t address, void* dest, uint32_t size);
    bool Write(uint32_t address, const void* src, uint32_t size);
    // A guest function pointer in the data block; the callback gets r0 to r15 and sets r0 to r3.
    void RegisterCallback(uint32_t address, std::function<void(uint32_t*)> callback);
    void UnregisterCallback(uint32_t address);
    uint32_t DataAddress() const;
    uint32_t DataSize() const;
    // Frees the scratch block; must succeed before committing. On any exception discard the whole checkpoint.
    void Close();

 private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}  // namespace ZodiacNative
#endif
