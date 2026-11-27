// Lists and reads a FAT card a few folders deep, then makes the edits the file managers make, the
// input picking which. Only frondStorage::Error is caught: the callers catch that and nothing else.
#pragma once

#include <fuzzer/FuzzedDataProvider.h>

#include "storage/FatImage.h"

inline void FuzzWalk(frondStorage::FatImage& image, const std::string& path, int depth, std::vector<std::string>& seen) {
    for (const auto& entry : image.list(path)) {
        const auto child = frondStorage::FatImage::joined(path, entry.name);
        if (seen.size() < 64) seen.push_back(child);
        if (entry.directory) {
            if (depth < 3 && entry.name != "." && entry.name != "..") FuzzWalk(image, child, depth + 1, seen);
        } else {
            int64_t total = 0;
            image.read(entry, [&](const char*, int64_t n) { total += n; });
        }
    }
}

inline void FuzzFat(frondStorage::Blocks& blocks, FuzzedDataProvider& data) {
    using frondStorage::Error;
    try {
        frondStorage::FatImage image(blocks);
        image.freeBytes();
        std::vector<std::string> seen;
        try {
            FuzzWalk(image, "/", 0, seen);
        } catch (const Error&) {
        }
        image.check();
        image.fatCopiesMatch();
        auto pick = [&] { return seen.empty() ? std::string("/") : seen[data.ConsumeIntegralInRange<size_t>(0, seen.size() - 1)]; };
        for (int i = 0; i < 8 && data.remaining_bytes(); ++i) try {
                switch (data.ConsumeIntegralInRange(0, 6)) {
                    case 0: image.makeFolder(pick(), data.ConsumeRandomLengthString(16)); break;
                    case 1: image.remove(pick()); break;
                    case 2: image.move(pick(), pick(), data.ConsumeBool() ? data.ConsumeRandomLengthString(16) : std::string()); break;
                    case 3: image.copy(pick(), pick()); break;
                    case 4: {
                        const int64_t size = data.ConsumeIntegralInRange<int64_t>(0, 70000);
                        image.add(pick(), data.ConsumeRandomLengthString(16), [](char* b, int64_t n) {
                            std::memset(b, 'x', size_t(n));
                            return n;
                        }, size);
                        break;
                    }
                    case 5: image.sizeOf(pick()); image.find(pick()); break;
                    default: image.check(); break;
                }
            } catch (const Error&) {
            }
    } catch (const Error&) {
    }
}
