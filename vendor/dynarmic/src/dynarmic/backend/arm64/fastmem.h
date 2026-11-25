/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <tuple>

#include <mcl/hash/xmrx.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;

struct DoNotFastmemMarkerHash {
    size_t operator()(const DoNotFastmemMarker& value) const {
        return mcl::hash::xmrx(std::get<0>(value).Value() ^ static_cast<u64>(std::get<1>(value)));
    }
};

struct FastmemPatchInfo {
    DoNotFastmemMarker marker;
    FakeCall fc;
    bool recompile;
    bool defer;
};

class FastmemManager {
public:
    explicit FastmemManager(ExceptionHandler& eh)
            : exception_handler(eh) {}

    bool SupportsFastmem() const {
        return exception_handler.SupportsFastmem();
    }

    bool ShouldFastmem(DoNotFastmemMarker marker) const {
        if (disabled.load(std::memory_order_relaxed)) return false;
        std::lock_guard lock{markers_mutex};
        return do_not_fastmem.count(marker) == 0;
    }

    bool MarkDoNotFastmem(DoNotFastmemMarker marker) {
        std::lock_guard lock{markers_mutex};
        return do_not_fastmem.insert(marker).second;
    }

    void Disable() { disabled.store(true, std::memory_order_relaxed); }

private:
    ExceptionHandler& exception_handler;
    std::atomic<bool> disabled{false};
    mutable std::mutex markers_mutex;
    tsl::robin_set<DoNotFastmemMarker, DoNotFastmemMarkerHash> do_not_fastmem;
};

}  // namespace Dynarmic::Backend::Arm64
