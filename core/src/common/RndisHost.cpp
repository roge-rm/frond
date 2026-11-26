#include "RndisHost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
std::vector<uint8_t> header(uint32_t type, uint32_t length) {
    std::vector<uint8_t> m;
    m.reserve(length);
    put32(m, type);
    put32(m, length);
    return m;
}
constexpr unsigned maxAttempts = 3;
constexpr size_t maxQueued = 64;
constexpr size_t packetHeader = 44;

bool completeMessageSequence(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    while (data.size() - pos >= 8) {
        const auto* message = data.data() + pos;
        const uint32_t length = get32(message + 4);
        if (get32(message) != RndisHost::packetMsg || length < packetHeader || length > data.size() - pos)
            return false;
        pos += length;
    }
    return pos == data.size();
}
}  // namespace

std::vector<uint8_t> RndisHost::PacketMessage(const uint8_t* frame, size_t size) {
    auto m = header(packetMsg, uint32_t(packetHeader + size));
    put32(m, packetHeader - 8);  // DataOffset, from the RequestId field
    put32(m, uint32_t(size));
    for (int i = 0; i < 7; ++i) put32(m, 0);  // OOB, per-packet info, VcHandle, reserved
    m.insert(m.end(), frame, frame + size);
    return m;
}

std::vector<uint8_t> RndisHost::InitMessage(uint32_t id) {
    auto m = header(initMsg, 24);
    put32(m, id);
    put32(m, 1);  // RNDIS 1.0
    put32(m, 0);
    put32(m, maxTransfer);
    return m;
}

std::vector<uint8_t> RndisHost::QueryMessage(uint32_t id, uint32_t oid, uint32_t bufferLength) {
    // The buffer is zeros the size of the answer; WM5 refuses a query without one (NDIS_STATUS_INVALID_LENGTH).
    auto m = header(queryMsg, 28 + bufferLength);
    put32(m, id);
    put32(m, oid);
    put32(m, bufferLength);  // InformationBufferLength
    put32(m, 20);            // InformationBufferOffset, from the RequestId field
    put32(m, 0);             // DeviceVcHandle
    m.resize(28 + bufferLength);
    return m;
}

std::vector<uint8_t> RndisHost::SetMessage(uint32_t id, uint32_t oid, const uint8_t* data, size_t size) {
    auto m = header(setMsg, uint32_t(28 + size));
    put32(m, id);
    put32(m, oid);
    put32(m, uint32_t(size));
    put32(m, 20);
    put32(m, 0);
    m.insert(m.end(), data, data + size);
    return m;
}

std::vector<uint8_t> RndisHost::KeepaliveMessage(uint32_t id) {
    auto m = header(keepaliveMsg, 12);
    put32(m, id);
    return m;
}

std::vector<uint8_t> RndisHost::KeepaliveComplete(uint32_t id) {
    auto m = header(keepaliveMsg | completion, 16);
    put32(m, id);
    put32(m, 0);  // RNDIS_STATUS_SUCCESS
    return m;
}

void RndisHost::Message(const std::string& text) {
    if (messages.size() < 64) messages.push_back("rndis: " + text);
}

void RndisHost::Start(unsigned inPacketSize, uint64_t now) {
    Stop();
    inPacket = inPacketSize ? inPacketSize : 64;
    state = State::init;
    step = 0;
    Next(now);
}

void RndisHost::Stop() {
    const auto kept = std::move(messages);
    const auto c = counters;
    auto handler = std::move(frameFromDevice);
    *this = RndisHost{};
    messages = kept;
    counters = c;
    frameFromDevice = std::move(handler);
}

void RndisHost::Fail(const std::string& why) {
    Message(why);
    commands.clear();
    awaiting = notified = false;
    if (++attempts > maxAttempts) {
        state = State::failed;
        Message("gave up");
        return;
    }
    // Start again from INIT.
    state = State::init;
}

// The bring-up sequence: each step sends one command and waits for its completion.
void RndisHost::Next(uint64_t now) {
    if (state == State::init) {
        Send(InitMessage(++requestId), now);
    } else if (state == State::query) {
        if (step == 1) Send(QueryMessage(++requestId, oidPermanentAddress, 6), now);
        else if (step == 2) Send(QueryMessage(++requestId, oidMaximumFrameSize, 4), now);
        else if (step == 3) {
            uint8_t filter[4] = {uint8_t(packetFilter), uint8_t(packetFilter >> 8), uint8_t(packetFilter >> 16),
                                 uint8_t(packetFilter >> 24)};
            Send(SetMessage(++requestId, oidCurrentPacketFilter, filter, 4), now);
        }
    }
}

