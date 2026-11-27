#include "zodiac_native.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "Defer.h"
#include "SoC.h"
#include "device_profile5.h"
#include "soc_IMX1.h"
#include "syscall_dispatch.h"

namespace ZodiacNative {
namespace {
// Facts of the 1.1 ROM. The boundary is EvtGetSysEvent (Thumb, 0x2005cd1c) just after its event group wait
// returns (bl at 0x2005cea6, then the wait flag at r6+5 cleared), on the cmp of the wait's result, the same
// place as the T|X's. It's reached from the Tapwave ROM's EvtGetEvent patch (ARM, 0x203cc728), whose return
// address is in the frame's saved lr. r9 is the UI task's, with the Boot and DAL tables just below it.
constexpr uint32_t site = 0x2005ceae, function = 0x2005cd1c, waitCall = 0x2005cea6, patch = 0x203cc728;
constexpr uint32_t patchReturn = 0x203cc754, bootTable = 0x1000a93c, dalTable = 0x10009a50, eventOffset = 0x6a8;
constexpr uint16_t functionCode[] = {0xb5f3, 0xb08f, 0x1c05, 0x4649, 0x6809, 0x6849, 0x482a, 0x1840};
constexpr uint16_t waitCode[] = {0xf018, 0xf953, 0x2100, 0x7171, 0x2800, 0xd020};
constexpr uint32_t patchCode[] = {0xe92d40f8, 0xe1a07001, 0xe1a06000, 0xeb0003a7, 0xe2804d40, 0xe1a05000,
                                  0xe1a00006, 0xe5942048, 0xe1a01007, 0xe1a0e00f, 0xe12fff12};
// The frame: push {r0, r1, r4-r7, lr} then sub sp, #0x3c; the event globals pointer at sp+0x38.
constexpr uint32_t frameSize = 0x58, savedR0 = 0x3c, savedLr = 0x54, globalsSlot = 0x38;
constexpr uint32_t allocationSize = 32 * 1024, dataSize = 8192, marker = 0xa55a3cc3;
// Guest time for one call, in 100 MHz cycles: a call runs on while its callbacks do.
constexpr uint64_t hz = 100000000, callSlice = 10 * hz, prepareBudget = hz;

// Boot table routines (native trap numbers as on the T|X) with their ROM entry and its first words: ARM glue
// into the Thumb routine, or the routine itself.
struct Entry {
    uint16_t trap;
    uint32_t address, code[4];
};
constexpr Entry entries[] = {
    {0x812c, 0x2006ffd0, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffcbf25}},  // DmDatabaseInfo
    {0x8148, 0x20070040, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffcb07b}},  // DmDeleteDatabase
    {0x815c, 0x20070090, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffcaa17}},  // DmFindDatabase
    {0x81d4, 0x20070270, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffcbd85}},  // DmNumDatabases
    {0x8188, 0x20070140, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffcbe59}},  // DmGetDatabase
    {0x8304, 0x20070730, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffd8a71}},  // ExgDBWrite
    {0x82fc, 0x20070710, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffd842b}},  // ExgDBRead
    {0x88f8, 0x20071f00, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffeda4d}},  // SysUIAppSwitch
    {0x8584, 0x20071130, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffe110d}},  // MemPtrNew
    {0x84b8, 0x20070e00, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffe0fdd}},  // MemChunkFree
    {0x8800, 0x20071b20, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffee0c9}},  // SysCurAppDatabase
    {0x8198, 0x20070180, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffccfa3}},  // DmGetNextDatabaseByTypeCreator
    {0x87c8, 0x2020a694, {0xe92d4ff0, 0xe5999000, 0xe24dd0d4, 0xe599900c}},  // SysAppLaunch
    {0x82e0, 0x200706a0, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffec851}},  // EvtSysEventAvail
    {0x881c, 0x20071b90, {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffd6851}},  // SysEventAvail
};

