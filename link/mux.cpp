#include "mux.h"

#include <algorithm>

namespace frond::link {

size_t Mux::windowFor(ChannelKind kind) {
    switch (kind) {
        case ChannelKind::Serial: return kSerialWindow;
        case ChannelKind::Bluetooth: return kBluetoothWindow;
        default: return 0;
    }
}

Mux::Channel* Mux::find(uint16_t ch) {
    auto it = channels_.find(ch);
    return it == channels_.end() ? nullptr : &it->second;
}

const Mux::Channel* Mux::find(uint16_t ch) const {
    auto it = channels_.find(ch);
    return it == channels_.end() ? nullptr : &it->second;
}

void Mux::event(ChannelEvent::Kind kind, const Channel& c) {
    ChannelEvent e;
    e.kind = kind;
    e.channel = c.id;
    e.channelKind = c.kind;
    events.push_back(std::move(e));
}

void Mux::makeOpen(Channel& c) {
    c.state = State::Open;
    c.sendCredit = windowFor(c.kind);
    c.allowance = windowFor(c.kind);
}

uint16_t Mux::open(ChannelKind kind, const std::string& machine, const std::string& device) {
    if (channels_.size() >= kMaxChannels) return 0;
    uint16_t id = nextId_;
    while (id == 0 || channels_.count(id)) id = uint16_t(id + 2);
    nextId_ = uint16_t(id + 2);
    Channel c{id, kind, State::Opening, {}};
    channels_.emplace(id, std::move(c));
    control_.push_back({MsgType::Open, id, encodeOpen({kind, machine, device})});
    return id;
}

bool Mux::accept(uint16_t ch, const std::string& machine, const std::string& device) {
    Channel* c = find(ch);
    if (!c || (c->state != State::Requested && c->state != State::Holding)) return false;
    makeOpen(*c);
    control_.push_back({MsgType::Accept, ch, encodeMachine({machine, device})});
    return true;
}

bool Mux::waiting(uint16_t ch) {
    Channel* c = find(ch);
    if (!c || c->state != State::Requested) return false;
    c->state = State::Holding;
    control_.push_back({MsgType::Waiting, ch, {}});
    return true;
}

bool Mux::busy(uint16_t ch, const std::string& machine, const std::string& device) {
    Channel* c = find(ch);
    if (!c || (c->state != State::Requested && c->state != State::Holding)) return false;
    channels_.erase(ch);
    control_.push_back({MsgType::Busy, ch, encodeMachine({machine, device})});
    return true;
}

void Mux::close(uint16_t ch, CloseReason reason) {
    if (!channels_.erase(ch)) return;
    control_.push_back({MsgType::Close, ch, Bytes{uint8_t(reason)}});
}

void Mux::sendState(uint16_t ch, PeerState state) {
    if (ch != 0 && !find(ch)) return;
    control_.push_back({MsgType::State, ch, Bytes{uint8_t(state)}});
}

size_t Mux::send(uint16_t ch, const uint8_t* p, size_t n) {
    Channel* c = find(ch);
    if (c && c->queueAt > 0) {
        // Drop the sent prefix so a queue that never fully drains can't grow.
        c->queue.erase(c->queue.begin(), c->queue.begin() + long(c->queueAt));
        c->queueAt = 0;
    }
    if (c && c->kind == ChannelKind::Infrared) {
        // A Palm out of range: bytes that can't go now are lost, as over real IR.
        if (c->state != State::Open || c->pending() + n > kInfraredQueue) {
            irDropped_ += n;
            return 0;
        }
        putBytes(c->queue, p, n);
        return n;
    }
    if (!c || c->state != State::Open) return 0;
    size_t take = std::min(n, windowFor(c->kind) - std::min(windowFor(c->kind), c->pending()));
    putBytes(c->queue, p, take);
    return take;
}

size_t Mux::writable(uint16_t ch) const {
    const Channel* c = find(ch);
    if (!c || c->state != State::Open) return 0;
    size_t cap = c->kind == ChannelKind::Infrared ? kInfraredQueue : windowFor(c->kind);
    return cap - std::min(cap, c->pending());
}

size_t Mux::queued(uint16_t ch) const {
    const Channel* c = find(ch);
    return c ? c->pending() : 0;
}

bool Mux::isOpen(uint16_t ch) const {
    const Channel* c = find(ch);
    return c && c->state == State::Open;
}

void Mux::consumed(uint16_t ch, size_t n) {
    Channel* c = find(ch);
    if (!c || c->state != State::Open || c->kind == ChannelKind::Infrared) return;
    n = std::min(n, c->unconsumed);
    c->unconsumed -= n;
    c->pendingCredit += n;
    // Batch credit, but never sit on it once our side has taken everything.
    if (c->pendingCredit > 0 && (c->pendingCredit >= windowFor(c->kind) / 4 || c->unconsumed == 0)) {
        Bytes b;
        putU32(b, uint32_t(c->pendingCredit));
        control_.push_back({MsgType::Credit, ch, std::move(b)});
        c->allowance += c->pendingCredit;
        c->pendingCredit = 0;
    }
}

bool Mux::receive(const Message& m) {
    Channel* c = find(m.channel);
    switch (m.type) {
        case MsgType::Open: {
            OpenBody o;
            if (!peerOwns(m.channel) || c || !decodeOpen(m.payload, o)) return false;
            if (channels_.size() >= kMaxChannels) {
                control_.push_back({MsgType::Busy, m.channel, encodeMachine({})});
                return true;
            }
            Channel nc{m.channel, o.kind, State::Requested, {}};
            channels_.emplace(m.channel, std::move(nc));
            ChannelEvent e;
            e.kind = ChannelEvent::Kind::OpenRequest;
            e.channel = m.channel;
            e.channelKind = o.kind;
            e.machine = o.machine;
            e.device = o.device;
            events.push_back(std::move(e));
            return true;
        }
        case MsgType::Accept:
        case MsgType::Busy: {
            MachineBody mb;
            if (!c) return true;  // closed here meanwhile
            if ((c->state != State::Opening && c->state != State::Waiting) || !decodeMachine(m.payload, mb))
                return false;
            ChannelEvent e;
            e.kind = m.type == MsgType::Accept ? ChannelEvent::Kind::Accepted : ChannelEvent::Kind::Busy;
            e.channel = c->id;
            e.channelKind = c->kind;
            e.machine = mb.machine;
            e.device = mb.device;
            if (m.type == MsgType::Accept)
                makeOpen(*c);
            else
                channels_.erase(m.channel);
            events.push_back(std::move(e));
            return true;
        }
        case MsgType::Waiting:
            if (!c) return true;
            if (c->state != State::Opening) return false;
            c->state = State::Waiting;
            event(ChannelEvent::Kind::Waiting, *c);
            return true;
        case MsgType::Data: {
            if (!c) return true;  // closed here meanwhile
            if (c->state != State::Open) return false;
            if (c->kind != ChannelKind::Infrared) {
                if (m.payload.size() > c->allowance) return false;  // peer ignored our window
                c->allowance -= m.payload.size();
                c->unconsumed += m.payload.size();
            }
            ChannelEvent e;
            e.kind = ChannelEvent::Kind::Data;
            e.channel = c->id;
            e.channelKind = c->kind;
            e.data = m.payload;
            events.push_back(std::move(e));
            return true;
        }
        case MsgType::Credit: {
            if (m.payload.size() != 4) return false;
            if (!c) return true;
            if (c->state != State::Open || c->kind == ChannelKind::Infrared) return false;
            c->sendCredit += getU32(m.payload.data());
            return c->sendCredit <= windowFor(c->kind);
        }
        case MsgType::Close: {
            if (!c) return true;
            ChannelEvent e;
            e.kind = ChannelEvent::Kind::Closed;
            e.channel = c->id;
            e.channelKind = c->kind;
            e.code = m.payload.empty() ? 0 : m.payload[0];
            channels_.erase(m.channel);
            events.push_back(std::move(e));
            return true;
        }
        case MsgType::State: {
            if (m.payload.size() != 1 || m.payload[0] > uint8_t(PeerState::Reset)) return false;
            if (m.channel != 0 && !c) return true;
            ChannelEvent e;
            e.kind = ChannelEvent::Kind::State;
            e.channel = m.channel;
            if (c) e.channelKind = c->kind;
            e.code = m.payload[0];
            events.push_back(std::move(e));
            return true;
        }
        default:
            return false;  // Hello, Ping, Pong belong to the link
    }
}

bool Mux::takeData(Channel& c, size_t max, Message& out) {
    size_t n = std::min(c.pending(), max);
    if (n == 0) return false;
    out.type = MsgType::Data;
    out.channel = c.id;
    out.payload.assign(c.queue.begin() + long(c.queueAt), c.queue.begin() + long(c.queueAt + n));
    c.queueAt += n;
    if (c.queueAt == c.queue.size()) {
        c.queue.clear();
        c.queueAt = 0;
    }
    return true;
}

bool Mux::next(Message& out) {
    if (!control_.empty()) {
        out = std::move(control_.front());
        control_.pop_front();
        return true;
    }
    for (auto& [id, c] : channels_)
        if (c.kind == ChannelKind::Infrared && c.state == State::Open && takeData(c, kInfraredQueue, out))
            return true;
    // Round robin over lossless channels with bytes and credit, starting after the last one served.
    for (int pass = 0; pass < 2; pass++) {
        auto it = pass == 0 ? channels_.upper_bound(lastServed_) : channels_.begin();
        auto end = pass == 0 ? channels_.end() : channels_.upper_bound(lastServed_);
        for (; it != end; ++it) {
            Channel& c = it->second;
            if (c.kind == ChannelKind::Infrared || c.state != State::Open || c.sendCredit == 0) continue;
            if (takeData(c, std::min(c.sendCredit, kMaxPayload), out)) {
                c.sendCredit -= out.payload.size();
                lastServed_ = c.id;
                return true;
            }
        }
    }
    return false;
}

bool Mux::hasOutput() const {
    if (!control_.empty()) return true;
    for (auto& [id, c] : channels_) {
        if (c.state != State::Open || c.pending() == 0) continue;
        if (c.kind == ChannelKind::Infrared || c.sendCredit > 0) return true;
    }
    return false;
}

void Mux::reset(CloseReason reason) {
    for (auto& [id, c] : channels_) {
        ChannelEvent e;
        e.kind = ChannelEvent::Kind::Closed;
        e.channel = id;
        e.channelKind = c.kind;
        e.code = uint8_t(reason);
        events.push_back(std::move(e));
    }
    channels_.clear();
    control_.clear();
}

}  // namespace frond::link
