#ifndef FROND_STORAGE_TAR_STREAM_H
#define FROND_STORAGE_TAR_STREAM_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

// Streaming reads for the Zaurus CF builder: a file descriptor, gzip (one or more members, CRC
// checked) and tar as Python's tarfile reads it (ustar, GNU long names, pax). Everything throws
// frondStorage::Error on bad or cut-off data.
namespace frondTar {

class Reader {
   public:
    virtual ~Reader() = default;
    // Fills `n` bytes unless the stream ends first; returns what it read.
    virtual size_t read(uint8_t* data, size_t n) = 0;
};

// Reads a file descriptor from where it is. `onRead` sees each chunk's byte count and may throw.
class FdReader : public Reader {
   public:
    FdReader(int fd, std::function<void(size_t)> onRead = {}) : fd_(fd), onRead_(std::move(onRead)) {}
    size_t read(uint8_t* data, size_t n) override;

   private:
    int fd_;
    std::function<void(size_t)> onRead_;
};

// Bytes already in memory.
class MemoryReader : public Reader {
   public:
    MemoryReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    size_t read(uint8_t* data, size_t n) override;

   private:
    const uint8_t* data_;
    size_t size_, at_{0};
};

// Lets the first bytes be looked at before reading on.
class PeekReader : public Reader {
   public:
    explicit PeekReader(Reader& source) : source_(source) {}
    // True when the stream starts with gzip's 1f 8b.
    bool gzipped();
    size_t read(uint8_t* data, size_t n) override;

   private:
    Reader& source_;
    uint8_t head_[2]{};
    size_t have_{0}, at_{0};
    bool peeked_{false};
};

// gzip as Python's gzip module reads it: members one after another, zero padding between them.
class GzipReader : public Reader {
   public:
    explicit GzipReader(Reader& source);
    ~GzipReader() override;
    size_t read(uint8_t* data, size_t n) override;

   private:
    struct State;
    std::unique_ptr<State> state_;
};

struct Member {
    std::string name, linkname;  // bytes as stored, after long names and pax
    char type{'0'};              // '0' regular (also '\0', '7'), '1' hard link, '2' symlink, '5' folder, else as stored
    uint32_t mode{};
    int64_t uid{}, gid{}, size{}, mtime{};
    bool sparse{};

    bool isReg() const { return type == '0'; }
    bool isDir() const { return type == '5'; }
    bool isSym() const { return type == '2'; }
    bool isLink() const { return type == '1'; }
};

class TarReader {
   public:
    // `limit` caps the bytes read from `source`, headers and padding included.
    TarReader(Reader& source, uint64_t limit) : source_(source), limit_(limit) {}
    // The next member, false at the end of the archive. Skips what is left of the last one.
    bool next(Member& member);
    // The current member's data, up to what is left of it.
    size_t readData(uint8_t* data, size_t n);
    uint64_t dataLeft() const { return left_; }

   private:
    void readBlock(uint8_t* block, bool& empty);
    void readExact(uint8_t* data, size_t n);
    void skip(uint64_t n);
    bool header(Member& member, bool dircheck, bool first);
    std::string payload(uint64_t size);

    Reader& source_;
    uint64_t limit_, offset_{0}, left_{0}, padding_{0};
    std::map<std::string, std::string> global_;
};

}  // namespace frondTar

#endif
