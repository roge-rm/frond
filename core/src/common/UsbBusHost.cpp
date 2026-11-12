#include "UsbBusHost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using R = UsbPacketResult;

namespace {
// Full-speed bit times: 12 Mbit/s.
uint64_t bitsNs(uint64_t bits) { return (bits * 1000 + 11) / 12; }
// Token (SYNC, PID, ADDR, ENDP, CRC5: 32), DATAx (SYNC, PID, CRC16: 32 + 8 * bytes),
// handshake (16), a 2-bit EOP after each packet and two 2-bit inter-packet gaps.
uint64_t transactionBits(unsigned bytes) { return 32 + 32 + 8ull * bytes + 16 + 3 * 2 + 2 * 2; }
constexpr uint64_t nakBits = 32 + 16 + 2 * 2 + 2;  // token, NAK/STALL handshake
constexpr uint64_t timeoutBits = 32 + 2 + 18;      // token, bus turnaround timeout
constexpr uint64_t sofBits = 32 + 2;
constexpr uint64_t eofGuardNs = 50000;             // keep clear of the frame end

std::string hex4(uint16_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "%04x", value);
    return text;
}
std::string printable(const uint8_t* bytes, size_t size) {
    std::string text;
    for (size_t i = 0; i < size; ++i) text += bytes[i] >= 0x20 && bytes[i] < 0x7f ? char(bytes[i]) : '.';
    return text;
}
}  // namespace

UsbBusHost::UsbBusHost(std::function<UsbDevicePort()> udc, Link link) : udc(std::move(udc)), link(std::move(link)) {}

const char* UsbBusHost::PhaseName(Phase phase) {
    switch (phase) {
        case Phase::detached: return "detached";
        case Phase::waitEnable: return "waiting";
        case Phase::connect: return "connecting";
        case Phase::reset: return "resetting";
        case Phase::enumerate: return "enumerating";
        case Phase::running: return "running";
        case Phase::failed: return "failed";
    }
    return "?";
}

void UsbBusHost::Message(const std::string& text) {
    if (messages.size() < 64) messages.push_back(text);
}

void UsbBusHost::Attach() {
    if (phase != Phase::detached && phase != Phase::failed) return;
    Detach();
    phase = Phase::waitEnable;
    poked = true;  // look at the UDC on the next tick
}

void UsbBusHost::Detach() {
    inRequestAt = outRequestAt = 0;
    phase = Phase::detached;
    inFlight = Token::none;
    control = {};
    sofRunning = false;
    address = pendingAddress = 0;
    attempts = 0;
    outPending = clearHalt = false;
    outSize = 0;
    bulkTimeouts = 0;
    retryAt = busyUntil = lastControlEnd = requeryAt = 0;
    poked = waitSpace = false;
    step = 0;
    inEndpoint = outEndpoint = 0;
    portFunction.clear();
    configDescriptor.clear();
    inMps = outMps = 0;
}

uint64_t UsbBusHost::NextWake() const {
    if (phase == Phase::detached || phase == Phase::failed) return UINT64_MAX;
    if (poked) return 0;
    if (phase == Phase::waitEnable) return UINT64_MAX;  // the UDE and UDCICR writes poke
    if (phase == Phase::connect || phase == Phase::reset) return phaseAt;
    uint64_t wake = inFlight != Token::none ? busyUntil : retryAt;
    if (pendingAddress) wake = std::min(wake, addressAt);
    auto u = udc();
    if (sofRunning && u && u.SofWanted()) wake = std::min(wake, nextSof);
    return wake;
}

bool UsbBusHost::LinkWaiting() const {
    if (phase != Phase::running || !inEndpoint || inFlight != Token::none || control.active || poked) return false;
    return (!outPending && link.pending && link.pending()) || (waitSpace && link.sendSpace && link.sendSpace() >= inMps);
}

// A NAK, or nothing to send: try again 10 us after the guest next touches the UDC
// (Poke), or once the next frame has started.
void UsbBusHost::Nak() {
    nakAt = now;
    retryAt = std::max(now + nakRetryNs, nextSof + bitsNs(sofBits));
}

bool UsbBusHost::Addressed() { return udc().Address() == address; }

// Enumeration failed: reset the port and start again a little later, a few times.
void UsbBusHost::Restart(const char* why) {
    Message(std::string("usb: ") + why);
    inFlight = Token::none;
    control = {};
    sofRunning = false;
    clearHalt = false;
    inEndpoint = outEndpoint = inMps = outMps = 0;
    if (++attempts >= maxAttempts) {
        phase = Phase::failed;
        Message("usb: gave up; unplug and plug in again to retry");
        return;
    }
    phase = Phase::connect;
    phaseAt = now + retryNs;
}

