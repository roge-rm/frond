// ZIP archives walked for imports.
#include <vector>

#include "ZipfileWalker.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    std::vector<uint8_t> copy(bytes, bytes + size);
    ZipfileWalker walker(copy.size(), copy.data());
    walker.GetTotalEntries();
    for (int i = 0; i < 64 && walker.GetState() == ZipfileWalker::stateOpen; ++i) {
        walker.GetCurrentEntryName();
        walker.GetCurrentEntrySize();
        walker.GetCurrentEntryContent();
        walker.Next();
    }
    return 0;
}
