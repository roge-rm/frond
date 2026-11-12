#ifndef FROND_TG50_PACE_SESSION_H
#define FROND_TG50_PACE_SESSION_H

#include <map>
#include <exception>
#include <vector>
#include "tg50_call_budget.h"
#include "tg50_pace_boundary.h"

namespace Tg50Pace {
class Session {
    class FailureScope {
        Session& session;
        int exceptions = std::uncaught_exceptions();
     public:
        explicit FailureScope(Session& session) : session(session) {}
        ~FailureScope() { if (std::uncaught_exceptions() > exceptions) session.failed = true; }
    };
 public:
    explicit Session(SoC& soc)
        : soc(soc), budget(soc.GetInjectedTimeNsec()), anchor(Boundary::Capture(soc, true)) {
        Require(OriginalLayout(soc), "TG50 original context layout changed");
    }
    void CheckOwner() {
        FailureScope failure(*this);
        budget.Check(soc.GetInjectedTimeNsec());
        Require(!failed, "TG50 transaction has failed");
        Require(anchor.SameOwner(Boundary::Capture(soc, false)),
                "TG50 injected call lost its app context");
    }
    void BeforeCall() {
        FailureScope failure(*this);
        CheckOwner();
        budget.EnterCall(soc.GetInjectedTimeNsec());
        Require(anchor.m68kStack.Contains(paceGetAreg(7) - 64, 64),
                "TG50 syscall arguments exceed its app stack");
    }
    void AfterCall() { CheckOwner(); }
    void AddAllocation(uint32_t address, uint32_t size) {
        FailureScope failure(*this);
        if (!address) return;
        CheckOwner();
        Require(size && !(address & 1) && uint64_t(address) + size <= 0x100000000ull &&
                cpuInspectWritableRamRange(soc.GetCpu(), address, size, false), "TG50 allocation is not writable RAM");
        for (const auto& item : allocations)
            Require(uint64_t(address) + size <= item.first ||
                    uint64_t(item.first) + item.second <= address, "TG50 scratch allocations overlap");
        allocations.emplace(address, size);
    }
    void BeforeFree(uint32_t address) {
        FailureScope failure(*this);
        CheckOwner();
        auto item = allocations.find(address);
        Require(item != allocations.end(), "TG50 free does not own its allocation");
        for (const auto& stub : stubs)
            Require(stub.first < address || stub.first - address >= item->second,
                    "TG50 callback must close before freeing its allocation");
    }
    void AfterFree(uint32_t address, uint16_t error) {
        FailureScope failure(*this);
        Require(!error, "TG50 could not free temporary allocation");
        allocations.erase(address);
    }
    void BeforeTransfer(uint32_t address, uint32_t size, bool write = true) {
        FailureScope failure(*this);
        CheckOwner();
        Require(!size || (uint64_t(address) + size <= 0x100000000ull &&
                         (write ? cpuInspectWritableRamRange(soc.GetCpu(), address, size, false) :
                                  cpuInspectRamRange(soc.GetCpu(), address, size))),
                "TG50 transfer is not entirely writable RAM");
    }
    void Register(uint32_t address, uint32_t argumentBytes) {
        FailureScope failure(*this);
        CheckOwner();
        Require(!(address & 1) && argumentBytes && argumentBytes <= 64,
                "TG50 invalid callback extent");
        bool owned = false;
        for (const auto& item : allocations)
            owned |= address >= item.first && uint64_t(address) + 2 <= uint64_t(item.first) + item.second;
        Require(owned && cpuInspectWritableRamRange(soc.GetCpu(), address, 2, false) && !stubs.count(address), "TG50 callback is outside owned scratch");
        uint8_t old[2];
        Require(cpuInspectRead(soc.GetCpu(), address, old, 2), "TG50 callback is unreadable");
        stubs.emplace(address, Stub{argumentBytes, uint16_t(old[0] << 8 | old[1])});
    }
    uint32_t BeforeCallback(uint32_t address) {
        FailureScope failure(*this);
        CheckOwner();
        budget.EnterCallback(soc.GetInjectedTimeNsec());
        auto stub = stubs.find(address);
        Require(stub != stubs.end() && paceGetPC() == address + 2 &&
                Byte(soc.GetCpu(), address) == 0x4e && Byte(soc.GetCpu(), address + 1) == 0x40,
                "TG50 callback entry changed");
        const uint32_t sp = paceGetAreg(7);
        Require(!(sp & 1) && anchor.m68kStack.Contains(sp, stub->second.argumentBytes + 4),
                "TG50 callback arguments exceed its app stack");
        Require(BigWord(soc.GetCpu(), sp) == PACE_RETURN_MAGIC_PC,
                "TG50 callback return is unqualified");
        activeCallbacks.push_back({address, sp, Boundary::Capture(soc, false)});
        return sp;
    }
    void AfterCallback(uint32_t address, uint32_t sp) {
        FailureScope failure(*this);
        CheckOwner();
        const auto current = Boundary::Capture(soc, false);
        Require(!activeCallbacks.empty() && activeCallbacks.back().address == address &&
                activeCallbacks.back().sp == sp &&
                current.frameSp == activeCallbacks.back().boundary.frameSp &&
                current.frame == activeCallbacks.back().boundary.frame &&
                stubs.count(address) && paceGetAreg(7) == sp &&
                BigWord(soc.GetCpu(), sp) == PACE_RETURN_MAGIC_PC,
                "TG50 callback changed its return frame");
        activeCallbacks.pop_back();
    }
    void Unregister(uint32_t address) noexcept {
        auto stub = stubs.find(address);
        if (stub == stubs.end()) return;
        cpuRemoveM68kTrap0Handler(soc.GetCpu(), address);
        if (!failed) {
            try {
                CheckOwner();
                BeforeTransfer(address, 2);
                uint8_t bytes[]{uint8_t(stub->second.original >> 8), uint8_t(stub->second.original)};
                Require(cpuNativeBuffer(soc.GetCpu(), address, bytes, 2, true, false), "TG50 callback restore failed");
            } catch (...) { failed = true; }
        }
        stubs.erase(stub);
    }
    void AbortHandlers() {
        for (const auto& stub : stubs) cpuRemoveM68kTrap0Handler(soc.GetCpu(), stub.first);
        stubs.clear();
        activeCallbacks.clear();
        failed = true;
    }
    void BeginFileTransfer(uint32_t bytes) {
        FailureScope failure(*this); CheckOwner(); budget.BeginFileTransfer(bytes, soc.GetInjectedTimeNsec());
    }
    void FileProgress(uint32_t bytes) {
        FailureScope failure(*this); CheckOwner(); budget.FileProgress(bytes, soc.GetInjectedTimeNsec());
    }
    void Close() {
        FailureScope failure(*this);
        CheckOwner();
        budget.RequireFileComplete();
        Require(stubs.empty() && allocations.empty() && activeCallbacks.empty(), "TG50 temporary guest objects remain live");
        const auto current = Boundary::Capture(soc, false);
        Require(current.frameSp == anchor.frameSp && current.m68kSp == anchor.m68kSp &&
                current.frame == anchor.frame, "TG50 original app frame was not restored");
        closed = true;
    }
    bool Closed() const { return closed; }
    Budget& ExecutionBudget() { return budget; }
    void Fail() { failed = true; }

 private:
    struct Stub { uint32_t argumentBytes; uint16_t original; };
    struct CallbackFrame { uint32_t address, sp; Boundary boundary; };
    SoC& soc;
    Budget budget;
    Boundary anchor;
    std::map<uint32_t, uint32_t> allocations;
    std::map<uint32_t, Stub> stubs;
    std::vector<CallbackFrame> activeCallbacks;
    bool failed{false}, closed{false};
};
}
#endif