bool UsbBusHost::ReadStringDescriptor(uint8_t index,uint16_t language) {
    if(phase!=Phase::running || control.active || inFlight != Token::none || clearHalt) return false;
    StartControl(0x80,6,uint16_t(0x0300|index),language,255, Completion::external);
    Poke();
    return true;
}

void UsbBusHost::StartControl(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, Completion completion) {
    control = {};
    control.active = true;
    control.completion = completion;
    const uint8_t setup[8] = {type, request, uint8_t(value), uint8_t(value >> 8), uint8_t(index),
                              uint8_t(index >> 8), uint8_t(length), uint8_t(length >> 8)};
    std::memcpy(control.setup, setup, 8);
    // After a SOF, at least 1 ms after the previous transfer (and the address recovery).
    uint64_t earliest = std::max(lastControlEnd + frameNs, now + 1);
    if (pendingAddress) earliest = std::max(earliest, addressAt);
    control.startAt = (earliest + frameNs - 1) / frameNs * frameNs;
    control.deadline = control.startAt + controlTimeoutNs;
}

void UsbBusHost::ControlDone(Result result) {
    const auto completed = control;
    control.active = false;
    lastControlEnd = now;
    switch (completed.completion) {
        case Completion::enumeration: EnumerationStep(result); break;
        case Completion::clearHalt:
            clearHalt = false;
            if (result != Result::ok) Restart("could not clear a halted endpoint");
            break;
        case Completion::external: break;
    }
    if (controlTrace) controlTrace(completed.setup, completed.data, result == Result::ok);
}

// SET_CONFIGURATION selects alternate setting zero on every interface.
bool UsbBusHost::ParseConfiguration() {
    const auto& d = configDescriptor;
    std::fill(std::begin(endpointMps), std::end(endpointMps), 0);
    if (d.size() != configLength || d.size() < 9 || d[0] != 9 || d[1] != 2 ||
        (d[2] | d[3] << 8) != configLength || d[5] != configuration || !configuration || !d[4]) return false;
    bool interfaces[256]{}, activeAddresses[256]{}, addresses[256]{};
    bool alternates[256][256]{};
    unsigned interfaceCount = 0, expected = 0, endpoints = 0;
    bool haveInterface = false, active = false;
    for (size_t pos = 9; pos < d.size();) {
        if (d.size() - pos < 2 || d[pos] < 2 || d[pos] > d.size() - pos) return false;
        const auto* e = &d[pos];
        if (e[1] == 2) return false;
        if (e[1] == 4) {
            if (e[0] != 9 || e[2] >= d[4] || (haveInterface && endpoints != expected) || alternates[e[2]][e[3]]) return false;
            alternates[e[2]][e[3]] = true;
            if (!interfaces[e[2]]) { interfaces[e[2]] = true; ++interfaceCount; }
            std::fill(std::begin(addresses), std::end(addresses), false);
            haveInterface = true;
            active = e[3] == 0;
            expected = e[4];
            endpoints = 0;
        } else if (e[1] == 5) {
            if (e[0] != 7 || !haveInterface || !(e[2] & 15) || (e[2] & 0x70)) return false;
            if (addresses[e[2]]) return false;
            addresses[e[2]] = true;
            ++endpoints;
            const unsigned mps = e[4] | e[5] << 8;
            if ((e[3] & 3) == 2 && (e[3] != 2 || (mps != 8 && mps != 16 && mps != 32 && mps != 64))) return false;
            if (active) {
                if (activeAddresses[e[2]]) return false;
                activeAddresses[e[2]] = true;
                if (e[3] == 2) endpointMps[e[2]] = uint8_t(mps);
            }
        }
        pos += e[0];
    }
    if (!haveInterface || endpoints != expected || interfaceCount != d[4]) return false;
    for (unsigned i = 0; i < 256; ++i)
        if (interfaces[i] && !alternates[i][0]) return false;
    return true;
}

bool UsbBusHost::SelectEndpoints() {
    inMps = outMps = 0;
    if (!inEndpoint && !outEndpoint) return portFunction.empty();
    if (!inEndpoint || inEndpoint > 15 || !outEndpoint || outEndpoint > 15) return false;
    inMps = endpointMps[0x80 | inEndpoint];
    outMps = endpointMps[outEndpoint];
    return inMps && outMps;
}

