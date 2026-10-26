// Link-time libc resolver double: every outcome is controlled; no DNS escapes
// to the machine's resolver or an external service. Exercise the real worker,
// bounded queue, cancellation, error mapping and guest-side polling unchanged.
#include <arpa/inet.h>
#include <netdb.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "HostNetwork.h"
using namespace std::chrono_literals;
namespace {
std::mutex mutex;
std::condition_variable changed;
std::map<std::string, unsigned> calls;
std::string held;
bool released = false;
const HostNetwork::Key key{1, 2, 0xa81b};
void hold(const std::string& name) {
    std::lock_guard lock(mutex);
    held = name;
    released = false;
}
void entered(const std::string& name) {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return calls[name] != 0; }));
}
void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
}
unsigned count(const std::string& name) {
    std::lock_guard lock(mutex);
    return calls[name];
}
HostNetwork::Result resolve(HostNetwork& net, const std::string& name, uint32_t& ip,
                            int ticks = 100) {
    auto end = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        auto r = net.Resolve(name, ip, ticks, key);
        if (!r.waiting) {
            net.Complete(key);
            return r;
        }
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(1ms);
    }
}
}
extern "C" int __wrap_getaddrinfo(const char* name, const char*, const addrinfo* hints,
                                   addrinfo** answers) {
    assert(hints && hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM);
    unsigned attempt;
    {
        std::unique_lock lock(mutex);
        attempt = ++calls[name];
        changed.notify_all();
        if (held == name) assert(changed.wait_for(lock, 5s, [] { return released; }));
    }
    *answers = nullptr;
    const std::string host(name);
    if (host == "missing.test") return EAI_NONAME;
    if (host == "transient.test" && attempt == 1) return EAI_AGAIN;
    auto* address = new sockaddr_in{};
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(host == "old.test" ? 0x7f000002 : 0x7f000001);
    auto* answer = new addrinfo{};
    answer->ai_family = AF_INET;
    answer->ai_addrlen = sizeof(*address);
    answer->ai_addr = reinterpret_cast<sockaddr*>(address);
    *answers = answer;
    return 0;
}
extern "C" void __wrap_freeaddrinfo(addrinfo* answer) {
    delete reinterpret_cast<sockaddr_in*>(answer->ai_addr);
    delete answer;
}
int main() {
    HostNetwork net;
    uint32_t ip = 0xfeedface;
    assert(!net.Resolve("127.0.0.9", ip, 0, key).error && ip == 0x7f000009);
    assert(net.Resolve("", ip, 100, key).error == 0x1236);
    assert(net.Resolve(std::string(256, 'x'), ip, 100, key).error == 0x1236);
    for (unsigned i = 0; i < 64; ++i) {
        auto r = resolve(net, "success.test", ip);
        assert(!r.error && !r.value && ip == 0x7f000001);
    }
    assert(count("success.test") == 64);  // Completed keys do not reuse stale DNS answers.
    ip = 0xfeedface;
    assert(resolve(net, "missing.test", ip).error == 0x123e && ip == 0xfeedface);
    assert(resolve(net, "transient.test", ip).error == 0x123a && ip == 0xfeedface);
    assert(!resolve(net, "transient.test", ip).error && ip == 0x7f000001);

    hold("poll.test");
    assert(net.Resolve("poll.test", ip, 0, key).error == 0x122f);
    entered("poll.test");
    assert(net.Resolve("poll.test", ip, 0, key).error == 0x122f);
    release();
    assert(!resolve(net, "poll.test", ip).error && count("poll.test") == 1);

    hold("slow.test");
    assert(net.Resolve("slow.test", ip, 1, key).waiting);
    entered("slow.test");
    assert(resolve(net, "slow.test", ip, 1).error == 0x123a);
    release();
    assert(!resolve(net, "after-timeout.test", ip).error);

    hold("old.test");
    assert(net.Resolve("old.test", ip, -1, key).waiting);
    entered("old.test");
    net.Reset();  // Must neither block on libc nor allow the old result into the reused key.
    assert(net.Resolve("new.test", ip, 100, key).waiting);
    release();
    assert(!resolve(net, "new.test", ip).error && ip == 0x7f000001);

    hold("blocker.test");
    assert(net.Resolve("blocker.test", ip, -1, key).waiting);
    entered("blocker.test");
    std::vector<HostNetwork> queued(16);
    for (auto& client : queued) assert(client.Resolve("cancelled.test", ip, -1, key).waiting);
    HostNetwork overflow;
    assert(overflow.Resolve("overflow.test", ip, -1, key).error == 0x1206);
    overflow.Complete(key);
    for (auto& client : queued) client.Reset();
    // Cancelling queued jobs makes room without waiting for the active lookup.
    assert(overflow.Resolve("after-cancel.test", ip, 100, key).waiting);
    release();
    assert(!resolve(overflow, "after-cancel.test", ip).error);
    assert(count("cancelled.test") == 0 && count("overflow.test") == 0);
    net.Reset();
    puts("PASS: 64 fresh async DNS lookups, numeric/invalid names, negative/transient answers, zero-time polling, timeout and reset cancellation, bounded queue recovery; no external resolver");
}
