#ifndef FROND_HOST_NETWORK_H
#define FROND_HOST_NETWORK_H
#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
// Host descriptors and DNS jobs belong to this live runtime, never to a savestate.
// All socket calls are nonblocking. Only the resolver worker can wait in host I/O.
class HostNetwork {
   public:
    using Key = std::tuple<uint32_t, uint32_t, uint16_t>;
    struct Result {
        int32_t value{-1};
        uint16_t error{};
        bool waiting{};
    };
    HostNetwork();
    ~HostNetwork();
    void Reset();
    Result Open(unsigned index, unsigned type, int protocol);
    Result Close(unsigned index);
    Result Connect(unsigned index, uint32_t address, uint16_t port, int32_t ticks, const Key& key);
    Result Bind(unsigned index, uint32_t address, uint16_t port);
    Result Send(unsigned index, const void* bytes, size_t size, unsigned flags, bool peer,
                uint32_t address, uint16_t port, int32_t ticks, const Key& key);
    Result Receive(unsigned index, void* bytes, size_t size, unsigned flags, uint32_t& address,
                   uint16_t& port, int32_t ticks, const Key& key);
    Result Select(unsigned width, uint32_t masks[3], int32_t ticks, const Key& key);
    Result Resolve(const std::string& name, uint32_t& address, int32_t ticks, const Key& key);
    Result Shutdown(unsigned index, unsigned direction);
    Result Address(unsigned index, bool peer, uint32_t& address, uint16_t& port);
    Result SetOption(unsigned index, uint16_t level, uint16_t option, uint32_t value);
    Result GetOption(unsigned index, uint16_t level, uint16_t option, uint32_t& value);
    void Complete(const Key& key);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
#endif