void UsbBusHost::EnumerationStep(Result result) {
    const auto& d = control.data;
    auto fail = [&](const char* what) {
        Restart((std::string(what) + (result == Result::stall ? " stalled" :
                                      result == Result::timeout ? " timed out" : " failed")).c_str());
    };
    switch (step) {
        case 0:  // GET_DESCRIPTOR(device, 8) at address 0
            if (result != Result::ok || d.size() < 8 ||
                (d[7] != 8 && d[7] != 16 && d[7] != 32 && d[7] != 64))
                return fail("device descriptor");
            maxPacket0 = d[7];
            step = 1;
            return StartControl(0x00, 5, deviceAddress, 0, 0);
        case 1:  // SET_ADDRESS, then 2 ms recovery (USB 2.0 9.2.6.3)
            if (result != Result::ok) return fail("SET_ADDRESS");
            pendingAddress = deviceAddress;
            addressAt = now + 2 * frameNs;
            step = 2;
            return StartControl(0x80, 6, 0x0100, 0, 18);
        case 2:
            if (result != Result::ok || d.size() < 18) return fail("device descriptor");
            vendor = uint16_t(d[8] | d[9] << 8);
            product = uint16_t(d[10] | d[11] << 8);
            step = 3;
            return StartControl(0x80, 6, 0x0200, 0, 9);
        case 3:
            if (result != Result::ok || d.size() < 9 || d[0] != 9 || d[1] != 2) return fail("configuration descriptor");
            configLength = uint16_t(d[2] | d[3] << 8);
            configuration = d[5];
            configDescriptor = d;
            if (configLength < 9 || !configuration) return fail("configuration descriptor");
            step = 4;
            return StartControl(0x80, 6, 0x0200, 0, configLength);
        case 4:
            if (d.size() < 9 || !std::equal(configDescriptor.begin(), configDescriptor.end(), d.begin()))
                return fail("inconsistent configuration header");
            configDescriptor = d;
            if (result != Result::ok || !ParseConfiguration()) return fail("configuration descriptor");
            step = 5;
            return StartControl(0x00, 9, configuration, 0, 0);
        case 5:
            if (result != Result::ok) return fail("SET_CONFIGURATION");
            step = 6;
            // GET_EXT_CONNECTION_INFORMATION, as pilot-link and the Linux visor driver ask.
            return StartControl(0xc2, 4, 0, 0, 20);
        case 6:
            inEndpoint = outEndpoint = 0;
            portFunction.clear();
            if (result == Result::ok && d.size() >= 12 && d[0]) {
                // num_ports, endpoint_numbers_different, 2 reserved, then 8 bytes per port:
                // function id (4), port, endpoint info (IN << 4 | OUT), 2 reserved.
                const unsigned ports = std::min<unsigned>(d[0], unsigned((d.size() - 4) / 8));
                unsigned chosen = 0;
                for (unsigned i = 0; i < ports; ++i)
                    if (!std::memcmp(&d[4 + 8 * i], "cnys", 4)) chosen = i;
                const uint8_t* port = &d[4 + 8 * chosen];
                portFunction = printable(port, 4);
                inEndpoint = d[1] ? port[5] >> 4 : port[4];
                outEndpoint = d[1] ? port[5] & 15 : port[4];
            } else if (result == Result::stall) {
                step = 7;
                // GET_CONNECTION_INFORMATION (Visor, Palm OS 3): pick the HotSync port.
                return StartControl(0xc2, 3, 0, 0, 18);
            }
            break;
        case 7:
            if (result == Result::ok && d.size() >= 4) {
                const unsigned ports = std::min<unsigned>(d[0] | d[1] << 8, unsigned((d.size() - 2) / 2));
                for (unsigned i = 0; i < ports; ++i)
                    if (d[2 + 2 * i] == 2) {
                        portFunction = "hotsync";
                        inEndpoint = outEndpoint = d[3 + 2 * i];
                    }
            }
            break;
    }
    if (!SelectEndpoints()) return Restart("invalid bulk endpoints in port list");
    const bool wasRunning = phase == Phase::running;
    phase = Phase::running;
    attempts = 0;
    // No port named yet (the TX lists none outside HotSync at times): ask again each
    // second. The bulk endpoints are never guessed; the TX ROM faults on OUT data to an
    // endpoint it has not set up.
    if (!inEndpoint || !outEndpoint) {
        inEndpoint = outEndpoint = 0;
        requeryAt = now + 1000 * frameNs;
        if (!wasRunning) {
            ++counters.enumerations;
            Message("usb: enumerated " + hex4(vendor) + ":" + hex4(product) + ", no HotSync port yet");
        }
        return;
    }
    if (!wasRunning) ++counters.enumerations;
    Message("usb: enumerated " + hex4(vendor) + ":" + hex4(product) + ", port " +
            (portFunction.empty() ? std::string("(none)") : "'" + portFunction + "'") + " IN " +
            std::to_string(inEndpoint) + " OUT " + std::to_string(outEndpoint));
}

