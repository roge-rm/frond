// Sharp HDD1 archives for the Zaurus CF builder (storage/ZaurusCf.h): gzip, tar, the nested
// factory seed and the checks on what they hold, built into a sink that drops the image.
#include "storage/Errors.h"
#include "storage/ZaurusCf.h"

#include <sys/mman.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    const int fd = memfd_create("hdd1", MFD_CLOEXEC);
    if (fd < 0) return 0;
    for (size_t at = 0; at < size;) {
        const auto n = write(fd, bytes + at, size - at);
        if (n <= 0) {
            close(fd);
            return 0;
        }
        at += size_t(n);
    }
    lseek(fd, 0, SEEK_SET);
    frondZaurusCf::Request request;
    request.hdd1Fd = fd;
    request.hdd1Size = int64_t(size);
    try {
        frondZaurusCf::build(request, [](uint64_t, const uint8_t*, size_t) {});
    } catch (const frondStorage::Error&) {
    }
    close(fd);
    return 0;
}
