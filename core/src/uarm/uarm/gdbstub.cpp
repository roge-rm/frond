//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr
// Frond's native, single-CPU ARM/Thumb RSP foundation.
#include "gdbstub.h"

#ifdef GDB_STUB_ENABLED
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include "cputil.h"
#include "pace.h"
#include "native_boundary.h"

namespace {
constexpr size_t packetSize = 4096; // RSP PacketSize is hexadecimal: 1000.
constexpr size_t maxBreakpoints = 16;
enum class RunState { Stopped, Running, Step, PaceStep };
enum class RxState { Idle, Data, Checksum1, Checksum2 };
struct Breakpoint { uint32_t addr, kind; char type; };

const std::string targetXml = [] {
    std::string xml = "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
                      "<target><architecture>arm</architecture>"
                      "<feature name=\"org.gnu.gdb.arm.core\">";
    for (unsigned i = 0; i < 16; ++i) {
        const std::string name = i == 13 ? "sp" : i == 14 ? "lr" : i == 15 ? "pc" : "r" + std::to_string(i);
        xml += "<reg name=\"" + name + "\" bitsize=\"32\" regnum=\"" + std::to_string(i) +
               "\" type=\"" + (i == 15 ? "code_ptr" : i == 13 ? "data_ptr" : "uint32") + "\"/>";
    }
    return xml + "<reg name=\"cpsr\" bitsize=\"32\" regnum=\"25\"/>"
                 "</feature></target>";
}();

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool number(std::string_view& in, uint32_t& value) {
    value = 0;
    size_t n = 0;
    while (n < in.size() && hexDigit(in[n]) >= 0) {
        if (value > (UINT32_MAX - unsigned(hexDigit(in[n]))) / 16) return false;
        value = value * 16 + hexDigit(in[n++]);
    }
    in.remove_prefix(n);
    return n != 0;
}
bool take(std::string_view& in, char c) {
    if (in.empty() || in.front() != c) return false;
    in.remove_prefix(1);
    return true;
}
bool range(uint32_t addr, uint32_t len) { return uint64_t(addr) + len <= (1ull << 32); }
bool decodeHex(std::string_view in, uint8_t* out, size_t len) {
    if (in.size() != len * 2) return false;
    // Validate the whole input before writing any guest state.
    for (char c : in) if (hexDigit(c) < 0) return false;
    for (size_t i = 0; i < len; ++i) out[i] = hexDigit(in[2*i]) * 16 + hexDigit(in[2*i+1]);
    return true;
}
std::string encodeHex(const uint8_t* bytes, size_t len) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}
uint32_t littleEndian(const uint8_t* in) {
    return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}
bool validCpsr(uint32_t val) {
    // Invalid modes otherwise reach a fatal error in the CPU's bank switch.
    switch (val & ARM_SR_M) {
        case ARM_SR_MODE_USR: case ARM_SR_MODE_FIQ: case ARM_SR_MODE_IRQ:
        case ARM_SR_MODE_SVC: case ARM_SR_MODE_ABT: case ARM_SR_MODE_UND:
        case ARM_SR_MODE_SYS: return true;
        default: return false;
    }
}
} // namespace

struct stub {
    ArmCpu* cpu;
    SoC* soc = nullptr;
    int listener = -1, sock = -1;
    RunState state = RunState::Stopped;
    bool stepDone = false, escaped = false, oversized = false, debugRequested = false;
    unsigned poll = 0;
    RxState rx = RxState::Idle;
    uint8_t sum = 0, expected = 0;
    size_t wireSize = 0;
    std::string input, lastReply, stopReason = "S05";
    std::vector<Breakpoint> breakpoints;
    std::vector<uint32_t> paceBreakpoints;
    bool stopPaceException = false, atPaceException = false;
    std::string lastPaceException, lastPaceStep;
    bool paceStepArmed = false, pendingPaceDispatch = false;
};