// Reserves bus time for a transaction of `bits`, handshake included.
// False if it would not fit in what is left of this frame.
bool UsbBusHost::Begin(Token token, unsigned bits) {
    const uint64_t ns = bitsNs(bits);
    if (now < lastSof + bitsNs(sofBits)) {
        retryAt = lastSof + bitsNs(sofBits);
        return false;
    }
    if (now % frameNs + ns + eofGuardNs > frameNs) {
        retryAt = nextSof + bitsNs(sofBits);
        return false;
    }
    inFlight = token;
    busyUntil = now + ns;
    if (log) log->push_back({now, now + ns});
    ++counters.transactions;
    return true;
}

bool UsbBusHost::StartTransaction() {
    auto u = udc();
    if (control.active) {
        if (now < control.startAt) {
            retryAt = control.startAt;
            return false;
        }
        if (now > control.deadline) {
            ControlDone(Result::timeout);
            return false;
        }
        switch (control.stage) {
            case Stage::setup: return Begin(Token::setup, unsigned(transactionBits(8)));
            case Stage::dataOut: {
                const unsigned length = control.setup[6] | control.setup[7] << 8;
                return Begin(Token::out0, unsigned(transactionBits(std::min<unsigned>(maxPacket0, length - unsigned(control.sent)))));
            }
            case Stage::statusOut: return Begin(Token::out0, unsigned(transactionBits(0)));
            case Stage::dataIn:
            case Stage::statusIn: {
                if (!Addressed()) return Begin(Token::in0, unsigned(timeoutBits));
                unsigned size = 0;
                const R r = u.PeekIn(0, &size);
                return Begin(Token::in0, unsigned(r == R::Ack ? transactionBits(size) : r == R::Timeout ? timeoutBits : nakBits));
            }
        }
        return false;
    }
    if (phase != Phase::running) return false;
    if (!inEndpoint) {
        if (now < requeryAt) {
            retryAt = requeryAt;
            return false;
        }
        step = 6;
        StartControl(0xc2, 4, 0, 0, 20);
        return false;
    }
    if (clearHalt) {
        StartControl(0x02, 1, 0, haltedEndpoint, 0, Completion::clearHalt);
        return false;
    }
    if (!outPending && link.receive) {
        outSize = unsigned(link.receive(outPacket, outMps));
        outPending = outSize != 0;
    }
    const bool wantIn = link.sendSpace && link.sendSpace() >= inMps;
    // A cooperative adapter may hold this request without putting an OUT token on the bus.
    const bool outReady = outPending && (!Addressed() || u.OutReady(outEndpoint,std::min<unsigned>(outSize,outMps)));
    if (outReady && now >= outRequestAt && (preferOut || !wantIn || now < inRequestAt)) {
        preferOut = false;
        inFlightEndpoint = outEndpoint;
        outFlightSize = std::min<unsigned>(outSize, outMps);
        return Begin(Token::bulkOut, unsigned(Addressed() ? transactionBits(outFlightSize) : timeoutBits));
    }
    preferOut = true;
    waitSpace = !wantIn;
    if (!wantIn) {
        Nak();  // Service pokes once the host side has room or bytes
        if (outPending && outRequestAt > now) retryAt = std::min(retryAt, outRequestAt);
        return false;
    }
    if (now < inRequestAt) {
        retryAt = outPending && outRequestAt > now ? std::min(inRequestAt, outRequestAt) : inRequestAt;
        return false;
    }
    inFlightEndpoint = uint8_t(0x80 | inEndpoint);
    if (!Addressed()) return Begin(Token::bulkIn, unsigned(timeoutBits));
    unsigned size = 0;
    const R r = u.PeekIn(inFlightEndpoint, &size);
    if (r == R::Ack && size > inMps) { Restart("oversized bulk IN packet"); return false; }
    return Begin(Token::bulkIn, unsigned(r == R::Ack ? transactionBits(size) : r == R::Timeout ? timeoutBits : nakBits));
}

