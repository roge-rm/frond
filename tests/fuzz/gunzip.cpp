// gzip images (the IMX1 firmware loader and session imports).
#include "GunzipContext.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    GunzipContext gunzip(bytes, size, 4096);
    for (int i = 0; i < 4096 && gunzip.GetState() == int(GunzipContext::State::more); ++i) gunzip.Continue();
    return 0;
}
