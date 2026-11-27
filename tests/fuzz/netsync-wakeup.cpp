// The Network HotSync wakeup answer the virtual router makes from a guest's UDP packet.
#include <cstdint>
#include <cstddef>

#include "net/NetSyncWakeup.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (!size) return 0;
    frondNet::NetSyncWakeupAnswer(bytes + 1, size - 1, bytes[0] & 1);
    return 0;
}