void RndisHost::Send(std::vector<uint8_t> message, uint64_t, bool wantsReply) {
    // Replies to the device's own messages go first and wait for nothing; a command waits for its completion.
    if (!wantsReply) commands.push_front(std::move(message));
    else commands.push_back(std::move(message));
}

bool RndisHost::NextCommand(std::vector<uint8_t>& message, uint64_t now) {
    if (state == State::idle || state == State::failed || commandOut) return false;
    if (state == State::init && commands.empty() && !awaiting) Next(now);
    if (Up() && commands.empty() && !awaiting && now >= keepaliveAt) {
        Send(KeepaliveMessage(++requestId), now);
        ++counters.keepalives;
    }
    if (commands.empty()) return false;
    const bool reply = get32(commands.front().data()) != (keepaliveMsg | completion);
    if (reply && awaiting) return false;
    message = std::move(commands.front());
    commands.pop_front();
    commandOut = true;
    if (reply) {
        waitingId = get32(message.data() + 8);
        awaiting = true;
        notified = false;
        sentAt = now;
        lastHeard = now;
    }
    ++counters.commands;
    return true;
}

void RndisHost::CommandSent(bool ok, uint64_t now) {
    commandOut = false;
    if (!ok) Fail("SEND_ENCAPSULATED_COMMAND failed");
    else if (awaiting) sentAt = now;
}

bool RndisHost::WantResponse(uint64_t now) const {
    if (commandOut || state == State::idle || state == State::failed) return false;
    return notified || (awaiting && now >= sentAt + responseWaitNs);
}

uint64_t RndisHost::NextWake() const {
    if (state == State::idle || state == State::failed || commandOut) return UINT64_MAX;
    if (notified) return 0;
    if (!commands.empty() && !awaiting) return 0;
    if (state == State::init && !awaiting) return 0;
    if (awaiting) return std::min(sentAt + responseWaitNs, lastHeard + commandTimeoutNs);
    return Up() ? keepaliveAt : UINT64_MAX;
}

void RndisHost::Notification(const uint8_t* data, size_t size, uint64_t) {
    // RESPONSE_AVAILABLE: 0x00000001 then a reserved word.
    if (size >= 4 && get32(data) == 1) notified = true;
}

void RndisHost::Response(bool ok, const uint8_t* data, size_t size, uint64_t now) {
    notified = false;
    if (!ok || size < 8) {
        // Nothing yet (a STALL or a lone zero byte): ask again a little later, until the command times out.
        if (awaiting) {
            if (now >= lastHeard + commandTimeoutNs) Fail("no completion");
            else sentAt = now;
        }
        return;
    }
    ++counters.responses;
    const bool waited = awaiting;
    HandleControl(data, size, now);
    // Something other than the awaited completion (a device message or a late one): wait before asking
    // again, and start again once the command has gone unanswered too long.
    if (waited && awaiting) {
        if (now >= lastHeard + commandTimeoutNs) Fail("no completion");
        else sentAt = now;
    }
}

