#include "NetworkSimulation.h"
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "CPU.h"
#include "pace.h"
namespace {
constexpr uint16_t param = 0x1204, notOpen = 0x1202, noSockets = 0x1205, badSocket = 0x1208,
                   notConnected = 0x120b, noInterfaces = 0x120c, unimplemented = 0x120e,
                   timedOut = 0x1212, alreadyConnected = 0x1213, wouldBlock = 0x122f,
                   noName = 0x123e;
constexpr const char* modeNames[] = {"disabled", "connected", "timeout", "disconnected",
                                     "internet", "wireless"};
class Guest {
   public:
    ArmCpu* cpu;
    uint32_t sp;
    void bytes(uint32_t address, void* data, size_t size, bool write = false) {
        if ((!address && size) || size > UINT32_MAX - address)
            throw std::runtime_error("guest address");
        for (size_t i = 0; i < size; i++)
            if (!cpuMemOpExternal(cpu, static_cast<uint8_t*>(data) + i, address + i, 1, write))
                throw std::runtime_error("guest memory");
    }
    uint32_t read(uint32_t address, unsigned size) {
        uint8_t b[4]{};
        bytes(address, b, size);
        uint32_t value = 0;
        for (unsigned i = 0; i < size; i++) value = (value << 8) | b[i];
        return value;
    }
    void put(uint32_t address, uint32_t value, unsigned size) {
        uint8_t b[4]{};
        for (unsigned i = 0; i < size; i++) b[size - 1 - i] = value >> (8 * i);
        bytes(address, b, size, true);
    }
    uint32_t arg(unsigned offset, unsigned size = 4) {
        if (sp > UINT32_MAX - offset) throw std::runtime_error("stack");
        return read(sp + offset, size);
    }
    std::string text(uint32_t address) {
        std::string value;
        for (unsigned i = 0; i < 256; i++) {
            auto c = read(address + i, 1);
            if (!c) return value;
            value += char(c);
        }
        throw std::runtime_error("guest string");
    }
    void writable(uint32_t address, size_t size) {
        if (!size) return;
        if (!address || size > UINT32_MAX - address) throw std::runtime_error("guest output");
        for (size_t i = 0; i < size;) {
            auto value = read(address + i, 1);
            put(address + i, value, 1);
            i += std::min<size_t>(size - i, 4096 - ((address + i) & 4095));
        }
        auto value = read(address + size - 1, 1);
        put(address + size - 1, value, 1);
    }
    void address(uint32_t p, uint32_t& ip, uint16_t& port) {
        if (read(p, 2) != 2) throw std::runtime_error("address family");
        port = read(p + 2, 2);
        ip = read(p + 4, 4);
    }
    void putAddress(uint32_t p, uint32_t ip, uint16_t port) {
        put(p, 2, 2);
        put(p + 2, port, 2);
        put(p + 4, ip, 4);
    }
    void yield() {
        // The suspended caller owns these 32 bytes below its original SP. Palm
        // OS delays only this task. On return, the 68k trampoline restores SP
        // and retries the original NetLib trap with its arguments untouched.
        if (sp < 32 || (sp & 1)) throw std::runtime_error("network wait stack");
        const auto pc = paceGetPC();
        if (pc < 4) throw std::runtime_error("network wait PC");
        const auto frame = sp - 32;
        writable(frame, 32);
        put(frame, 1, 4);           // SysTaskDelay(1 tick)
        put(frame + 4, 0x4e4f, 2);  // trap #15
        put(frame + 6, 0xa0a0, 2);  // SysTaskDelay
        put(frame + 8, 0x4fef, 2);
        put(frame + 10, 32, 2);  // lea 32(sp),sp
        put(frame + 12, 0x4ef9, 2);
        put(frame + 14, pc - 4, 4);  // jmp original trap
        cpuInvalidateCodeCache(cpu);
        paceSetAreg(7, frame);
        paceSetPC(frame + 4);
    }
};
}  // namespace
NetworkSimulation::NetworkSimulation() = default;
NetworkSimulation::~NetworkSimulation() = default;
const char* NetworkSimulation::GetModeName() const { return modeNames[mode]; }
bool NetworkSimulation::SetMode(const std::string& name) {
    for (uint32_t i = 0; i < 6; i++)
        if (name == modeNames[i]) {
            if (mode != i) {
                Reset();
                mode = i;
            }
            return true;
        }
    return false;
}
void NetworkSimulation::Reset() {
    opens = 0;
    sockets = {};
    if (!checkpoints) host.Reset();
}
void NetworkSimulation::Load(SavestateLoader<ChunkType>& loader) {
    mode = disabled;
    Reset();
    if (!loader.HasChunk(ChunkType::networkSimulation)) return;
    uint32_t version;
    auto* chunk = loader.GetChunkOrFail(ChunkType::networkSimulation, 1, "network", version);
    if (!chunk) return;
    LoadChunkHelper helper(*chunk);
    Transfer(helper, version);
    bool valid = mode <= wireless && opens <= 65535 && (mode != wireless || !opens);
    for (const auto& s : sockets) {
        valid &= s.used <= 1 && s.connected <= 1 && s.size <= s.bytes.size() &&
                 (s.type == 1 || s.type == 2) && (s.used || (!s.connected && !s.size));
        if (!opens || mode == disabled || mode == disconnected) valid &= !s.used;
    }
    if (!valid) {
        mode = disabled;
        Reset();
        loader.NotifyError();
        return;
    }
    // Host TCP/UDP connections cannot survive snapshot restore. Retain occupied
    // Palm references as closed handles until the app closes/reopens them.
    if (mode == internet && !checkpoints)
        for (auto& s : sockets) {
            s.connected = 0;
            s.size = 0;
            s.bytes = {};
        }
}
bool NetworkSimulation::Handle(ArmCpu* cpu, uint16_t trap) {
    // Virtual Wi-Fi leaves Net.lib lookups and real references to Palm's NetLib.
    // A reference this fixture handed out earlier stays closed, like out of range.
    if (mode == wireless && trap != 0xa0b8 && trap != 0xa0b9 && (trap < 0xa800 || trap > 0xa8ff))
        return false;
    Guest g{cpu, paceGetAreg(7)};
    uint32_t errPtr = 0;
    const bool pointerResult = trap == 0xa81b || trap == 0xa805;
    const HostNetwork::Key key{g.sp, paceGetPC(), trap};
    auto result = [&](int32_t value, uint16_t error = 0) {
        if (errPtr) g.put(errPtr, error, 2);
        paceSetDreg(0, value);
        if (pointerResult) paceSetAreg(0, error ? 0 : uint32_t(value));
        return true;
    };
    auto finish = [&](HostNetwork::Result r) {
        if (r.waiting) {
            g.yield();
            return true;
        }
        host.Complete(key);
        return result(r.value, r.error);
    };
    try {
        if (trap == 0xa0ba) {
            if (mode == disabled || mode == wireless || g.text(g.arg(0)) != "Net.lib")
                return false;
            g.put(g.arg(4), refnum, 2);
            return result(0);
        }
        if (trap == 0xa0b8 && g.arg(0, 2) == refnum) return result(opens ? 0x1203 : 0);
        if (trap == 0xa0b9 && g.arg(0, 2) == refnum) {
            paceSetAreg(0, 0);
            return result(0);
        }
        if (trap < 0xa800 || trap > 0xa8ff || g.arg(0, 2) != refnum) return false;
        if (trap == 0xa800) {
            paceSetAreg(0, 0);
            return result(0);
        }
        if (trap == 0xa805) {
            auto p = g.arg(6);
            g.writable(p, 16);
            in_addr ip{htonl(g.arg(2))};
            char text[16];
            if (!inet_ntop(AF_INET, &ip, text, sizeof(text))) return result(0, param);
            g.bytes(p, text, strlen(text) + 1, true);
            return result(p);
        }
        if (trap == 0xa806) {
            in_addr ip{};
            auto name = g.text(g.arg(2));
            return result(inet_pton(AF_INET, name.c_str(), &ip) == 1 ? ntohl(ip.s_addr)
                                                                     : UINT32_MAX);
        }
        if (trap == 0xa801 || trap == 0xa83b) {
            errPtr = g.arg(trap == 0xa801 ? 2 : 8);
            if (mode == internet && checkpoints) return result(0x1209, 0x1209);
            if (mode == disabled || mode == disconnected || mode == wireless)
                return result(noInterfaces, noInterfaces);
            if (opens == 65535) return result(0x1206, 0x1206);
            const auto previous = opens;
            result(previous ? 0x1201 : 0);
            ++opens;
            return true;
        }
        if (trap == 0xa802) {
            if (mode == internet && checkpoints) return result(0x1209);
            if (!opens) return result(notOpen);
            if (--opens) return result(0x1203);
            Reset();
            return result(0);
        }
        if (trap == 0xa82e) {
            g.put(g.arg(2), opens, 2);
            return result(0);
        }
        if (trap == 0xa803 || trap == 0xa804) return result(0);
        if (trap == 0xa833) {
            g.put(g.arg(4), opens ? 1 : 0, 1);
            g.put(g.arg(8), 0, 2);
            return result(opens ? 0 : notOpen);
        }
        if (trap == 0xa820) {
            if (g.arg(2, 2)) return result(0x121c);
            g.put(g.arg(4), 0x46726e64, 4);
            g.put(g.arg(8), 0, 2);
            return result(0);
        }
        if (trap == 0xa823 || trap == 0xa824) return result(opens ? 0 : notOpen);
        if (trap == 0xa81c) {
            const auto setting = g.arg(2, 2), p = g.arg(4), length = g.arg(8);
            uint32_t value = 0;
            unsigned size = 4;
            if (setting == 4)
                value = 0x46726e64;
            else if (setting == 5)
                size = 2;
            else if (setting != 1 && setting != 2 && setting != 3)
                return result(0x1218);
            if (g.read(length, 2) < size) {
                g.put(length, size, 2);
                return result(0x120d);
            }
            g.put(p, value, size);
            g.put(length, size, 2);
            return result(0);
        }
        switch (trap) {
            case 0xa807:
                errPtr = g.arg(12);
                break;
            case 0xa808:
                errPtr = g.arg(8);
                break;
            case 0xa809:
                errPtr = g.arg(18);
                break;
            case 0xa80a:
                errPtr = g.arg(20);
                break;
            case 0xa80b:
            case 0xa80c:
                errPtr = g.arg(14);
                break;
            case 0xa80f:
                errPtr = g.arg(10);
                break;
            case 0xa811:
                errPtr = g.arg(22);
                break;
            case 0xa813:
                errPtr = g.arg(24);
                break;
            case 0xa815:
                errPtr = g.arg(20);
                break;
            case 0xa81b:
                errPtr = g.arg(14);
                break;
            case 0xa829:
                errPtr = g.arg(24);
                break;
            default:
                paceSetAreg(0, 0);
                return result(-1, unimplemented);
        }
        if (errPtr) {
            auto value = g.read(errPtr, 2);
            g.put(errPtr, value, 2);
        }
        if (!opens || mode == disabled || mode == disconnected || mode == wireless)
            return result(pointerResult ? 0 : -1, notOpen);
        if (mode == internet && checkpoints) return result(pointerResult ? 0 : -1, wouldBlock);
        if (trap == 0xa81b) {
            const auto name = g.text(g.arg(2));
            const auto b = g.arg(6);
            uint32_t ip = 0x0a000002;
            if (mode == internet) {
                g.writable(b, 568);
                auto r = host.Resolve(name, ip, int32_t(g.arg(10)), key);
                if (r.waiting) {
                    g.yield();
                    return true;
                }
                if (r.error != wouldBlock) host.Complete(key);
                if (r.error) return result(0, r.error);
            } else if (name != "frond.test" && name != "10.0.0.2")
                return result(0, noName);
            std::array<uint8_t, 568> data{};
            g.bytes(b, data.data(), data.size(), true);
            g.put(b, b + 16, 4);
            g.put(b + 4, b + 272, 4);
            g.put(b + 8, 2, 2);
            g.put(b + 10, 4, 2);
            g.put(b + 12, b + 536, 4);
            g.put(b + 536, b + 552, 4);
            g.put(b + 552, ip, 4);
            const std::string canonical = mode == internet ? name : "frond.test";
            g.bytes(b + 16, const_cast<char*>(canonical.c_str()), canonical.size() + 1, true);
            return result(b);
        }
        if (trap == 0xa807) {
            const auto type = g.arg(4, 1), protocol = g.arg(6, 2);
            if (g.arg(2, 1) != 2 || (type != 1 && (mode != internet || type != 2)) ||
                (protocol && protocol != (type == 1 ? 6 : 17)))
                return result(-1, param);
            for (unsigned i = 0; i < sockets.size(); i++)
                if (!sockets[i].used) {
                    if (mode == internet) {
                        auto r = host.Open(i, type, protocol);
                        if (r.error) return result(-1, r.error);
                    }
                    result(i + 4);
                    sockets[i].used = 1;
                    sockets[i].type = type;
                    return true;
                }
            return result(-1, noSockets);
        }
        if (trap == 0xa815) {
            const auto width = g.arg(2, 2);
            if (width > 32) return result(-1, param);
            uint32_t ptr[3], masks[3]{};
            unsigned ready = 0;
            for (unsigned j = 0; j < 3; j++) {
                ptr[j] = g.arg(4 + j * 4);
                if (ptr[j]) {
                    g.writable(ptr[j], 4);
                    masks[j] = g.read(ptr[j], 4);
                }
            }
            if (mode == internet) {
                auto r = host.Select(width, masks, int32_t(g.arg(16)), key);
                if (r.waiting) {
                    g.yield();
                    return true;
                }
                host.Complete(key);
                if (r.error) return result(-1, r.error);
                ready = r.value;
            } else
                for (unsigned j = 0; j < 3; j++) {
                    const auto wanted = masks[j];
                    masks[j] = 0;
                    for (unsigned i = 0; i < sockets.size(); i++) {
                        const auto bit = 1u << (i + 4);
                        if (i + 4 >= width || !(wanted & bit)) continue;
                        const auto& s = sockets[i];
                        if (!s.used) return result(-1, badSocket);
                        if ((j == 0 && s.size) ||
                            (j == 1 && s.connected && s.size < s.bytes.size())) {
                            masks[j] |= bit;
                            ++ready;
                        }
                    }
                }
            for (unsigned j = 0; j < 3; j++)
                if (ptr[j]) g.put(ptr[j], masks[j], 4);
            return result(ready);
        }
        const auto id = g.arg(2, 2);
        if (id < 4 || id >= 20 || !sockets[id - 4].used) return result(-1, badSocket);
        auto& s = sockets[id - 4];
        if (trap == 0xa808) {
            if (mode == internet) host.Close(id - 4);
            result(0);
            s = Socket{};
            return true;
        }
        if (trap == 0xa80c || trap == 0xa80b) {
            if (trap == 0xa80c && s.connected) return result(-1, alreadyConnected);
            if (g.arg(8, 2) < 8) return result(-1, param);
            uint32_t ip;
            uint16_t port;
            g.address(g.arg(4), ip, port);
            if (mode == internet) {
                auto r = trap == 0xa80c ? host.Connect(id - 4, ip, port, int32_t(g.arg(10)), key)
                                        : host.Bind(id - 4, ip, port);
                if (!r.waiting && !r.error && trap == 0xa80c) s.connected = 1;
                return finish(r);
            }
            if (trap == 0xa80b) return result(-1, unimplemented);
            if (mode == timeout) return result(-1, timedOut);
            if (port != 7 || ip != 0x0a000002) return result(-1, noInterfaces);
            result(0);
            s.connected = 1;
            return true;
        }
        if (mode == internet) {
            if (trap == 0xa809) {
                const auto length = g.arg(12, 2);
                if (length != 1 && length != 2 && length != 4) return result(-1, param);
                return finish(
                    host.SetOption(id - 4, g.arg(4, 2), g.arg(6, 2), g.read(g.arg(8), length)));
            }
            if (trap == 0xa80a) {
                const auto length = g.arg(12), p = g.arg(8);
                if (g.read(length, 2) < 4) {
                    g.put(length, 4, 2);
                    return result(-1, 0x120d);
                }
                g.writable(p, 4);
                g.writable(length, 2);
                uint32_t value;
                auto r = host.GetOption(id - 4, g.arg(4, 2), g.arg(6, 2), value);
                if (!r.error) {
                    g.put(p, value, 4);
                    g.put(length, 4, 2);
                }
                return finish(r);
            }
            if (trap == 0xa80f) return finish(host.Shutdown(id - 4, g.arg(4, 2)));
            if (trap == 0xa829) {
                for (unsigned j = 0; j < 2; j++) {
                    const auto p = g.arg(4 + j * 8), length = g.arg(8 + j * 8);
                    if (!p) continue;
                    if (!length || g.read(length, 2) < 8) return result(-1, 0x120d);
                    g.writable(p, 8);
                    g.writable(length, 2);
                }
                for (unsigned j = 0; j < 2; j++) {
                    const auto p = g.arg(4 + j * 8), length = g.arg(8 + j * 8);
                    if (!p) continue;
                    uint32_t ip;
                    uint16_t port;
                    auto r = host.Address(id - 4, j, ip, port);
                    if (r.error) return finish(r);
                    g.putAddress(p, ip, port);
                    g.put(length, 8, 2);
                }
                return result(0);
            }
            if (trap == 0xa811 || trap == 0xa813) {
                if (s.type == 2 && g.arg(8, 2) > 32767) return result(-1, 0x120a);
                const auto p = g.arg(4), length = std::min<uint32_t>(g.arg(8, 2), 32767),
                           flags = g.arg(10, 2), peer = g.arg(12);
                std::vector<uint8_t> data(length);
                if (trap == 0xa811) {
                    uint32_t ip = 0;
                    uint16_t port = 0;
                    if (peer) {
                        if (g.arg(16, 2) < 8) return result(-1, param);
                        g.address(peer, ip, port);
                    }
                    g.bytes(p, data.data(), length);
                    return finish(host.Send(id - 4, data.data(), length, flags, peer, ip, port,
                                            int32_t(g.arg(18)), key));
                }
                const auto peerLength = g.arg(16);
                g.writable(p, length);
                if (peer) {
                    if (!peerLength || g.read(peerLength, 2) < 8) return result(-1, 0x120d);
                    g.writable(peer, 8);
                    g.writable(peerLength, 2);
                }
                uint32_t ip;
                uint16_t port;
                auto r = host.Receive(id - 4, data.data(), length, flags, ip, port,
                                      int32_t(g.arg(20)), key);
                if (!r.waiting && !r.error) {
                    g.bytes(p, data.data(), r.value, true);
                    if (peer) {
                        g.putAddress(peer, ip, port);
                        g.put(peerLength, 8, 2);
                    }
                }
                return finish(r);
            }
            return result(-1, unimplemented);
        }
        if (trap != 0xa811 && trap != 0xa813) return result(-1, unimplemented);
        if (!s.connected) return result(-1, notConnected);
        const auto buffer = g.arg(4), length = g.arg(8, 2);
        if (g.arg(10, 2) || g.arg(12)) return result(-1, unimplemented);
        if (!length) return result(0);
        if (trap == 0xa811) {
            const auto count = std::min<size_t>(length, s.bytes.size() - s.size);
            if (!count) return result(-1, g.arg(18) ? timedOut : wouldBlock);
            std::array<uint8_t, 4096> data{};
            g.bytes(buffer, data.data(), count);
            result(count);
            std::copy_n(data.data(), count, s.bytes.data() + s.size);
            s.size += count;
            return true;
        }
        if (!s.size) return result(-1, g.arg(20) ? timedOut : wouldBlock);
        const auto count = std::min<uint32_t>(length, s.size);
        g.bytes(buffer, s.bytes.data(), count, true);
        result(count);
        std::memmove(s.bytes.data(), s.bytes.data() + count, s.size - count);
        s.size -= count;
        std::fill(s.bytes.begin() + s.size, s.bytes.end(), 0);
        return true;
    } catch (const std::exception&) {
        if (!checkpoints) host.Complete(key);
        paceSetDreg(0, pointerResult ? 0 : UINT32_MAX);
        if (pointerResult) paceSetAreg(0, 0);
        try {
            if (errPtr) g.put(errPtr, param, 2);
        } catch (...) {
        }
        return trap != 0xa0ba;
    }
}
#endif
