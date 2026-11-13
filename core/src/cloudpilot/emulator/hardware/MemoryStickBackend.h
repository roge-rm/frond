#ifndef _MEMORY_STICK_BACKEND_H_
#define _MEMORY_STICK_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "CardImage.h"

// A stick behind the MB86189 that works on whole transfer protocol commands and keeps the stick's
// flash pages itself, so the guest's own driver owns the block map. Frond's runner supplies one
// (the TG50's stick model); without it the MB86189 uses Cloudpilot's logical MemoryStick.
class MemoryStickBackend {
   public:
    // Bytes of state a save holds.
    static constexpr size_t STATE_BYTES = 32 + 512 + 6 * 4;

    virtual ~MemoryStickBackend() = default;

    virtual bool SupportsImage(size_t blocksTotal) const = 0;
    // The image's bytes are the stick's stored pages. Mount starts the stick afresh, Remount keeps
    // its transfer state.
    virtual bool Mount(CardImage& image) = 0;
    virtual bool Remount(CardImage& image) = 0;
    virtual void Unmount() = 0;

    virtual bool Present() const = 0;
    virtual void Reset() = 0;
    virtual bool TpcRead(uint8_t tpc, uint8_t* out, size_t size) = 0;
    virtual bool TpcWrite(uint8_t tpc, const uint8_t* in, size_t size) = 0;
    virtual void FinishRead(uint8_t tpc) = 0;
    // The stick's interrupt register.
    virtual uint8_t Interrupt() const = 0;
    virtual void SetInterrupt(uint8_t value) = 0;

    virtual void SaveState(uint8_t* out) const = 0;
    virtual bool LoadState(const uint8_t* in) = 0;

    // Set before the core starts; EmDevice hands one to each MB86189.
    static std::function<std::unique_ptr<MemoryStickBackend>()> factory;
};

#endif
