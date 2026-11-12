#ifndef FROND_GUEST_EXPORT_H
#define FROND_GUEST_EXPORT_H

#include <memory>

#include "ReadOnlyGuest.h"
#include "syscall_dispatch.h"

// Preparation runs on the live guest and may advance time and change guest state,
// even when it cannot reach the event loop. Only the export work that follows a
// successful preparation is checkpointed and rolled back.
class GuestExport {
 public:
    explicit GuestExport(SoC& soc, SyscallPrepareMode mode = SYSCALL_PREPARE_WAKE) : soc(soc) {
        if (syscallDispatchPrepare(soc.GetSyscallDispatch(), mode))
            checkpoint = std::make_unique<ReadOnlyGuest>(soc);
    }
    bool Ready() const { return bool(checkpoint); }
    void Finish() {
        if (checkpoint) syscallDispatchValidateCommit(soc.GetSyscallDispatch());
    }

 private:
    SoC& soc;
    std::unique_ptr<ReadOnlyGuest> checkpoint;
};
#endif