void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

SocIMX1* zodiac(SoC& soc) {
    return deviceTypeIsZodiac5(soc.GetDeviceType()) ? static_cast<SocIMX1*>(&soc) : nullptr;
}

uint32_t word(SocIMX1& soc, uint32_t address) {
    uint32_t value;
    require(soc.ReadGuest(address, &value, 4), "zodiac native: guest memory isn't mapped RAM");
    return value;
}

bool romChecks(SocIMX1& soc) {
    return soc.RomHas(function, functionCode, sizeof(functionCode)) &&
           soc.RomHas(waitCall, waitCode, sizeof(waitCode)) && soc.RomHas(patch, patchCode, sizeof(patchCode));
}

// The machine on the boundary in the UI task: Thumb, System mode with interrupts on, the frame and the event
// globals as EvtGetSysEvent left them, and the system tables below r9.
// ASSUMPTION: only the UI task runs EvtGetSysEvent (the event manager is the UI's).
bool validate(SocIMX1& soc) {
    uint32_t r[16], cpsr;
    if (!soc.GuestCallPending() || soc.NativeSite() != site || !soc.GuestRegisters(r, &cpsr) ||
        r[15] != (site | 1) || (cpsr & 0xff) != 0x3f || !romChecks(soc))
        return false;
    if ((r[13] & 3) || r[6] != r[4] + 0x20) return false;
    uint8_t frame[frameSize];
    if (!soc.ReadGuest(r[13], frame, sizeof(frame))) return false;
    auto at = [&](uint32_t offset) {
        uint32_t v;
        std::memcpy(&v, frame + offset, 4);
        return v;
    };
    if (at(savedLr) != patchReturn || at(savedR0) != r[5]) return false;
    if (word(soc, r[9] - 8) != bootTable || word(soc, r[9] - 4) != dalTable) return false;
    const uint32_t globals = word(soc, word(soc, r[9]) + 4) + eventOffset;
    if (at(globalsSlot) != globals || word(soc, globals + 0x3c) != r[4]) return false;
    uint8_t waiting;
    return soc.ReadGuest(r[6] + 5, &waiting, 1) && !waiting;
}

void notePatched(uint16_t trap, uint32_t hook) {
    static std::set<uint16_t> noted;
    if (noted.insert(trap).second)
        std::fprintf(stderr, "zodiac native: trap %#06x is patched (hook %#010x), calling its ROM routine\n", trap,
                     hook);
}
}  // namespace

bool Applies(SoC& soc) { return zodiac(soc); }

bool Prepare(SoC& soc, bool wake) {
    auto* z = zodiac(soc);
    if (!z || syscallDispatchInProgress(soc.GetSyscallDispatch())) return false;
    try {
        if (!romChecks(*z)) return false;
        if (z->GuestCallPending() && z->NativeSite() == site) return validate(*z);
        if (z->GuestCallPending()) return false;
        // As on the T|X: 0.1 s for an app on its way to its event wait, then one waiting with no timeout
        // (it leaves the UI task blocked for good) is woken when the guest idles; a busy one gets the rest and then the same wake.
        constexpr uint64_t grace = prepareBudget / 10;
        if (z->RunToGuestPc(site | 1, grace, hz)) return validate(*z);
        if (wake && z->WakeIdle()) return z->RunToGuestPc(site | 1, prepareBudget, hz) && validate(*z);
        if (z->RunToGuestPc(site | 1, prepareBudget - grace, hz)) return validate(*z);
        return wake && z->WakeIdle() && z->RunToGuestPc(site | 1, prepareBudget, hz) && validate(*z);
    } catch (const std::runtime_error&) {
        return false;
    }
}

struct Session::Impl {
    SocIMX1& soc;
    uint32_t allocation{0}, depth{0}, siteSp{0};
    bool closed{false}, failed{false};
    uint32_t calls{0}, callbacks{0};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::map<uint32_t, std::function<void(uint32_t*)>> handlers;
    std::exception_ptr callbackError;

