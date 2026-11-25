// DLP request and response parsing.
#include "sync/Dlp.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    const Bytes message(bytes, bytes + size);
    uint8_t function = 0;
    std::vector<DlpArg> args;
    if (DlpParseRequest(message, function, args)) DlpResponse(function, 0, args);
    uint16_t error = 0;
    args.clear();
    DlpParseResponse(message, size ? message[0] & 0x7f : dlpReadUserInfo, error, args);
    return 0;
}
