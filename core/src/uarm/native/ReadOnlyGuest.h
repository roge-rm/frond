#ifndef FROND_READ_ONLY_GUEST_H
#define FROND_READ_ONLY_GUEST_H

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "SoC.h"
#include "cputil.h"
#include "syscall_dispatch.h"
#include "sdcard.h"
#include "soc_PXA.h"
#include "mmiodev_MemoryStickController.h"
#include "CardBacking.h"
#include "nand.h"

// Palm database reads can populate/evict NVFS caches and run other guest work.
// Restore the guest after export, including when an injected call times out.
class ReadOnlyGuest {
 public:
    explicit ReadOnlyGuest(SoC& soc) : soc(soc), serialCheckpoint(soc), inputCheckpoint(soc), networkCheckpoint(soc.networkSimulation) {
        if (!soc.Save()) throw std::runtime_error("cannot checkpoint guest");
        state = copy(soc.GetSavestate());
        ram = copy(soc.GetMemoryData());
        if (auto* chip = soc.GetNand(); chip && !nandSaveCheckpoint(chip, nand))
            throw std::runtime_error("cannot checkpoint NAND");
        ramPages = copy(soc.GetMemoryDirtyPages());
        if (sdCardInitialized()) {
            sdPin = sdCardSnapshot();
            sdPages = copy(sdCardDirtyPages());
            sdDirty = sdCardIsDirty();
        }
        if (auto* pxa = dynamic_cast<SocPXA*>(&soc)) {
            if (auto* ms = pxa->GetMemoryStick())
                msPinned = msCtrlrSaveCheckpoint(ms, msPin, msProtect);
        }
        syscallDispatchBeginRecovery(soc.GetSyscallDispatch());
    }
    // Keep successful install/launch changes and end recovery/I/O freezing now.
    // Idempotent so callers can commit before the lexical scope ends.
    void Commit() {
        if (!active) return;
        if (syscallDispatchInProgress(soc.GetSyscallDispatch()))
            throw std::runtime_error("cannot commit an unfinished injected call");
        syscallDispatchValidateCommit(soc.GetSyscallDispatch());
        syscallDispatchEndRecovery(soc.GetSyscallDispatch());
        active = false;
        serialCheckpoint.Finish();
        inputCheckpoint.Finish();
        networkCheckpoint.Finish();
    }
    ~ReadOnlyGuest() {
        if (!active) return;
        // Restore before allowing the frontend to execute the guest again.
        syscallDispatchEndRecovery(soc.GetSyscallDispatch());
        restore(soc.GetMemoryData(), ram);
        if (auto* chip = soc.GetNand(); chip && !nandRestoreCheckpoint(chip, nand)) ERR("cannot restore NAND checkpoint");
        if (sdPin) sdCardRestore(sdPin);
        if (!inputCheckpoint.Restore(state.size(), state.data())) ERR("cannot restore export checkpoint");
        restore(soc.GetMemoryDirtyPages(), ramPages);
        if (sdPin) {
            restore(sdCardDirtyPages(), sdPages);
            sdCardSetDirty(sdDirty);
        }
        if (msPinned) {
            if (auto* pxa = dynamic_cast<SocPXA*>(&soc)) {
                if (auto* ms = pxa->GetMemoryStick(); ms && msCtrlrPresent(ms))
                    msCtrlrRestoreCheckpoint(ms, msPin, msProtect);
            }
        }
        soc.SetFramebufferDirty();
    }
    ReadOnlyGuest(const ReadOnlyGuest&) = delete;
    ReadOnlyGuest& operator=(const ReadOnlyGuest&) = delete;

 private:
    static std::vector<uint8_t> copy(Buffer b) {
        if (!b.size) return {};
        const auto* p = static_cast<const uint8_t*>(b.data);
        return {p, p + b.size};
    }
    static void restore(Buffer b, const std::vector<uint8_t>& saved) {
        if (!saved.empty()) std::memcpy(b.data, saved.data(), saved.size());
    }
    SoC& soc;
    SoC::SerialCheckpoint serialCheckpoint;
    SoC::InputCheckpoint inputCheckpoint;
    bool active{true};
    NetworkSimulation::Checkpoint networkCheckpoint;
    bool sdDirty{false};
    NandCheckpoint nand;
    std::vector<uint8_t> state, ram, ramPages, sdPages;
    std::shared_ptr<const CardSnapshot> sdPin, msPin;
    bool msProtect{false}, msPinned{false};
};
#endif