namespace {
void disconnect(stub* s) {
    if (s->sock >= 0) close(s->sock);
    s->sock = -1;
    s->state = RunState::Running;
    s->stepDone = false;
    s->breakpoints.clear();
    gdbStubResetPace(s);
    s->debugRequested = false;
    s->rx = RxState::Idle;
    s->input.clear();
    s->lastReply.clear();
}
bool sendBytes(stub* s, std::string_view data) {
    while (!data.empty() && s->sock >= 0) {
        const ssize_t n = send(s->sock, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { disconnect(s); return false; }
        data.remove_prefix(n);
    }
    return s->sock >= 0;
}
bool reply(stub* s, std::string_view data) {
    std::string frame = "$";
    uint8_t sum = 0;
    for (unsigned char c : data) {
        if (c == '$' || c == '#' || c == '}' || c == '*') {
            frame += '}'; sum += '}'; c ^= 0x20;
        }
        frame += char(c); sum += c;
    }
    const uint8_t checksum[] = {sum};
    frame += "#" + encodeHex(checksum, 1);
    s->lastReply = frame;
    // Copy above: disconnect can clear lastReply inside sendBytes.
    return sendBytes(s, frame);
}
void acceptClient(stub* s, bool wait) {
    do {
        s->sock = accept(s->listener, nullptr, nullptr);
    } while (s->sock < 0 && errno == EINTR);
    if (s->sock < 0) {
        if (wait) ERR("gdb accept failed: %s\n", strerror(errno));
        return;
    }
    fcntl(s->sock, F_SETFD, FD_CLOEXEC);
    const int one = 1;
    setsockopt(s->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const timeval timeout{2, 0};
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    s->state = RunState::Stopped;
    s->stopReason = "S05";
    s->stepDone = false;
    s->rx = RxState::Idle;
    s->input.clear();
    s->lastReply.clear();
    s->breakpoints.clear();
    gdbStubResetPace(s);
    s->debugRequested = false;
    fprintf(stderr, "GDB connected\n");
}
uint32_t getReg(stub* s, unsigned reg) {
    return cpuGetRegExternal(s->cpu, reg == 25 ? ARM_REG_NUM_CPSR : reg) & (reg == 15 ? ~1u : ~0u);
}
std::string readReg(stub* s, unsigned reg) {
    const uint32_t val = getReg(s, reg);
    const uint8_t bytes[] = {uint8_t(val), uint8_t(val >> 8), uint8_t(val >> 16), uint8_t(val >> 24)};
    return encodeHex(bytes, 4);
}
void setPc(stub* s, uint32_t val) {
    // In RSP PC is an address; CPSR owns the instruction-set bit.
    const bool thumb = getReg(s, 25) & ARM_SR_T;
    const uint32_t pc = val & (thumb ? ~1u : ~3u);
    cpuSetReg(s->cpu, 15, pc | unsigned(thumb));
    cpuSetCurInstrPC(s->cpu, pc);
}
bool memory(stub* s, uint32_t addr, uint32_t len, uint8_t* bytes, bool write) {
    uint32_t done = 0;
    while (done < len) {
        // Preserve aligned word MMIO access semantics when possible.
        const unsigned size = !((addr + done) & 3) && len - done >= 4 ? 4 :
                              !((addr + done) & 1) && len - done >= 2 ? 2 : 1;
        // The CPU memory bus requires naturally aligned host buffers too.
        alignas(8) uint8_t buf[8]{};
        if (write) memcpy(buf, bytes + done, size);
        if (!cpuMemOpExternal(s->cpu, buf, addr + done, size, write)) {
            if (write) cpuInvalidateCodeCache(s->cpu);
            return false;
        }
        if (!write) memcpy(bytes + done, buf, size);
        done += size;
    }
    if (write && done) cpuInvalidateCodeCache(s->cpu);
    return true;
}
std::string paceSnapshot(const char* phase) {
    const auto r = paceDebugState();
    std::string out = "{\"version\":1,\"available\":true,\"phase\":\"" + std::string(phase) +
                      "\",\"program_identity\":null,\"pc\":" + std::to_string(r.pc) +
                      ",\"sr\":" + std::to_string(r.sr) +
                      ",\"state_pointer\":" + std::to_string(r.statePtr);
    for (unsigned bank = 0; bank < 2; ++bank) {
        out += bank ? ",\"a\":[" : ",\"d\":[";
        for (unsigned i = 0; i < 8; ++i) {
            if (i) out += ',';
            out += std::to_string(bank ? r.a[i] : r.d[i]);
        }
        out += ']';
    }
    return out + '}';
}
bool breakpointHit(stub* s, uint32_t pc, bool thumb) {
    if (cpuIsModePace(s->cpu))
        return std::find(s->paceBreakpoints.begin(), s->paceBreakpoints.end(), pc) != s->paceBreakpoints.end();
    return std::any_of(s->breakpoints.begin(), s->breakpoints.end(), [=](const Breakpoint& bp) {
        return bp.addr == pc && bp.kind == (thumb ? 2u : 4u);
    });
}
void command(stub* s, std::string_view in) {
    // In all-stop mode, only interrupts and ACKs are accepted while executing.
    if (s->state != RunState::Stopped) { reply(s, "E16"); return; }
    std::string out;
    uint32_t addr = 0, len = 0, reg = 0;
    std::array<uint8_t, packetSize / 2> bytes{};
    if (in.find('\0') != in.npos) { reply(s, "E01"); return; }
    // ARM register packets remain ARM. Do not let native edits/steps silently
    // change a suspended ARM context while optimized 68k code owns execution.
    if (cpuIsModePace(s->cpu) && !in.empty() &&
        (in.front() == 'P' || in.front() == 'G' || in.front() == 'M' ||
         in.front() == 's' || in.substr(0, 7) == "vCont;s" ||
         (in.front() == 'c' && in.size() > 1))) {
        reply(s, "E16"); return;
    }
    if (in.substr(0, 6) == "qRcmd,") {
        in.remove_prefix(6);
        if (in.size() % 2 || !decodeHex(in, bytes.data(), in.size() / 2)) out = "E01";
        else {
            std::string text(reinterpret_cast<char*>(bytes.data()), in.size() / 2);
            std::string_view cmd(text);
            if (cmd == "attribution") {
                std::string result="{\"version\":1,\"foreground_app\":";
                result+=s->soc ? NativeBoundary::InspectUiApp(*s->soc) : "{\"available\":false,\"reason\":\"unsupported-profile\"}";
                result+=",\"code_owner\":{\"available\":false,\"reason\":\"resource-map-unverified\"}}\n";
                out=encodeHex(reinterpret_cast<const uint8_t*>(result.data()),result.size());
            } else if (cmd == "pace-step") {
                // A monitor command arms the next plain continue; it does not
                // resume behind GDB's back or change the ARM meaning of s/si.
                if (!cpuIsModePace(s->cpu) || s->pendingPaceDispatch || s->atPaceException) out = "E16";
                else {
                    s->paceStepArmed = true;
                    s->lastPaceStep = "{\"version\":1,\"available\":false,\"reason\":\"armed\"}";
                    out = "OK";
                }
            } else if (cmd == "pace-step off") {
                if (s->paceStepArmed)
                    s->lastPaceStep = "{\"version\":1,\"available\":false,\"reason\":\"cancelled\"}";
                s->paceStepArmed = false;
                out = "OK";
            } else if (cmd == "pace" || cmd == "pace-last" || cmd == "pace-last-step") {
                const std::string& saved = cmd == "pace-last-step" ? s->lastPaceStep : s->lastPaceException;
                const std::string result = (cmd != "pace"
                    ? (saved.empty() ? "{\"version\":1,\"available\":false}" : saved)
                    : (cpuIsModePace(s->cpu) ? paceSnapshot(s->atPaceException ? "exception" : s->pendingPaceDispatch ? "dispatch" : "boundary")
                       : "{\"version\":1,\"available\":false,\"reason\":\"native-mode\",\"program_identity\":null}")) + "\n";
                // qRcmd permits a single hex-encoded output reply. Keeping one
                // packet outstanding preserves ACK/NACK retransmission semantics.
                out = encodeHex(reinterpret_cast<const uint8_t*>(result.data()), result.size());
            } else if (cmd == "tg50-original-pace-entry") {
                out = s->state == RunState::Stopped && !s->atPaceException &&
                      !s->pendingPaceDispatch && cpuOriginalTg50PaceEntryForDiagnostic(s->cpu)
                          ? "OK" : "E16";
            } else if (cmd == "pace-stop on" || cmd == "pace-stop off") {
                s->stopPaceException = cmd == "pace-stop on"; out = "OK";
            } else if (cmd == "pace-clear") {
                s->paceBreakpoints.clear(); out = "OK";
            } else if (cmd.substr(0, 11) == "pace-break ") {
                cmd.remove_prefix(11);
                if (!number(cmd, addr) || !cmd.empty() || (addr & 1)) out = "E01";
                else if (std::find(s->paceBreakpoints.begin(), s->paceBreakpoints.end(), addr) != s->paceBreakpoints.end()) out = "OK";
                else if (s->paceBreakpoints.size() == maxBreakpoints) out = "E05";
                else { s->paceBreakpoints.push_back(addr); out = "OK"; }
            } else out = "E01";
        }
    } else if (in == "qSupported" || in.substr(0, 11) == "qSupported:")
        out = "PacketSize=1000;qXfer:features:read+";
    else if (in.substr(0, std::string_view("qXfer:features:read:target.xml:").size()) == "qXfer:features:read:target.xml:") {
        // Prefix length is deliberately computed to keep syntax and parser in sync.
        in.remove_prefix(std::string_view("qXfer:features:read:target.xml:").size());
        if (!number(in, addr) || !take(in, ',') || !number(in, len) || !in.empty() || !len || addr > targetXml.size()) out = "E01";
        else {
            len = std::min<size_t>({len, packetSize - 1, targetXml.size() - addr});
            out = (addr + len == targetXml.size() ? "l" : "m") + targetXml.substr(addr, len);
        }
    } else if (in == "?") out = s->stopReason;
    else if (in == "qAttached") out = "1";
    else if (in == "qC") out = "QC1";
    else if (in == "qfThreadInfo") out = "m1";
    else if (in == "qsThreadInfo") out = "l";
    else if (in == "Hg0" || in == "Hg1" || in == "Hg-1" || in == "Hc0" || in == "Hc1" || in == "Hc-1" || in == "T1" || in == "qSymbol::") out = "OK";
    else if (in == "vCont?") out = "vCont;c;s";
    else if (in == "D") {
        reply(s, "OK");
        disconnect(s);
        fprintf(stderr, "GDB detached; emulation continues\n");
        return;
    } else if (in == "g") {
        for (unsigned i = 0; i < 16; ++i) out += readReg(s, i);
        out += readReg(s, 25);
    } else if (take(in, 'p')) {
        if (!number(in, reg) || !in.empty() || !(reg < 16 || reg == 25)) out = "E01";
        else out = readReg(s, reg);
    } else if (take(in, 'P')) {
        if (!number(in, reg) || !take(in, '=') || !(reg < 16 || reg == 25) || !decodeHex(in, bytes.data(), 4)) out = "E01";
        else {
            const uint32_t val = littleEndian(bytes.data());
            if (reg == 25 && !validCpsr(val)) out = "E01";
            else {
                if (reg == 15) setPc(s, val);
                else {
                    cpuSetReg(s->cpu, reg == 25 ? ARM_REG_NUM_CPSR : reg, val);
                    if (reg == 25) setPc(s, cpuGetRegisters(s->cpu)[15]);
                }
                out = "OK";
            }
        }
    } else if (take(in, 'G')) {
        if (!decodeHex(in, bytes.data(), 17 * 4) || !validCpsr(littleEndian(bytes.data() + 64))) out = "E01";
        else {
            // Change banks before installing the supplied bank's registers.
            cpuSetReg(s->cpu, ARM_REG_NUM_CPSR, littleEndian(bytes.data() + 64));
            for (unsigned i = 0; i < 15; ++i) cpuSetReg(s->cpu, i, littleEndian(bytes.data() + 4*i));
            setPc(s, littleEndian(bytes.data() + 60));
            out = "OK";
        }
    } else if (!in.empty() && (in.front() == 'm' || in.front() == 'M')) {
        const bool write = in.front() == 'M';
        in.remove_prefix(1);
        if (!number(in, addr) || !take(in, ',') || !number(in, len) || !range(addr, len) || len > bytes.size() ||
            (write ? (!take(in, ':') || !decodeHex(in, bytes.data(), len)) : !in.empty())) out = "E01";
        else if (!memory(s, addr, len, bytes.data(), write)) out = "E0e";
        else out = write ? "OK" : encodeHex(bytes.data(), len);
    } else if (!in.empty() && (in.front() == 'Z' || in.front() == 'z')) {
        const bool add = in.front() == 'Z';
        in.remove_prefix(1);
        if (!number(in, reg) || !take(in, ',') || !number(in, addr) || !take(in, ',') || !number(in, len) || !in.empty()) out = "E01";
        else if (reg > 1) out = ""; // Z2/Z3/Z4 require stop-before-access ARM semantics.
        else if ((len != 2 && len != 4) || (addr & (len - 1))) out = "E01";
        else {
            auto bp = std::find_if(s->breakpoints.begin(), s->breakpoints.end(), [=](const Breakpoint& b) {
                return b.addr == addr && b.kind == len && b.type == char(reg);
            });
            if (!add) { if (bp != s->breakpoints.end()) s->breakpoints.erase(bp); out = "OK"; }
            else if (bp != s->breakpoints.end()) out = "OK";
            else if (s->breakpoints.size() == maxBreakpoints) out = "E05";
            else { s->breakpoints.push_back({addr, len, char(reg)}); out = "OK"; }
        }
    } else if (in == "vCont;c" || in == "vCont;c:1" || in == "vCont;s" || in == "vCont;s:1" ||
               (!in.empty() && (in.front() == 'c' || in.front() == 's'))) {
        const bool step = in.front() == 'v' ? in[6] == 's' : in.front() == 's';
        if (in.front() != 'v') {
            in.remove_prefix(1);
            if (!in.empty()) {
                if (!number(in, addr) || !in.empty()) { reply(s, "E01"); return; }
                setPc(s, addr);
            }
        }
        s->state = step ? RunState::Step : s->paceStepArmed ? RunState::PaceStep : RunState::Running;
        s->paceStepArmed = false;
        s->stepDone = false;
        return; // The ACK was already sent; the reply is the eventual stop.
    }
    // Unknown commands (including kill/restart, binary writes and signals) have
    // the standard empty unsupported reply, never terminate the emulator.
    reply(s, out);
}
void stop(stub* s, const char* reason, bool paceCompleted = false) {
    if (s->state == RunState::PaceStep && !paceCompleted)
        s->lastPaceStep = "{\"version\":1,\"available\":false,\"reason\":\"interrupted-before-instruction\"}";
    s->paceStepArmed = false;
    s->state = RunState::Stopped;
    s->stepDone = false;
    s->stopReason = reason;
    reply(s, s->stopReason);
}
void receive(stub* s, bool block) {
    // Bound work per running poll even if a client floods the socket.
    unsigned budget = packetSize * 2;
    while (s->sock >= 0 && budget--) {
        char c;
        const ssize_t n = recv(s->sock, &c, 1, block ? 0 : MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { disconnect(s); return; }
        if (s->rx == RxState::Idle) {
            if (c == 3) { if (s->state != RunState::Stopped) stop(s, "S02"); }
            else if (c == '-') { const auto frame = s->lastReply; sendBytes(s, frame); }
            else if (c == '$') {
                s->rx = RxState::Data; s->input.clear(); s->sum = 0;
                s->wireSize = 0; s->escaped = false; s->oversized = false;
            }
        } else if (s->rx == RxState::Data) {
            if (c == '$' && !s->escaped) {
                s->input.clear(); s->sum = 0; s->wireSize = 0; s->oversized = false;
            } else if (c == '#' && !s->escaped) s->rx = RxState::Checksum1;
            else {
                s->sum += uint8_t(c);
                if (++s->wireSize > packetSize) s->oversized = true;
                if (s->escaped) { if (!s->oversized) s->input += char(c ^ 0x20); s->escaped = false; }
                else if (c == '}') s->escaped = true;
                else if (!s->oversized) s->input += c;
            }
        } else if (s->rx == RxState::Checksum1) {
            if (hexDigit(c) < 0) { s->rx = RxState::Idle; sendBytes(s, "-"); }
            else { s->expected = hexDigit(c) << 4; s->rx = RxState::Checksum2; }
        } else {
            s->rx = RxState::Idle;
            if (hexDigit(c) < 0 || (s->expected | hexDigit(c)) != s->sum || s->oversized) sendBytes(s, "-");
            else if (sendBytes(s, "+")) {
                const auto packet = s->input;
                command(s, packet);
                if (block && s->state != RunState::Stopped) return;
            }
        }
    }
}
} // namespace

bool gdbStubEnabled(stub* s) { return s && s->sock >= 0; }
void gdbStubSetSoc(stub* s,SoC* soc) { if(s) s->soc=soc; }
void gdbStubDebugBreakRequested(stub* s) {
    if (gdbStubEnabled(s)) s->debugRequested = true;
}
void gdbStubInstructionExecuted(stub* s) {
    if (s && s->state == RunState::Step) s->stepDone = true;
}
void gdbStubReportPc(stub* s, uint32_t pc, bool thumb) {
    if (!s || s->listener < 0) return;
    if (s->sock < 0) {
        if ((++s->poll & 1023) == 0) acceptClient(s, false);
        if (s->sock < 0) return;
    }
    // A guest interrupt may transfer execution between the previous post-step
    // stop and the next interpreter boundary. Never step an ARM handler while
    // pretending it was a 68k instruction. The attempted step is cancelled.
    if (s->state == RunState::PaceStep && !cpuIsModePace(s->cpu)) {
        stop(s, "S05");
    } else if (s->state == RunState::PaceStep) {
        // Ctrl-C/EOF must be handled before the single attempt, not 1024 later.
        // A PACE breakpoint at this boundary is deliberately stepped over once.
        receive(s, false);
    }
    if (s->debugRequested) {
        s->debugRequested = false;
        if (s->state != RunState::Stopped) stop(s, "S05");
    } else if (s->state == RunState::Step && s->stepDone) stop(s, "S05");
    else if (s->state == RunState::Running) {
        if ((++s->poll & 1023) == 0) receive(s, false);
        if (s->sock >= 0 && s->state == RunState::Running) {
            if (breakpointHit(s, pc, thumb)) stop(s, "S05");
        }
    }
    if (s->state != RunState::Stopped) return;
    for (;;) {
        while (s->sock >= 0 && s->state == RunState::Stopped) receive(s, true);
        if (s->sock < 0) return;
        if (s->state == RunState::PaceStep) {
            receive(s, false);
            if (s->state == RunState::Stopped) continue;
            return;
        }
        if (s->state != RunState::Running) return;
        // Continue can redirect PC or change CPSR inside this very hook.
        const uint32_t currentPc = cpuIsModePace(s->cpu) ? paceGetPC() : cpuGetRegisters(s->cpu)[15];
        if (!breakpointHit(s, currentPc, cpuIsThumb(s->cpu))) return;
        stop(s, "S05");
    }
}

void gdbStubResetPace(stub* s) {
    if (!s) return;
    s->paceBreakpoints.clear();
    s->stopPaceException = s->atPaceException = false;
    s->paceStepArmed = s->pendingPaceDispatch = false;
    if (s->state == RunState::PaceStep) s->state = RunState::Running;
    s->lastPaceException.clear();
    s->lastPaceStep.clear();
}
void gdbStubPaceInstructionCompleted(stub* s, uint32_t instructionPc, unsigned status) {
    if (!gdbStubEnabled(s)) return;
    // Syscalls, returns and emulator patch traps are normal control transfers.
    const bool exception = status != pace_status_ok && status != pace_status_syscall &&
        status != pace_status_return && status != pace_status_trap0 && status != pace_status_trap8;
    const bool stepped = s->state == RunState::PaceStep;
    if (!exception && !stepped) return;
    auto snapshot = paceSnapshot(exception ? "exception" : status == pace_status_ok ? "boundary" : "dispatch");
    snapshot.pop_back();
    snapshot += ",\"instruction_pc\":" + std::to_string(instructionPc) +
                ",\"status\":" + std::to_string(status);
    if (status == pace_status_memory_fault) {
        uint32_t addr; bool write; uint_fast8_t fsr;
        paceGetMemeryFault(&addr, &write, &fsr);
        snapshot += ",\"fault_address\":" + std::to_string(addr) +
                    ",\"fault_write\":" + (write ? "true" : "false") +
                    ",\"fault_status\":" + std::to_string(fsr);
    }
    snapshot += '}';
    if (exception) s->lastPaceException = snapshot;
    if (stepped) s->lastPaceStep = snapshot;
    if (!stepped && !s->stopPaceException) return;
    // Observe the completed/partially completed instruction before dispatching
    // its exception. Continue dispatches it once; it never replays the access.
    s->atPaceException = exception;
    s->pendingPaceDispatch = status != pace_status_ok;
    stop(s, "S05", stepped);
    while (s->sock >= 0 && s->state == RunState::Stopped) receive(s, true);
    s->atPaceException = s->pendingPaceDispatch = false;
}

stub* gdbStubInit(ArmCpu* cpu, int port) {
    if (port == -1) return nullptr;
    auto* s = new (std::nothrow) stub;
    if (!s) ERR("cannot allocate GDB stub\n");
    s->cpu = cpu;
    if (port < 0 || port > 65535) ERR("GDB port must be 1..65535\n");
    s->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->listener < 0) ERR("gdb socket failed: %s\n", strerror(errno));
    const int one = 1;
    setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->listener, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) || listen(s->listener, 1))
        ERR("gdb listen failed: %s\n", strerror(errno));
    socklen_t size = sizeof(sa);
    if (getsockname(s->listener, reinterpret_cast<sockaddr*>(&sa), &size)) ERR("gdb getsockname failed\n");
    fprintf(stderr, "GDB listening on 127.0.0.1:%u; waiting for debugger\n", ntohs(sa.sin_port));
    acceptClient(s, true);
    if (fcntl(s->listener, F_SETFL, O_NONBLOCK) < 0) ERR("gdb nonblocking listener failed\n");
    return s;
}
#else
struct stub {};
void gdbStubSetSoc(stub*,SoC*) {}
stub* gdbStubInit(ArmCpu*, int) { static stub disabled; return &disabled; }
#endif