void RndisHost::HandleControl(const uint8_t* m, size_t size, uint64_t now) {
    const uint32_t type = get32(m), length = get32(m + 4);
    if (length < 8 || length > size) {
        ++counters.badMessages;
        return;
    }
    if (type == keepaliveMsg && length >= 12) {
        Send(KeepaliveComplete(get32(m + 8)), now, false);
        return;
    }
    if (type == indicateStatusMsg && length >= 12) {
        char text[64];
        std::snprintf(text, sizeof(text), "status %08x", get32(m + 8));
        Message(text);
        return;
    }
    if (!(type & completion) || length < 16) {
        ++counters.badMessages;
        return;
    }
    if (!awaiting || get32(m + 8) != waitingId) return;  // a late completion
    awaiting = false;
    lastHeard = now;
    if (Up()) keepaliveAt = now + keepaliveNs;
    const uint32_t status = get32(m + 12);
    char text[96];
    if (status) {
        std::snprintf(text, sizeof(text), "message %08x failed, status %08x", type & ~completion, status);
        if (type == (keepaliveMsg | completion)) return Fail(text);
        if (state == State::query && step == 2) {  // the frame size query is optional
            ++step;
            return Next(now);
        }
        return Fail(text);
    }
    switch (type & ~completion) {
        case initMsg: {
            if (state != State::init || length < 44) return Fail("short INIT completion");
            std::snprintf(text, sizeof(text), "INIT done, RNDIS %u.%u, medium %u, %u packets and %u bytes a transfer",
                          get32(m + 16), get32(m + 20), get32(m + 28), get32(m + 32), get32(m + 36));
            Message(text);
            state = State::query;
            step = 1;
            return Next(now);
        }
        case queryMsg: {
            const bool optional = step == 2;  // the frame size query
            if (length < 24) {
                if (optional) {
                    ++step;
                    return Next(now);
                }
                return Fail("short QUERY completion");
            }
            const uint32_t n = get32(m + 16), offset = get32(m + 20);
            if (8ull + offset + n > length) return Fail("bad QUERY completion");
            const uint8_t* info = m + 8 + offset;
            if (step == 1) {
                if (n < 6) return Fail("no MAC address");
                std::memcpy(mac, info, 6);
            } else if (step == 2 && n >= 4) {
                // OID_GEN_MAXIMUM_FRAME_SIZE excludes the Ethernet header.
                maxFrame = std::min(get32(info), 1500u) + 14;
            }
            ++step;
            return Next(now);
        }
        case setMsg:
            if (step != 3) return;
            state = State::up;
            attempts = 0;
            ++counters.ups;
            keepaliveAt = now + keepaliveNs;
            std::snprintf(text, sizeof(text), "up, MAC %02x:%02x:%02x:%02x:%02x:%02x, frames up to %u bytes", mac[0],
                          mac[1], mac[2], mac[3], mac[4], mac[5], maxFrame);
            Message(text);
            return;
        case keepaliveMsg:
            return;
    }
}

void RndisHost::BulkInPacket(const uint8_t* data, size_t size) {
    // A transfer longer than INIT allowed is dropped up to its short packet.
    if (inDiscard || inTransfer.size() + size > maxTransfer) {
        if (!inDiscard) ++counters.badMessages;
        inTransfer.clear();
        inDiscard = size >= inPacket;
        return;
    }
    if (size) inTransfer.insert(inTransfer.end(), data, data + size);
    // A device can end a complete message sequence on a full packet without padding.
    if (size < inPacket || completeMessageSequence(inTransfer)) {
        BulkInTransfer(inTransfer.data(), inTransfer.size());
        inTransfer.clear();
    }
}

void RndisHost::BulkInTransfer(const uint8_t* data, size_t size) {
    for (size_t pos = 0; size - pos >= 8;) {
        const uint8_t* m = data + pos;
        const uint32_t type = get32(m), length = get32(m + 4);
        if (!type && !length) return;  // padding
        if (type != packetMsg || length < packetHeader || length > size - pos) {
            ++counters.badMessages;
            return;
        }
        const uint32_t offset = get32(m + 8), n = get32(m + 12);
        if (offset < packetHeader - 8 || 8ull + offset + n > length) {
            ++counters.badMessages;
            return;
        }
        ++counters.framesIn;
        if (Up() && frameFromDevice) frameFromDevice(m + 8 + offset, n);
        pos += length;
    }
}

void RndisHost::FrameToDevice(const uint8_t* frame, size_t size) {
    if (!Up() || outQueue.size() >= maxQueued || size < 14 || size > maxFrame) {
        ++counters.dropped;
        return;
    }
    outQueue.push_back(PacketMessage(frame, size));
}

bool RndisHost::BulkOutPacket(uint8_t* data, unsigned max, unsigned& size) {
    if (outZlp) {
        outZlp = false;
        size = 0;
        return true;
    }
    if (outOffset >= outTransfer.size()) {
        if (outQueue.empty() || !max) return false;
        outTransfer = std::move(outQueue.front());
        outQueue.pop_front();
        outOffset = 0;
        ++counters.framesOut;
    }
    const size_t n = std::min<size_t>(max, outTransfer.size() - outOffset);
    std::memcpy(data, outTransfer.data() + outOffset, n);
    outOffset += n;
    size = unsigned(n);
    // A transfer that ends on a full packet needs a zero-length packet to end it.
    if (outOffset >= outTransfer.size() && n == max) outZlp = true;
    return true;
}
