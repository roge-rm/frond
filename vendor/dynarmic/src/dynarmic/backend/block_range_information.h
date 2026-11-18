/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <set>

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);
    // Frond: whether a live block's code overlaps the range.
    bool Overlaps(boost::icl::discrete_interval<ProgramCounterType> range) const;
    // Frond: ORs into words (16 of them) a bit per 4-byte word of the 4 KiB page at page_base that a live
    // block's code covers.
    void Words(ProgramCounterType page_base, std::uint64_t* words) const;

private:
    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;
    // Frond: each block's range, so an invalidated block leaves block_ranges.
    tsl::robin_map<IR::LocationDescriptor, boost::icl::discrete_interval<ProgramCounterType>> range_of;
};

}  // namespace Dynarmic::Backend
