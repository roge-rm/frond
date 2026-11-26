// Palm PIM records (classic and Garnet layouts) and Pocket Word notes from a device or a backup.
#include <fuzzer/FuzzedDataProvider.h>

#include "sync/PimRecord.h"
#include "sync/PwiNote.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    FuzzedDataProvider data(bytes, size);
    const auto kind = PimKind(data.ConsumeIntegralInRange(0, 3));
    const auto format = PalmPimFormat(data.ConsumeIntegralInRange(0, 1));
    const auto rest = data.ConsumeRemainingBytes<uint8_t>();
    PimRecord record;
    std::string error;
    if (DecodePalmPim(kind, format, rest, record, error)) {
        Bytes out;
        EncodePalmPim(record, format, out, error);
    }
    PalmPimText(rest);
    if (auto text = ReadPwiNote(rest)) {
        Bytes note = rest;
        ReplacePwiNoteText(note, *text + "x");
    }
    return 0;
}