    explicit Impl(SocIMX1& soc) : soc(soc) {}

    void deadline() {
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(300),
                "zodiac native: session deadline exceeded");
    }

    // At a callback stop: the handler gets the registers, r0 to r3 go back and the guest returns to lr.
    void callback(uint32_t address) {
        uint32_t r[16];
        try {
            deadline();
            require(++callbacks <= 65536, "zodiac native: too many callbacks");
            const auto handler = handlers.find(address);
            require(handler != handlers.end() && soc.GuestRegisters(r), "zodiac native: unknown callback");
            auto fn = handler->second;  // the handler may unregister itself
            fn(r);
            for (unsigned i = 0; i < 4; ++i) require(soc.SetGuestRegister(i, r[i]), "zodiac native: callback result");
            require(soc.ReturnFromCallback(), "zodiac native: callback return");
        } catch (...) {
            failed = true;
            if (!callbackError) callbackError = std::current_exception();
            // Back to the caller with an error, so the call returns and the failure is reported.
            soc.SetGuestRegister(0, 1);
            soc.ReturnFromCallback();
        }
    }

    uint32_t invoke(uint16_t trap, std::array<uint32_t, 4> args, const std::vector<uint32_t>& stackArgs, bool boot) {
        deadline();
        require(!closed && !failed && syscallDispatchRecoveryActive(soc.GetSyscallDispatch()),
                "zodiac native: recovery scope isn't active");
        require(depth < 4 && ++calls <= 65536 && stackArgs.size() <= 16, "zodiac native: call bound exceeded");
        const Entry* entry = nullptr;
        for (const auto& candidate : entries)
            if (candidate.trap == trap) entry = &candidate;
        require(entry, "zodiac native: trap is outside the qualified set");
        require(soc.RomHas(entry->address, entry->code, sizeof(entry->code)), "zodiac native: ROM routine differs");
        // A system patch points the table entry at its own code; the call goes to the ROM routine.
        const uint32_t hooked = word(soc, bootTable + (trap & 0xfff));
        if (hooked != entry->address) notePatched(trap, hooked);

        uint32_t saved[16], cpsr;
        require(soc.GuestRegisters(saved, &cpsr), "zodiac native: registers");
        Defer restore([&] {
            for (unsigned i = 0; i < 15; ++i) soc.SetGuestRegister(i, saved[i]);
            soc.SetGuestRegister(16, cpsr);
            if (!soc.JumpGuest(saved[15])) failed = true;
        });
        uint32_t stack = boot ? siteSp - 256 : depth ? saved[13] : allocation + allocationSize - 64;
        stack = (stack - uint32_t(stackArgs.size() * 4) - 8) & ~7u;
        require(boot || depth || stack > allocation + dataSize + 256, "zodiac native: scratch stack exhausted");
        if (!stackArgs.empty())
            require(soc.WriteGuest(stack, stackArgs.data(), uint32_t(stackArgs.size() * 4)),
                    "zodiac native: stack arguments unmapped");
        for (unsigned i = 0; i < 4; ++i) require(soc.SetGuestRegister(i, args[i]), "zodiac native: arguments");
        require(soc.SetGuestRegister(13, stack) && soc.SetGuestRegister(14, zodiacInjectedReturn) &&
                    soc.JumpGuest(entry->address),
                "zodiac native: could not start the call");
        ++depth;
        Defer leave([&] { --depth; });
        uint32_t stops[3];
        unsigned count = 0;
        for (const auto& h : handlers)
            if (count < 3) stops[count++] = h.first;
        bool returned = false;
        for (;;) {
            const uint32_t before = callbacks;
            returned = soc.ExecuteNative(callSlice, hz, stops, count, [this](uint32_t a) { callback(a); });
            if (returned || callbackError || soc.Halted() || callbacks == before) break;
            deadline();
        }
        if (callbackError) std::rethrow_exception(callbackError);
        require(returned, "zodiac native: call deadline");
        uint32_t r[16];
        require(soc.GuestRegisters(r) && r[13] == stack, "zodiac native: call changed the stack");
        if (allocation)
            require(word(soc, allocation + dataSize) == marker && word(soc, allocation + allocationSize - 4) == marker,
                    "zodiac native: scratch stack guard damaged");
        return r[0];
    }
};

