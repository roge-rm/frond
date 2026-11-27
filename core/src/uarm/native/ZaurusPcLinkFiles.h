#ifndef FROND_ZAURUS_PC_LINK_FILES_H
#define FROND_ZAURUS_PC_LINK_FILES_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace zaurusPcLink {

// The USB network transport supplies a connected stream. Each call transfers all bytes or throws.
class Stream {
   public:
    virtual ~Stream() = default;
    virtual void CheckCancelled() {}
    virtual void Send(const uint8_t* bytes, size_t size) = 0;
    virtual void Receive(uint8_t* bytes, size_t size) = 0;
};

struct Entry {
    std::string name;
    bool directory;
};

// A guest session on the stock Zaurus Samba share. Paths are relative to Internal Disk.
class Client {
   public:
    explicit Client(Stream& stream);
    ~Client();

    std::vector<Entry> List(const std::string& remote);
    void Send(const std::string& remote, FILE* source);
    void Receive(const std::string& remote, FILE* target);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace zaurusPcLink
#endif
