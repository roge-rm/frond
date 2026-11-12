#include "IrLap.h"

#include <algorithm>

namespace frondIr {

namespace {

constexpr uint64_t microsecond = 1000ull, millisecond = 1000000ull, second = 1000000000ull;

// Control fields with the P/F bit set (IrLAP 1.1 section 5.2).
constexpr uint8_t pf = 0x10;
constexpr uint8_t snrm = 0x93, disc = 0x53, ui = 0x13, xidCommand = 0x3f, xidResponse = 0xbf, test = 0xf3,
                  ua = 0x73, dm = 0x1f;
constexpr uint8_t rr = 0x01, rnr = 0x05, rej = 0x09, srej = 0x0d;
constexpr uint8_t broadcast = 0xfe;

// Negotiation parameters (section 6.6), each a bit mask over these values.
constexpr uint8_t piBaud = 0x01, piMaxTurn = 0x82, piDataSize = 0x83, piWindow = 0x84, piBofs = 0x85,
                  piMinTurn = 0x86, piDisconnect = 0x08;
constexpr unsigned bauds[] = {2400, 9600, 19200, 38400, 57600, 115200, 576000, 1152000};
constexpr uint64_t maxTurns[] = {500 * millisecond, 250 * millisecond, 100 * millisecond, 50 * millisecond};
constexpr unsigned dataSizes[] = {64, 128, 256, 512, 1024, 2048};
constexpr unsigned bofCounts[] = {48, 24, 12, 5, 3, 2, 1, 0};
constexpr uint64_t minTurns[] = {10 * millisecond, 5 * millisecond, 1 * millisecond, 500 * microsecond,
                                 100 * microsecond, 50 * microsecond, 10 * microsecond, 0};
constexpr uint64_t disconnects[] = {3 * second, 8 * second, 12 * second, 16 * second,
                                    20 * second, 25 * second, 30 * second, 40 * second};
// Our own receive side, sent in the UA: 500 ms turnaround, up to 2048 bytes, a window of 7, no
// extra BOFs and no minimum turnaround.
constexpr uint8_t ourMaxTurn = 0x01, ourDataSize = 0x3f, ourWindow = 0x7f, ourBofs = 0x80, ourMinTurn = 0x80;

// Before a link is up: 10 extra BOFs at 9600, as IrLAP's defaults, and a short turnaround so a
// discovery reply lands inside its slot.
constexpr unsigned ndmBofs = 10;
constexpr uint64_t ndmTurn = 1 * millisecond;

// Index of the highest bit set within the table's size, or -1.
int Highest(unsigned mask, size_t size) {
    for (int bit = int(size) - 1; bit >= 0; --bit)
        if (mask & (1u << bit)) return bit;
    return -1;
}

const uint8_t* Find(const Bytes& frame, size_t start, uint8_t pi, uint8_t& length) {
    for (size_t i = start; i + 2 <= frame.size();) {
        const uint8_t id = frame[i], pl = frame[i + 1];
        if (i + 2 + pl > frame.size()) break;
        if (id == pi) {
            length = pl;
            return &frame[i + 2];
        }
        i += 2 + pl;
    }
    return nullptr;
}

unsigned Mask(const Bytes& frame, size_t start, uint8_t pi, unsigned fallback) {
    uint8_t length = 0;
    const uint8_t* value = Find(frame, start, pi, length);
    if (!value || !length) return fallback;
    // Multi-byte masks are little-endian; only the first byte matters for SIR speeds.
    return value[0];
}

}  // namespace

IrLap::IrLap(IrLapUser& user) : IrLap(user, Options{}) {}

IrLap::IrLap(IrLapUser& user, Options options) : user(user), options(std::move(options)) {}

void IrLap::Receive(const uint8_t* data, size_t size, uint64_t now) {
    for (size_t i = 0; i < size; ++i)
        if (unwrapper.Feed(data[i])) Frame(unwrapper.Frame(), now);
}

size_t IrLap::Read(uint8_t* data, size_t max, uint64_t now) {
    if (now < holdUntil) return 0;
    const size_t n = std::min(max, out.size() - outPos);
    std::copy(out.begin() + outPos, out.begin() + outPos + n, data);
    outPos += n;
    if (outPos == out.size()) {
        out.clear();
        outPos = 0;
    }
    return n;
}

size_t IrLap::Available(uint64_t now) const { return now < holdUntil ? 0 : out.size() - outPos; }

void IrLap::Service(uint64_t now) {
    // The watchdog: a primary that stops polling has gone.
    if (state == State::Nrm && now > lastHeard + link.disconnectNs) {
        ++stats.timeouts;
        Down(now);
    } else if (state == State::Nrm && options.idleNs && !dataSeen && now > lastData + options.idleNs) {
        ++stats.timeouts;
        Down(now);
    }
}

void IrLap::Drop(uint64_t now) {
    unwrapper.Reset();
    out.clear();
    outPos = 0;
    holdUntil = 0;
    ourSlot = lastSlot = -1;
    answered = false;
    if (state == State::Nrm) Down(now);
}

void IrLap::Send(Bytes info) { queue.push_back(std::move(info)); }

void IrLap::Frame(const Bytes& frame, uint64_t now) {
    if (frame.size() < 2) return;
    // Only commands: a response would come from another secondary.
    const uint8_t address = frame[0];
    if (!(address & 1)) return;
    ++stats.framesIn;
    const uint8_t control = frame[1], target = address & 0xfe;
    if (target == broadcast) {
        if (control == xidCommand || control == (xidCommand & ~pf)) {
            if (state == State::Ndm) Discovery(frame, now);
            return;
        }
        if (control == snrm) return Snrm(frame, now);
        if (control == test || control == (test & ~pf)) return Test(frame, broadcast, now);
        return;
    }
    if (state == State::Nrm && target == caddr) return Nrm(frame, now);
    // A command for a link we don't have: tell it so, if it asks for an answer.
    if (options.answerDm && state == State::Ndm && (control & pf) && control != ui) {
        holdUntil = std::max(holdUntil, now + ndmTurn);
        Emit({target, dm}, now);
    }
}

void IrLap::Discovery(const Bytes& frame, uint64_t now) {
    // Format, source, destination, flags, slot, version, then discovery information.
    if (frame.size() < 14 || frame[2] != 0x01) return;
    std::array<uint8_t, 4> source, target;
    std::copy(frame.begin() + 3, frame.begin() + 7, source.begin());
    std::copy(frame.begin() + 7, frame.begin() + 11, target.begin());
    if (target != std::array<uint8_t, 4>{{0xff, 0xff, 0xff, 0xff}} && target != options.address) return;
    const uint8_t flags = frame[11], slot = frame[12];
    static constexpr int slotCounts[] = {1, 6, 8, 16};
    const int slots = slotCounts[flags & 3];
    if (slot == 0xff) {
        // The closing frame carries the guest's hints and nickname.
        size_t i = 14;
        while (i < frame.size() && (frame[i] & 0x80)) ++i;
        i += 2;
        guestNickname.assign(frame.begin() + std::min(i, frame.size()), frame.end());
        lastSlot = -1;
        return;
    }
    if (source != guest || lastSlot < 0 || slot <= lastSlot) {
        ++stats.discoveries;
        guest = source;
        answered = false;
        // A slot that depends only on the two addresses, so runs repeat.
        unsigned mix = 0;
        for (const uint8_t b : source) mix = mix * 31 + b;
        for (const uint8_t b : options.address) mix = mix * 31 + b;
        ourSlot = int(mix % unsigned(slots));
    }
    lastSlot = slot;
    if (answered || slot < ourSlot) return;
    answered = true;
    ++stats.xidReplies;
    Bytes reply{broadcast, xidResponse, 0x01};
    reply.insert(reply.end(), options.address.begin(), options.address.end());
    reply.insert(reply.end(), source.begin(), source.end());
    reply.insert(reply.end(), {flags, slot, 0x00});
    reply.push_back(options.hints[0]);
    if (options.hints[0] & 0x80) reply.push_back(options.hints[1]);
    reply.push_back(0x00);  // ASCII
    reply.insert(reply.end(), options.nickname.begin(), options.nickname.end());
    holdUntil = std::max(holdUntil, now + ndmTurn);
    Emit(reply, now);
}

void IrLap::Snrm(const Bytes& frame, uint64_t now) {
    // Source, destination, new connection address, then parameters.
    if (frame.size() < 11) return;
    std::array<uint8_t, 4> source, target;
    std::copy(frame.begin() + 2, frame.begin() + 6, source.begin());
    std::copy(frame.begin() + 6, frame.begin() + 10, target.begin());
    if (target != options.address) return;
    if (state == State::Nrm) Down(now);
    guest = source;
    caddr = frame[10] & 0xfe;
    if (caddr == broadcast) caddr = 0x02;

    // Baud and disconnect time are agreed by both (the highest common value); the rest are the
    // guest's own receive limits, which bind what we send.
    const unsigned baudMask = Mask(frame, 11, piBaud, 0x02) & options.baudMask;
    const unsigned disconnectMask = Mask(frame, 11, piDisconnect, 0x01);
    Link next;
    const int baud = Highest(baudMask ? baudMask : 0x02, 8);
    next.baud = bauds[baud];
    next.disconnectNs = disconnects[std::max(0, Highest(disconnectMask, 8))];
    next.dataSize = dataSizes[std::max(0, Highest(Mask(frame, 11, piDataSize, 0x01), 6))];
    next.window = 1 + std::max(0, Highest(Mask(frame, 11, piWindow, 0x01), 7));
    next.maxTurnNs = maxTurns[std::max(0, Highest(Mask(frame, 11, piMaxTurn, 0x01), 4))];
    // Answers wait exactly the guest's own minimum turnaround: Palm OS 3.5 asks for 1 ms and 4.1
    // for 5 ms, and both stop listening if an answer comes much later.
    next.minTurnNs = minTurns[std::max(0, Highest(Mask(frame, 11, piMinTurn, 0x01), 8))];
    const unsigned bofs = bofCounts[std::max(0, Highest(Mask(frame, 11, piBofs, 0x01), 8))];
    // The extra BOF count is given for 115200 and scales with the speed.
    next.extraBofs = unsigned(uint64_t(bofs) * next.baud / 115200);

    Bytes reply{caddr, ua};
    reply.insert(reply.end(), options.address.begin(), options.address.end());
    reply.insert(reply.end(), source.begin(), source.end());
    reply.insert(reply.end(), {piBaud, 1, uint8_t(baudMask ? baudMask : 0x02), piMaxTurn, 1, ourMaxTurn,
                               piDataSize, 1, ourDataSize, piWindow, 1, ourWindow, piBofs, 1, ourBofs,
                               piMinTurn, 1, ourMinTurn, piDisconnect, 1, uint8_t(disconnectMask ? disconnectMask : 1)});
    // The UA goes at the old speed with the old framing, after the guest's turnaround.
    holdUntil = std::max(holdUntil, now + next.minTurnNs);
    Emit(reply, now);

    link = next;
    state = State::Nrm;
    ResetSequence();
    lastHeard = lastData = now;
    dataSeen = false;
    ++stats.connects;
    user.LinkUp(now);
}

void IrLap::Test(const Bytes& frame, uint8_t address, uint64_t now) {
    if (!(frame[1] & pf) || frame.size() < 10) return;
    Bytes reply{address, test};
    reply.insert(reply.end(), options.address.begin(), options.address.end());
    reply.insert(reply.end(), frame.begin() + 2, frame.begin() + 6);
    reply.insert(reply.end(), frame.begin() + 10, frame.end());
    holdUntil = std::max(holdUntil, now + (state == State::Nrm ? link.minTurnNs : ndmTurn));
    Emit(reply, now);
}

void IrLap::Nrm(const Bytes& frame, uint64_t now) {
    lastHeard = now;
    const uint8_t control = frame[1];
    const bool poll = control & pf;
    if (!(control & 1)) {
        // I frame.
        const uint8_t ns = (control >> 1) & 7, nr = control >> 5;
        Acknowledge(nr);
        if (ns == vr && !localBusy) {
            vr = (vr + 1) & 7;
            user.Data(frame.data() + 2, frame.size() - 2, now);
        } else if (ns != vr) {
            ++stats.outOfSequence;
            rejected = true;
        }
    } else if ((control & 3) == 1) {
        // Supervisory: RR, RNR, REJ, SREJ. A reject needs nothing more: the unacknowledged
        // frames go again on this poll.
        const uint8_t kind = control & 0x0f;
        Acknowledge(control >> 5);
        remoteBusy = kind == rnr;
        if (kind != rr && kind != rnr && kind != rej && kind != srej) return;
    } else {
        switch (control) {
            case disc: {
                // Anything unread for the old link goes; the UA is the last frame it gets.
                const uint64_t turn = link.minTurnNs;
                out.clear();
                outPos = 0;
                Emit({caddr, ua}, now);
                holdUntil = std::max(holdUntil, now + turn);
                ++stats.disconnects;
                Down(now, true);
                return;
            }
            case test:
                return Test(frame, caddr, now);
            default:
                // UI, XID and the rest take no part in the link.
                break;
        }
    }
    if (poll) Respond(now);
}

void IrLap::Acknowledge(uint8_t nr) {
    // nr acknowledges our frames up to nr - 1; it must fall within what we've sent.
    const uint8_t oldest = uint8_t((vs - window.size()) & 7);
    const size_t acked = (nr - oldest) & 7;
    if (acked > window.size()) return;
    window.erase(window.begin(), window.begin() + acked);
}

void IrLap::Respond(uint64_t now) {
    user.Pump(now);
    if (out.size() == outPos) holdUntil = std::max(holdUntil, now + link.minTurnNs);
    const uint8_t nr = uint8_t(vr << 5);
    if (rejected) {
        // Out of sequence: ask for everything from vr again, and send nothing of ours this turn.
        rejected = false;
        ++stats.rejects;
        Emit({caddr, uint8_t(rej | pf | nr)}, now);
        return;
    }
    // Go-back-N: frames still in the window weren't received, so they go again, then new ones up
    // to the guest's window and what fits in its turnaround time.
    std::vector<const Sent*> frames;
    if (!remoteBusy) {
        for (const Sent& sent : window) frames.push_back(&sent);
        stats.resent += unsigned(window.size());
        const uint64_t budget = link.maxTurnNs / millisecond * link.baud / 10 / 1000;
        uint64_t bytes = 0;
        for (const Sent& sent : window) bytes += sent.info.size() + 6;
        while (!queue.empty() && window.size() < link.window && (frames.empty() || bytes + queue.front().size() + 6 <= budget)) {
            bytes += queue.front().size() + 6;
            window.push_back({vs, std::move(queue.front())});
            queue.pop_front();
            vs = (vs + 1) & 7;
            frames.push_back(&window.back());
        }
    }
    if (frames.empty()) {
        Emit({caddr, uint8_t((localBusy ? rnr : rr) | pf | nr)}, now);
        return;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        Bytes frame{caddr, uint8_t(nr | (frames[i]->ns << 1) | (i + 1 == frames.size() ? pf : 0))};
        frame.insert(frame.end(), frames[i]->info.begin(), frames[i]->info.end());
        Emit(frame, now);
    }
}

void IrLap::Emit(const Bytes& frame, uint64_t now) {
    if (outPos && outPos == out.size()) {
        out.clear();
        outPos = 0;
    }
    SirWrap(frame, ExtraBofs(), out);
    ++stats.framesOut;
}

unsigned IrLap::ExtraBofs() const { return state == State::Nrm ? link.extraBofs : ndmBofs; }

void IrLap::Down(uint64_t now, bool keepOutput) {
    // Frames still unread were for the old link: a new one must not get them, and the core
    // shouldn't keep waking for them.
    if (!keepOutput) {
        out.clear();
        outPos = 0;
        holdUntil = 0;
    }
    state = State::Ndm;
    link = Link{};
    ResetSequence();
    queue.clear();
    ourSlot = lastSlot = -1;
    answered = false;
    user.LinkDown(now);
}

void IrLap::ResetSequence() {
    vs = vr = 0;
    window.clear();
    remoteBusy = rejected = false;
}

}  // namespace frondIr
