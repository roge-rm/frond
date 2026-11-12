#ifndef FROND_NATIVE_BOUNDARY_H
#define FROND_NATIVE_BOUNDARY_H
#ifndef __EMSCRIPTEN__
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>
class SoC;
struct ArmCpu;
namespace NativeBoundary {
// No guest calls made by the host and no host writes to guest memory. Advances at most 100M normal
// cycles; with wake, up to 50M more after waking the UI task with WakeIdle if it doesn't reach its
// event wait by itself. Host calls pass wake, readiness checks don't.
// A successful return is a candidate, revalidated by Session after checkpoint.
bool Prepare(SoC& soc, bool wake = true);
// For a host call that is waiting: on the T|X and Zire 72 runs at most 50M normal cycles to the
// kernel's idle loop and has it call EvtWakeup once, as an interrupt routine would, so a UI task
// blocked in EvtGetEvent returns a nilEvent and reaches its safe point again. The guest goes on from
// the call; false if the firmware or the stop doesn't qualify, or the guest never idles.
bool WakeIdle(SoC& soc);
std::string InspectUiApp(SoC& soc);
class Session {
 public:
    // Requires active syscallDispatchBeginRecovery and a whole-guest checkpoint.
    // Revalidates boundary before allocating one owned guest scratch block.
    explicit Session(SoC& soc);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    uint32_t Call(uint16_t trap, std::array<uint32_t,4> args = {},
                  std::vector<uint32_t> stackArgs = {});
    bool Read(uint32_t address, void* dest, uint32_t size);
    bool Write(uint32_t address, const void* src, uint32_t size);
    void RegisterCallback(uint32_t address, std::function<void(ArmCpu*)> callback);
    void UnregisterCallback(uint32_t address);
    uint32_t DataAddress() const;
    uint32_t DataSize() const;
    // Must succeed before committing writes; destructor only restores host/CPU state.
    // On any exception discard the entire checkpoint (including allocation).
    void Close();
 private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif
#endif
