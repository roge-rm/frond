// The NetLib redirect to host sockets isn't ported to Windows: every call reports that the stack has
// no resources. The PPP and Wi-Fi paths through the built-in slirp network work as elsewhere.
#ifdef _WIN32
#include "HostNetwork.h"

namespace {
constexpr uint16_t unavailable = 0x1206;  // netErrNoMoreSockets
}  // namespace

struct HostNetwork::Impl {};
HostNetwork::HostNetwork() : impl(new Impl) {}
HostNetwork::~HostNetwork() = default;
void HostNetwork::Reset() {}
HostNetwork::Result HostNetwork::Open(unsigned, unsigned, int) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Close(unsigned) { return {0}; }
HostNetwork::Result HostNetwork::Connect(unsigned, uint32_t, uint16_t, int32_t, const Key&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Bind(unsigned, uint32_t, uint16_t) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Send(unsigned, const void*, size_t, unsigned, bool, uint32_t, uint16_t, int32_t,
                                      const Key&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Receive(unsigned, void*, size_t, unsigned, uint32_t&, uint16_t&, int32_t,
                                         const Key&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Select(unsigned, uint32_t[3], int32_t, const Key&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Resolve(const std::string&, uint32_t&, int32_t, const Key&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Shutdown(unsigned, unsigned) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::Address(unsigned, bool, uint32_t&, uint16_t&) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::SetOption(unsigned, uint16_t, uint16_t, uint32_t) { return {-1, unavailable}; }
HostNetwork::Result HostNetwork::GetOption(unsigned, uint16_t, uint16_t, uint32_t&) { return {-1, unavailable}; }
void HostNetwork::Complete(const Key&) {}
#endif
