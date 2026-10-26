// Firmware-free tests of the host backend against bounded loopback peers.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "HostNetwork.h"
using namespace std::chrono_literals;
using Bytes = std::vector<uint8_t>;
static const HostNetwork::Key key{1, 2, 3};
static int endpoint(int type) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    timeval timeout{5, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    return fd;
}
static uint16_t port(int fd) {
    sockaddr_in a{};
    socklen_t size = sizeof(a);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) == 0);
    return ntohs(a.sin_port);
}
template <class F> static HostNetwork::Result finish(HostNetwork& net, F call) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        auto r = call();
        if (!r.waiting) {
            net.Complete(key);
            return r;
        }
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(1ms);
    }
}
static void tcp(HostNetwork& net, unsigned round) {
    int listener = endpoint(SOCK_STREAM);
    assert(listen(listener, 1) == 0);
    Bytes expected(512 * 1024);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = (i * 37 + round) ^ (i >> 8);
    std::atomic<bool> release{false};
    std::thread peer([&] {
        pollfd p{listener, POLLIN, 0};
        assert(poll(&p, 1, 5000) == 1);
        int fd = accept(listener, nullptr, nullptr);
        assert(fd >= 0);
        timeval timeout{5, 0};
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!release.load()) {
            assert(std::chrono::steady_clock::now() < end);
            std::this_thread::sleep_for(1ms);
        }
        Bytes actual;
        uint8_t buffer[997];
        for (;;) {
            auto n = recv(fd, buffer, sizeof(buffer), 0);
            assert(n >= 0);
            if (!n) break;
            actual.insert(actual.end(), buffer, buffer + n);
            assert(actual.size() <= expected.size());
        }
        assert(actual == expected);
        // Deliberately different write/read sizes exercise stream semantics.
        for (size_t offset = 0; offset < actual.size();) {
            auto n = send(fd, actual.data() + offset,
                          std::min<size_t>(997, actual.size() - offset), MSG_NOSIGNAL);
            assert(n > 0);
            offset += n;
        }
        assert(shutdown(fd, SHUT_WR) == 0);
        close(fd);
    });
    assert(net.Open(0, 1, 6).value == 4);
    assert(!finish(net, [&] { return net.Connect(0, 0x7f000001, port(listener), 100, key); }).error);
    assert(!net.SetOption(0, 0xffff, 0x1001, 4096).error);
    size_t sent = 0;
    bool backpressure = false;
    while (sent < expected.size()) {
        auto r = net.Send(0, expected.data() + sent, expected.size() - sent, 0, false, 0, 0, 100, key);
        if (r.waiting) {
            backpressure = true;
            break;
        }
        assert(!r.error && r.value > 0);
        sent += r.value;
    }
    assert(backpressure);  // The peer has not read anything; a real send wait was exercised.
    release = true;
    while (sent < expected.size()) {
        auto r = finish(net, [&] {
            return net.Send(0, expected.data() + sent, expected.size() - sent, 0, false, 0, 0, 100, key);
        });
        assert(!r.error && r.value > 0);
        sent += r.value;
    }
    assert(!net.Shutdown(0, 1).error);  // Half-close still permits the entire reply.
    Bytes actual;
    uint8_t buffer[1543];
    uint32_t ip;
    uint16_t remotePort;
    for (;;) {
        auto r = finish(net, [&] {
            return net.Receive(0, buffer, sizeof(buffer), 0, ip, remotePort, 100, key);
        });
        assert(!r.error && r.value >= 0);
        if (ip != 0x7f000001 || remotePort != port(listener))
            fprintf(stderr, "TCP peer after %zu bytes: n=%d ip=%08x port=%u expected=%u\n", actual.size(), r.value, ip, remotePort, port(listener));
        assert(ip == 0x7f000001 && remotePort == port(listener));
        if (!r.value) break;
        actual.insert(actual.end(), buffer, buffer + r.value);
        assert(actual.size() <= expected.size());
    }
    assert(actual == expected);
    peer.join();
    close(listener);
    assert(!net.Close(0).error);
}
static void udp(HostNetwork& net) {
    int fd = endpoint(SOCK_DGRAM);
    std::thread peer([&] {
        for (unsigned i = 0; i < 96; ++i) {
            uint8_t bytes[8192];
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            auto n = recvfrom(fd, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &length);
            assert(n >= 0);
            assert(sendto(fd, bytes, n, 0, reinterpret_cast<sockaddr*>(&from), length) == n);
        }
    });
    assert(net.Open(1, 2, 17).value == 5);
    assert(!finish(net, [&] { return net.Connect(1, 0x7f000001, port(fd), 100, key); }).error);
    uint32_t ip;
    uint16_t remotePort;
    uint8_t buffer[8192];
    const size_t sizes[] = {0, 1, 511, 1400, 8192};
    for (unsigned i = 0; i < 96; ++i) {
        if (i % 16 == 0) {
            uint32_t masks[] = {1u << 5, 0, 0};
            auto r = finish(net, [&] { return net.Select(6, masks, 1, key); });
            assert(!r.error && r.value == 0 && !masks[0]);
            assert(finish(net, [&] {
                return net.Receive(1, buffer, sizeof(buffer), 0, ip, remotePort, 1, key);
            }).error == 0x1212);
        }
        Bytes bytes(sizes[i % 5]);
        for (size_t j = 0; j < bytes.size(); ++j) bytes[j] = i ^ (j * 13);
        auto r = finish(net, [&] {
            return net.Send(1, bytes.data(), bytes.size(), 0, false, 0, 0, 100, key);
        });
        assert(!r.error && r.value == int(bytes.size()));
        uint32_t masks[] = {1u << 5, 0, 0};
        r = finish(net, [&] { return net.Select(6, masks, 100, key); });
        assert(!r.error && r.value == 1 && masks[0] == (1u << 5));
        r = net.Receive(1, buffer, 1, 2, ip, remotePort, 0, key);
        assert(!r.error && r.value == int(std::min<size_t>(1, bytes.size())));
        r = net.Receive(1, buffer, sizeof(buffer), 0, ip, remotePort, 0, key);
        assert(!r.error && r.value == int(bytes.size()));
        assert(ip == 0x7f000001 && remotePort == port(fd));
        assert(std::equal(bytes.begin(), bytes.end(), buffer));
        assert(net.Receive(1, buffer, sizeof(buffer), 0, ip, remotePort, 0, key).error == 0x122f);
    }
    peer.join();
    close(fd);
    net.Reset();
}
int main() {
    HostNetwork net;
    // Failed connects must not poison the reused Palm socket slot.
    int refused = endpoint(SOCK_STREAM);
    for (unsigned i = 0; i < 3; ++i) {
        assert(net.Open(0, 1, 6).value == 4);
        assert(finish(net, [&] { return net.Connect(0, 0x7f000001, port(refused), 100, key); }).error == 0x122d);
        assert(!net.Close(0).error);
        tcp(net, i);
        net.Reset();
    }
    close(refused);
    udp(net);
    puts("PASS: host TCP 3 x 512 KiB exact bidirectional transfers, send backpressure, half-close/EOF, refused-connect recovery; 96 UDP datagrams, peek, empty datagram, select/receive timeout recovery");
}