void UsbBusHost::FinishTransaction() {
    auto u = udc();
    const Token token = inFlight;
    inFlight = Token::none;
    const bool addressed = Addressed();
    auto nak = [&] {
        control.errors = 0;
        ++counters.naks;
        Nak();
        // A NAK retry must not postpone the other direction's next request.
        if (token == Token::bulkIn && outPending && outRequestAt > now)
            retryAt = std::min(retryAt, outRequestAt);
        if (token == Token::bulkOut && link.sendSpace && link.sendSpace() >= inMps && inRequestAt > now)
            retryAt = std::min(retryAt, inRequestAt);
        // Nothing but NAKs on a standard request for a while: the device may have missed
        // the SETUP. Vendor requests (the Palm port list) can take a TX several hundred
        // ms, and a new SETUP would start them over.
        if (control.active && control.stage != Stage::setup && !(control.setup[0] & 0x60) &&
            now >= control.setupAt + setupRetryNs) {
            ++counters.setupRetries;
            char text[96];
            std::snprintf(text, sizeof(text), "usb: no answer on EP0 to %02x/%02x %ums after its SETUP, sending it again",
                          control.setup[0], control.setup[1], unsigned((now - control.setupAt) / frameNs));
            Message(text);
            control.stage = Stage::setup;
            control.data.clear();
            control.sent = 0;
        }
    };
    // A device can miss a token while reallocating its FIFOs. Retry the transaction
    // on a later frame, with at most three consecutive transmission errors.
    auto timeout = [&] {
        if (++control.errors >= 3) return ControlDone(Result::timeout);
        retryAt = std::max(now + nakRetryNs, nextSof + bitsNs(sofBits));
    };
    switch (token) {
        case Token::none: return;
        case Token::setup: {
            if (!addressed) return timeout();
            const R r = u.Setup(control.setup);
            if (r == R::Timeout) return timeout();
            if (r != R::Ack) return ControlDone(Result::error);
            control.errors = 0;
            control.setupAt = now;
            const unsigned length = control.setup[6] | control.setup[7] << 8;
            control.stage = (control.setup[0] & 0x80) ? (length ? Stage::dataIn : Stage::statusIn)
                                                      : (length ? Stage::dataOut : Stage::statusIn);
            return;
        }
        case Token::out0: {
            if (!addressed) return timeout();
            const bool status = control.stage == Stage::statusOut;
            const unsigned length = control.setup[6] | control.setup[7] << 8;
            const unsigned n = status ? 0 : std::min<unsigned>(maxPacket0, length - unsigned(control.sent));
            const R r = u.Out(status ? nullptr : control.out.data() + control.sent, n);
            if (r == R::Nak) return nak();
            if (r == R::Stall) return ControlDone(Result::stall);
            if (r == R::Timeout) return timeout();
            if (r != R::Ack) return ControlDone(Result::error);
            control.errors = 0;
            if (status) return ControlDone(Result::ok);
            control.sent += n;
            if (control.sent >= length) control.stage = Stage::statusIn;
            return;
        }
        case Token::in0: {
            if (!addressed) return timeout();
            unsigned size = 0;
            R r = u.PeekIn(0, &size);
            uint8_t data[64];
            if (r == R::Nak) return nak();
            if (r == R::Ack && size > maxPacket0) return ControlDone(Result::error);
            r = u.In(data, &size);  // ACK, or the STALL handshake
            if (r == R::Nak) return nak();
            if (r == R::Stall) return ControlDone(Result::stall);
            if (r == R::Timeout) return timeout();
            if (r != R::Ack) return ControlDone(Result::error);
            control.errors = 0;
            if (size > maxPacket0) return ControlDone(Result::error);
            if (control.stage == Stage::statusIn) return ControlDone(size ? Result::error : Result::ok);
            const unsigned length = control.setup[6] | control.setup[7] << 8;
            control.data.insert(control.data.end(), data, data + size);
            if (control.data.size() > length) return ControlDone(Result::error);
            if (size < maxPacket0 || control.data.size() == length) control.stage = Stage::statusOut;
            return;
        }
        case Token::bulkIn: {
            uint8_t data[64];
            unsigned size = 0;
            if (addressed && u.PeekIn(inFlightEndpoint, &size) == R::Ack && size > inMps)
                return Restart("oversized bulk IN packet");
            const R r = addressed ? u.EpIn(inFlightEndpoint, data, &size) : R::Timeout;
            if (r == R::Ack) {
                bulkTimeouts = 0;
                if (size > inMps) return Restart("oversized bulk IN packet");
                if (size) link.send(data, size);
                counters.bytesIn += size;
                // Host software policy: retire this one-packet request and resubmit later.
                inRequestAt = now + frameNs;
                return;
            }
            if (r == R::Nak) return nak();
            if (r == R::Stall) {
                clearHalt = true;
                haltedEndpoint = inFlightEndpoint;
                return;
            }
            if (++bulkTimeouts >= 100) Restart("the bulk IN endpoint stopped answering");
            else retryAt = now + nakRetryNs;
            return;
        }
        case Token::bulkOut: {
            const R r = addressed ? u.EpOut(inFlightEndpoint, outPacket, outFlightSize) : R::Timeout;
            if (r == R::Ack) {
                bulkTimeouts = 0;
                outRequestAt = now + frameNs;
                counters.bytesOut += outFlightSize;
                outSize -= outFlightSize;
                std::memmove(outPacket, outPacket + outFlightSize, outSize);
                outPending = outSize != 0;
                return;
            }
            if (r == R::Nak) return nak();
            if (r == R::Stall) {
                clearHalt = true;
                haltedEndpoint = inFlightEndpoint;
                return;
            }
            if (++bulkTimeouts >= 100) Restart("the bulk OUT endpoint stopped answering");
            else retryAt = now + nakRetryNs;
            return;
        }
    }
}

