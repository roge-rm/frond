/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <algorithm>

#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    if (const auto it = range_of.find(location); it != range_of.end()) {
        block_ranges.subtract(std::make_pair(it->second, std::set<IR::LocationDescriptor>{location}));
    }
    range_of[location] = range;
    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    block_ranges.clear();
    range_of.clear();
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (auto invalidate_interval : ranges) {
        auto pair = block_ranges.equal_range(invalidate_interval);
        for (auto it = pair.first; it != pair.second; ++it) {
            for (const auto& descriptor : it->second) {
                erase_locations.insert(descriptor);
            }
        }
    }
    // Frond: the erased blocks leave block_ranges, so Overlaps and Lines see only live code.
    for (const auto& location : erase_locations) {
        if (const auto it = range_of.find(location); it != range_of.end()) {
            block_ranges.subtract(std::make_pair(it->second, std::set<IR::LocationDescriptor>{location}));
            range_of.erase(it);
        }
    }
    return erase_locations;
}

template<typename ProgramCounterType>
bool BlockRangeInformation<ProgramCounterType>::Overlaps(boost::icl::discrete_interval<ProgramCounterType> range) const {
    const auto pair = block_ranges.equal_range(range);
    return pair.first != pair.second;
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::Words(ProgramCounterType page_base, std::uint64_t* words) const {
    const ProgramCounterType last = page_base + 0xfff;
    const auto pair = block_ranges.equal_range(boost::icl::discrete_interval<ProgramCounterType>::closed(page_base, last));
    for (auto it = pair.first; it != pair.second; ++it) {
        const ProgramCounterType lo = std::max(boost::icl::first(it->first), page_base);
        const ProgramCounterType hi = std::min(boost::icl::last(it->first), last);
        for (unsigned word = unsigned((lo - page_base) >> 2); word <= unsigned((hi - page_base) >> 2); ++word) {
            words[word >> 6] |= std::uint64_t(1) << (word & 63);
        }
    }
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
