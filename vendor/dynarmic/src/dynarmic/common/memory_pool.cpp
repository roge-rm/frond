/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/common/memory_pool.h"

#include <cstdlib>
#include <vector>

namespace Dynarmic::Common {

namespace {
// Frond: each IR block has its own pool, and a 4096-instruction slab is over 256 KiB, which
// malloc maps and unmaps (and the kernel zero-fills) for every compile. Freed slabs are kept
// per thread and handed to the next pool of the same slab size instead.
constexpr size_t kKeptSlabs = 4;

struct KeptSlabs {
    size_t bytes = 0;
    std::vector<char*> slabs;
    ~KeptSlabs() {
        for (char* slab : slabs) {
            std::free(slab);
        }
    }
};

thread_local KeptSlabs kept;

char* TakeSlab(size_t bytes) {
    if (kept.bytes == bytes && !kept.slabs.empty()) {
        char* slab = kept.slabs.back();
        kept.slabs.pop_back();
        return slab;
    }
    return static_cast<char*>(std::malloc(bytes));
}

void GiveSlab(char* slab, size_t bytes) {
    if (kept.bytes != bytes) {
        for (char* s : kept.slabs) {
            std::free(s);
        }
        kept.slabs.clear();
        kept.bytes = bytes;
    }
    if (kept.slabs.size() < kKeptSlabs) {
        kept.slabs.push_back(slab);
    } else {
        std::free(slab);
    }
}
}  // namespace

Pool::Pool(size_t object_size, size_t initial_pool_size)
        : object_size(object_size), slab_size(initial_pool_size) {
    AllocateNewSlab();
}

Pool::~Pool() {
    GiveSlab(current_slab, object_size * slab_size);

    for (char* slab : slabs) {
        GiveSlab(slab, object_size * slab_size);
    }
}

void* Pool::Alloc() {
    if (remaining == 0) {
        slabs.push_back(current_slab);
        AllocateNewSlab();
    }

    void* ret = static_cast<void*>(current_ptr);
    current_ptr += object_size;
    remaining--;

    return ret;
}

void Pool::AllocateNewSlab() {
    current_slab = TakeSlab(object_size * slab_size);
    current_ptr = current_slab;
    remaining = slab_size;
}

}  // namespace Dynarmic::Common
