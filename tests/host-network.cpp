#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "HostNetwork.h"
using namespace std::chrono_literals;
static uint16_t port(int fd) {
    sockaddr_in a{};
    socklen_t size = sizeof(a);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) == 0);
    return ntohs(a.sin_port);
}
static int server(int type) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    return fd;
}
template <class F>
static HostNetwork::Result finish(HostNetwork& net, const HostNetwork::Key& key, F f) {
    auto end = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        auto r = f();
        if (!r.waiting) {
            net.Complete(key);
            return r;
        }
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(2ms);
    }
}
int main() {
    HostNetwork net;
    const HostNetwork::Key connectKey{1, 2, 0xa80c}, readKey{1, 3, 0xa813}, selectKey{1, 4, 0xa815},
        dnsKey{1, 5, 0xa81b};
    int tcp = server(SOCK_STREAM);
    assert(listen(tcp, 1) == 0);
    const auto tcpPort = port(tcp);
    std::atomic<bool> release{false};
    std::thread echo([&] {
        int client = accept(tcp, nullptr, nullptr);
        assert(client >= 0);
        char data[16];
        auto n = recv(client, data, sizeof(data), 0);
        assert(n == 5);
        while (!release.load()) std::this_thread::sleep_for(1ms);
        assert(send(client, data, n, MSG_NOSIGNAL) == n);
        close(client);
    });
    assert(net.Open(0, 1, 0).value == 4);
    assert(finish(net, connectKey, [&] {
               return net.Connect(0, 0x7f000001, tcpPort, 100, connectKey);
           }).value == 0);
    assert(!net.SetOption(0, 6, 1, 1).error);
    uint32_t value = 0;
    assert(!net.GetOption(0, 6, 1, value).error && value == 1);
    assert(net.Send(0, "hello", 5, 0, false, 0, 0, 0, readKey).value == 5);
    char data[32]{};
    uint32_t ip = 0;
    uint16_t remotePort = 0;
    auto pending = net.Receive(0, data, sizeof(data), 0, ip, remotePort, 100, readKey);
    assert(pending.waiting);
    uint32_t masks[3] = {1u << 4, 0, 0};
    assert(net.Select(5, masks, 100, selectKey).waiting);
    release = true;
    auto ready = finish(net, selectKey, [&] { return net.Select(5, masks, 100, selectKey); });
    assert(ready.value == 1 && masks[0] == (1u << 4));
    assert(finish(net, readKey, [&] {
               return net.Receive(0, data, sizeof(data), 0, ip, remotePort, 100, readKey);
           }).value == 5);
    assert(!memcmp(data, "hello", 5));
    assert(finish(net, readKey, [&] {
               return net.Receive(0, data, sizeof(data), 0, ip, remotePort, 100, readKey);
           }).value == 0);
    echo.join();
    close(tcp);
    net.Close(0);
    int udp = server(SOCK_DGRAM);
    const auto udpPort = port(udp);
    std::thread datagram([&] {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        char bytes[8];
        auto n = recvfrom(udp, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&peer), &size);
        assert(n == 4 && !memcmp(bytes, "ping", 4));
        assert(sendto(udp, "pong", 4, 0, reinterpret_cast<sockaddr*>(&peer), size) == 4);
    });
    assert(net.Open(1, 2, 17).value == 5);
    assert(net.Send(1, "ping", 4, 0, true, 0x7f000001, udpPort, 0, readKey).value == 4);
    assert(finish(net, readKey, [&] {
               return net.Receive(1, data, sizeof(data), 2, ip, remotePort, 100, readKey);
           }).value == 4);
    assert(ip == 0x7f000001 && remotePort == udpPort);
    assert(net.Receive(1, data, sizeof(data), 0, ip, remotePort, 0, readKey).value == 4 &&
           !memcmp(data, "pong", 4));
    assert(!net.SetOption(1, 0xffff, 0x2000, 1).error);
    auto empty = net.Receive(1, data, sizeof(data), 0, ip, remotePort, 100, readKey);
    assert(!empty.waiting && empty.error == 0x122f);
    net.SetOption(1, 0xffff, 0x2000, 0);
    assert(finish(net, readKey, [&] {
               return net.Receive(1, data, sizeof(data), 0, ip, remotePort, 1, readKey);
           }).error == 0x1212);
    // A socket timeout caps a call's wait; an explicit zero always remains a poll.
    net.SetOption(1, 0xffff, 0x1006, 20);
    empty = net.Receive(1, data, sizeof(data), 0, ip, remotePort, 0, readKey);
    assert(!empty.waiting && empty.error == 0x122f);
    masks[0] = 1;
    masks[1] = masks[2] = 0;
    assert(net.Select(6, masks, 0, selectKey).error == 0x1208);
    datagram.join();
    close(udp);
    assert(finish(net, dnsKey, [&] { return net.Resolve("localhost", ip, 100, dnsKey); }).value ==
               0 &&
           ip == 0x7f000001);
    assert(net.Resolve("127.0.0.1", ip, 0, dnsKey).value == 0 && ip == 0x7f000001);
    // Reading SO_ERROR must not erase the result of a failed async connect.
    int refused = server(SOCK_STREAM);  // bound, deliberately not listening
    assert(net.Open(2, 1, 0).value == 6);
    net.SetOption(2, 0xffff, 0x2000, 1);
    auto connectResult = net.Connect(2, 0x7f000001, port(refused), 0, connectKey);
    assert(connectResult.error == 0x122f || connectResult.error == 0x122d);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        assert(!net.GetOption(2, 0xffff, 0x1007, value).error);
        if (value) break;
        std::this_thread::sleep_for(2ms);
    }
    assert(value == 0x122d);
    assert(net.Connect(2, 0x7f000001, port(refused), 0, connectKey).error == 0x122d);
    close(refused);
    net.Reset();
    assert(net.Receive(1, data, sizeof(data), 0, ip, remotePort, 0, readKey).error == 0x1214);
    puts(
        "PASS: nonblocking host TCP/UDP, asynchronous DNS, select, peek, EOF, timeout, options and "
        "resource cleanup");
}
