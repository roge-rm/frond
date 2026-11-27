// Drives the core's Zaurus CF builder for tests/zaurus-cf-builder.py:
//   cli ARGS...                 frond --zaurus-cf ARGS...
//   cancel ARCHIVE OUTPUT       stops a build from the progress callback, checks it's refused
//   stream ARCHIVE [FONTS]      builds into a callback, checks the writes skip zeros and stay in bounds
//   geometry SECTORS            prints the FAT32 layout for a partition of that many sectors

#include "storage/Errors.h"
#include "storage/Fat32Writer.h"
#include "storage/ZaurusCf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    return 1;
}

int cancel(const char* archive, const char* output) {
    const int in = ::open(archive, O_RDONLY | O_CLOEXEC);
    const int out = ::open(output, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (in < 0 || out < 0) return fail("cancel: open");
    frondZaurusCf::Request request;
    request.hdd1Fd = in;
    request.hdd1Size = lseek(in, 0, SEEK_END);
    lseek(in, 0, SEEK_SET);
    uint64_t last = 0, calls = 0;
    bool backwards = false;
    request.progress = [&](uint64_t done, uint64_t total) {
        if (done < last || done > total) backwards = true;
        last = done;
        ++calls;
    };
    request.cancelled = [&] { return calls >= 3; };
    try {
        frondZaurusCf::buildToFd(request, out);
        return fail("cancel: the build finished");
    } catch (const frondStorage::Cancelled&) {
    } catch (const frondStorage::Error& e) {
        return fail(std::string("cancel: wrong error ") + e.message);
    }
    if (backwards) return fail("cancel: progress went backwards or past the total");
    if (last >= uint64_t(request.hdd1Size)) return fail("cancel: it read the whole archive");
    std::cout << "cancelled after " << last << " of " << request.hdd1Size << " bytes" << std::endl;
    return 0;
}

int stream(const char* archive, const char* fonts) {
    const int in = ::open(archive, O_RDONLY | O_CLOEXEC);
    const int font = fonts ? ::open(fonts, O_RDONLY | O_CLOEXEC) : -1;
    if (in < 0 || (fonts && font < 0)) return fail("stream: open");
    frondZaurusCf::Request request;
    request.hdd1Fd = in;
    request.fontRomFd = font;
    request.sizeGb = 5;
    const uint64_t end = frondZaurusCf::imageSectors(5) * frondZaurusCf::sectorBytes;
    uint64_t written = 0, calls = 0;
    std::string problem;
    try {
        frondZaurusCf::build(request, [&](uint64_t offset, const uint8_t* data, size_t size) {
            ++calls;
            written += size;
            if (!size || offset + size > end) problem = "write out of bounds";
            bool zero = true;
            for (size_t i = 0; i < size && zero; ++i) zero = !data[i];
            if (zero) problem = "an all-zero write";
            const uint64_t first = offset / 512, last = (offset + size - 1) / 512;
            for (uint64_t s = first; s <= last; ++s) {
                const uint64_t from = std::max(offset, s * 512), to = std::min(offset + size, (s + 1) * 512);
                bool z = true;
                for (uint64_t i = from; i < to && z; ++i) z = !data[i - offset];
                if (z) problem = "an all-zero sector in a write";
            }
        });
    } catch (const frondStorage::Error& e) {
        return fail(std::string("stream: ") + e.message);
    }
    if (!problem.empty()) return fail("stream: " + problem);
    std::cout << "streamed " << written << " bytes in " << calls << " writes" << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "cli") return frondZaurusCf::cliMain(std::vector<std::string>(argv + 2, argv + argc));
    if (mode == "cancel" && argc == 4) return cancel(argv[2], argv[3]);
    if (mode == "stream" && (argc == 3 || argc == 4)) return stream(argv[2], argc == 4 ? argv[3] : nullptr);
    if (mode == "geometry" && argc == 3) {
        const auto g = frondFat32::geometry(uint32_t(std::strtoul(argv[2], nullptr, 10)));
        std::cout << g.clusterSectors << " " << g.fatSectors << " " << g.clusters << std::endl;
        return 0;
    }
    std::cerr << "usage: test-zaurus-cf-builder cli|cancel|stream|geometry ..." << std::endl;
    return 2;
}
