// Android stand-in for uarm/WifiRouter.cpp, which needs libslirp (and GLib).
// The router reports itself invalid, so the guest sees Wi-Fi with no network.
#include "WifiRouter.h"

struct WifiRouter::Impl {};
WifiRouter::WifiRouter() : impl(std::make_unique<Impl>()) {}
WifiRouter::~WifiRouter() = default;
bool WifiRouter::Valid() const { return false; }
void WifiRouter::Input(const uint8_t*, size_t, uint64_t) {}
void WifiRouter::Poll(uint64_t) {}
bool WifiRouter::Receive(std::vector<uint8_t>&) { return false; }