void UsbBusHost::Tick(uint64_t nowNs) {
    now = nowNs;
    ++counters.ticks;
    if (poked) {
        poked = false;
        if (retryAt > now) retryAt = std::max(now, nakAt + nakRetryNs);
    }
    if (phase == Phase::detached || phase == Phase::failed) return;
    auto u = udc();
    if (!u) return;
    // On the bus: the device listens for a reset and EP0 (see pxa270UdcHostListening).
    const bool present = u.Listening();
    const bool reconnected = phase != Phase::waitEnable && u.EnableCount() != enableCount;
    if (!present || reconnected) {
        if (phase != Phase::waitEnable) {
            Message(present ? "usb: the device left the bus and came back" : "usb: the device left the bus; waiting");
            const unsigned pendingSize = outPending ? outSize : 0;
            Detach();
            outSize = pendingSize;
            outPending = outSize != 0;
            phase = Phase::waitEnable;
        }
        if (!present) return;
    }
    switch (phase) {
        case Phase::waitEnable:
            enableCount = u.EnableCount();
            phase = Phase::connect;
            phaseAt = now + connectNs;
            return;
        case Phase::connect:
            if (now < phaseAt) return;
            // Port reset: the UDC drops its address and configuration at once; the host
            // waits out the reset and recovery before its first SETUP.
            if (!u.Reset()) return;
            address = pendingAddress = 0;
            maxPacket0 = 8;
            phase = Phase::reset;
            phaseAt = now + resetNs;
            sofRunning = true;
            lastSof = now / frameNs * frameNs;
            nextSof = lastSof + frameNs;
            retryAt = 0;
            return;
        default: break;
    }
    if (inFlight != Token::none && now >= busyUntil) FinishTransaction();
    if (!Running()) return;  // a restart from FinishTransaction
    if (sofRunning && now >= nextSof) {
        // Frames slept through still count in UDCFNR, which wraps at 2048.
        const uint64_t frames = (now - lastSof) / frameNs;
        const uint64_t count = frames % 2048 ? frames % 2048 : 2048;
        for (uint64_t i = 0; i < count; ++i) u.Frame();
        counters.frames += frames;
        lastSof = now / frameNs * frameNs;
        nextSof = lastSof + frameNs;
    }
    if (pendingAddress && now >= addressAt) {
        address = pendingAddress;
        pendingAddress = 0;
    }
    if (phase == Phase::reset) {
        if (now < phaseAt) return;
        phase = Phase::enumerate;
        step = 0;
        StartControl(0x80, 6, 0x0100, 0, 8);
    }
    if (inFlight != Token::none || now < retryAt) return;
    // A step that only prepares (a new control transfer) may start at once.
    for (unsigned i = 0; i < 2 && inFlight == Token::none && now >= retryAt && Running(); ++i)
        if (StartTransaction()) break;
}
