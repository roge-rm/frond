#include "PalmConduitProtocol.h"

#include <windows.h>

extern "C" long LogAddEntry(const char* message, int, BOOL) {
    const auto runtime = GetModuleHandleW(L"sync20.dll");
    const auto log = runtime ? GetProcAddress(runtime, "FrondConduitLog") : nullptr;
    if (!log) return frondConduit::disconnected;
    return reinterpret_cast<long (*)(const char*)>(log)(message);
}
