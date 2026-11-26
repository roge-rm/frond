// Hand-written inputs that once broke parsers found by the fuzz targets (make fuzz): a savestate string
// with no end, a ZIP member that declares a huge size, ZIP names left allocated, session files that declare
// more than they hold, and a session written again after it was read. Built with ASan and LSan.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ZipfileWalker.h"
#include "savestate/Chunk.h"
#include "session/session_file5.h"
#include "zip/miniz.h"

static void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::fprintf(stderr, "fuzz regressions: FAIL line %d: %s\n", line, what);
    std::exit(1);
}
#define CHECK(x) check((x), #x, __LINE__)

using Bytes = std::vector<uint8_t>;

static void put(Bytes& v, uint32_t x, int bytes) {
    for (int i = 0; i < bytes; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

static void savestateStrings() {
    // maxLength 6 reads 7 bytes (2 words); the stored text fills all of them without a NUL.
    uint32_t words[2];
    std::memset(words, 'A', sizeof(words));
    Chunk bad(2, words);
    CHECK(bad.GetString(6).empty() && bad.HasError());

    // A string cut off by the end of the chunk is an error too, and returns nothing.
    Chunk shortChunk(1, words);
    CHECK(shortChunk.GetString(6).empty() && shortChunk.HasError());

    // A terminated one reads as before, whatever follows the NUL.
    uint32_t good[3] = {0x00636261, 0x58585858, 0x1234};  // "abc", then padding
    Chunk ok(3, good);
    CHECK(ok.GetString(6) == "abc" && !ok.HasError() && ok.Get32() == 0x1234);
}

// A ZIP with one stored member whose central directory entry says it holds `declared` bytes.
static Bytes zipOf(const std::string& name, const std::string& data, uint32_t declared, uint16_t method = 0) {
    Bytes z;
    const uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    put(z, 0x04034b50, 4);
    put(z, 10, 2), put(z, 0, 2), put(z, method, 2), put(z, 0, 2), put(z, 0x21, 2);
    put(z, crc, 4), put(z, uint32_t(data.size()), 4), put(z, method ? declared : uint32_t(data.size()), 4);
    put(z, uint32_t(name.size()), 2), put(z, 0, 2);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), data.begin(), data.end());
    const size_t central = z.size();
    put(z, 0x02014b50, 4);
    put(z, 20, 2), put(z, 10, 2), put(z, 0, 2), put(z, method, 2), put(z, 0, 2), put(z, 0x21, 2);
    put(z, crc, 4), put(z, uint32_t(data.size()), 4), put(z, declared, 4);
    put(z, uint32_t(name.size()), 2), put(z, 0, 2), put(z, 0, 2), put(z, 0, 2), put(z, 0, 2);
    put(z, 0, 4), put(z, 0, 4);
    z.insert(z.end(), name.begin(), name.end());
    const size_t centralSize = z.size() - central;
    put(z, 0x06054b50, 4);
    put(z, 0, 2), put(z, 0, 2), put(z, 1, 2), put(z, 1, 2);
    put(z, uint32_t(centralSize), 4), put(z, uint32_t(central), 4), put(z, 0, 2);
    return z;
}

static void zipMembers() {
    {
        Bytes z = zipOf("a.txt", "hello", 5);
        ZipfileWalker w(z.size(), z.data());
        CHECK(w.GetState() == ZipfileWalker::stateOpen && w.GetCurrentEntrySize() == 5);
        const uint8_t* content = w.GetCurrentEntryContent();
        CHECK(content && !std::memcmp(content, "hello", 5));
        // Walking to the end and stopping on a member frees its name when the walker goes.
    }
    {
        // A deflate member that declares 3 GiB: not unpacked, and no allocation of that size is tried.
        Bytes z = zipOf("big.bin", std::string("\x03\x00", 2), 0xc0000000u, 8);
        ZipfileWalker w(z.size(), z.data());
        CHECK(w.GetState() == ZipfileWalker::stateOpen);
        CHECK(w.GetCurrentEntryContent() == nullptr);
    }
}

static void sessionFiles() {
    Bytes memory(4096, 0);
    SessionFile5 made;
    made.SetDeviceName("palm-tx").SetMemory(memory.size(), memory.data());
    CHECK(made.Serialize());
    const auto* start = static_cast<const uint8_t*>(made.GetSerializedSession());
    Bytes file(start, start + made.GetSerializedSessionSize());

    SessionFile5 read;
    CHECK(read.Deserialize(file.size(), file.data()) && read.GetMemorySize() == memory.size());
    // Writing it again keeps the sections it points to.
    CHECK(read.Serialize() && read.GetMemorySize() == memory.size());
    const auto* readMemory = static_cast<const uint8_t*>(read.GetMemory());
    CHECK(readMemory[0] == 0 && readMemory[memory.size() - 1] == 0);

    // The memory length (a 64-bit field after the header, name, display mode, RAM size and four others)
    // says 400 MiB for a few dozen compressed bytes.
    const size_t memoryLength = SessionFile5::kHeaderBytes + 8 + 8 * 3;
    CHECK(file[memoryLength] == 0x00 && file[memoryLength + 1] == 0x10);
    Bytes lie = file;
    lie[memoryLength] = lie[memoryLength + 1] = lie[memoryLength + 2] = 0;
    lie[memoryLength + 3] = 0x19;  // 0x19000000
    SessionFile5 liar;
    CHECK(!liar.Deserialize(lie.size(), lie.data()));
}

int main() {
    savestateStrings();
    zipMembers();
    sessionFiles();
    std::puts("fuzz regressions: ok");
    return 0;
}
