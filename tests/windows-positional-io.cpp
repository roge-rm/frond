#include <cassert>
#include <cstdint>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    assert(argc == 2);
    const int fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR | O_BINARY, 0600);
    assert(fd >= 0);
    assert(_lseeki64(fd, 17, SEEK_SET) == 17);
    auto worker = [&](int64_t offset, uint32_t initial) {
        for (uint32_t i = 0; i < 1000; ++i) {
            const uint32_t value = initial + i;
            uint32_t read = 0;
            assert(pwrite(fd, &value, sizeof(value), offset) == sizeof(value));
            assert(pread(fd, &read, sizeof(read), offset) == sizeof(read));
            assert(read == value);
        }
    };
    std::thread first(worker, 0, 1000), second(worker, 512, 2000);
    first.join();
    second.join();
    assert(_lseeki64(fd, 0, SEEK_CUR) == 17);
    char byte = 0;
    assert(pread(fd, &byte, sizeof(byte), (int64_t(1) << 32) + 1) == 0);
    assert(_lseeki64(fd, 0, SEEK_CUR) == 17);
    assert(pread(fd, &byte, sizeof(byte), -1) == -1 && errno == EINVAL);
    assert(close(fd) == 0);
    assert(unlink(argv[1]) == 0);
}
