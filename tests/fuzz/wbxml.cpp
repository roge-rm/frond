// AirSync WBXML bodies from the device.
#include <string>

#include "sync/Wbxml.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    frondSync::WbxmlNode root;
    std::string error;
    if (frondSync::DecodeWbxml(bytes, size, root, error)) {
        frondSync::EncodeWbxml(root);
    }
    return 0;
}