Session::Session(SoC& soc) {
    auto* z = zodiac(soc);
    require(z && syscallDispatchRecoveryActive(soc.GetSyscallDispatch()) &&
                !syscallDispatchInProgress(soc.GetSyscallDispatch()),
            "zodiac native: session needs a top-level recovery checkpoint");
    require(validate(*z), "zodiac native: not on the boundary");
    auto candidate = std::make_unique<Impl>(*z);
    uint32_t r[16];
    require(z->GuestRegisters(r), "zodiac native: registers");
    // MemPtrNew runs on the UI task's stack below the boundary's frame, like a call from there would.
    candidate->siteSp = r[13];
    candidate->allocation = candidate->invoke(0x8584, {allocationSize, 0, 0, 0}, {}, true);
    const uint32_t a = candidate->allocation;
    require(a && !(a & 3), "zodiac native: scratch allocation failed");
    require(uint64_t(a) + allocationSize <= r[13] - 512 || a > r[13] + frameSize, "zodiac native: scratch overlaps the UI frame");
    require(z->WriteGuest(a + dataSize, &marker, 4) && z->WriteGuest(a + allocationSize - 4, &marker, 4),
            "zodiac native: scratch isn't RAM");
    impl = std::move(candidate);
}

Session::~Session() = default;

uint32_t Session::Call(uint16_t trap, std::array<uint32_t, 4> args, std::vector<uint32_t> stackArgs) {
    try {
        return impl->invoke(trap, args, stackArgs, false);
    } catch (...) {
        impl->failed = true;
        throw;
    }
}

bool Session::Read(uint32_t address, void* dest, uint32_t size) {
    return !impl->closed && !impl->failed && size <= 16 * 1024 * 1024 && impl->soc.ReadGuest(address, dest, size);
}

bool Session::Write(uint32_t address, const void* src, uint32_t size) {
    return !impl->closed && !impl->failed && size <= 16 * 1024 * 1024 && impl->soc.WriteGuest(address, src, size);
}

void Session::RegisterCallback(uint32_t address, std::function<void(uint32_t*)> callback) {
    require(!impl->closed && !impl->failed && callback && address >= DataAddress() &&
                uint64_t(address) + 4 <= uint64_t(DataAddress()) + DataSize() && !(address & 3),
            "zodiac native: invalid callback address");
    require(impl->handlers.size() < 3, "zodiac native: callback slots exhausted");
    require(impl->handlers.emplace(address, std::move(callback)).second, "zodiac native: duplicate callback");
}

void Session::UnregisterCallback(uint32_t address) { impl->handlers.erase(address); }

uint32_t Session::DataAddress() const { return impl->allocation + 64; }
uint32_t Session::DataSize() const { return dataSize - 64; }

void Session::Close() {
    require(!impl->depth, "zodiac native: can't close from a callback");
    if (impl->closed) return;
    require(!impl->failed, "zodiac native: a failed session needs a whole-guest rollback");
    impl->handlers.clear();
    const uint32_t allocation = impl->allocation;
    require(word(impl->soc, allocation + dataSize) == marker && word(impl->soc, allocation + allocationSize - 4) == marker,
            "zodiac native: stack guards damaged before free");
    impl->allocation = 0;
    try {
        require(impl->invoke(0x84b8, {allocation, 0, 0, 0}, {}, true) == 0, "zodiac native: scratch free failed");
    } catch (...) {
        impl->failed = true;
        throw;
    }
    impl->closed = true;
}
}  // namespace ZodiacNative
