#include "HayesModem.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace frondModem {

namespace {

constexpr uint64_t second = 1000000000ull;
constexpr uint64_t millisecond = 1000000ull;
// The longest command line taken; a longer one gives ERROR.
constexpr size_t commandLimit = 255;
// Bytes held for the line while the guest sends faster than the line speed; more are lost.
constexpr size_t toLineLimit = 16384;
// Bytes from the line held for the guest; the line waits while this is full.
constexpr size_t toGuestLimit = 256;
// The most a direction may save up between services, in bytes.
constexpr double paceBurst = 128;

const char* ResultText(HayesModem::Result result) {
    switch (result) {
        case HayesModem::Result::Ok: return "OK";
        case HayesModem::Result::Connect: return "CONNECT";
        case HayesModem::Result::Ring: return "RING";
        case HayesModem::Result::NoCarrier: return "NO CARRIER";
        case HayesModem::Result::Error: return "ERROR";
        case HayesModem::Result::NoDialtone: return "NO DIALTONE";
        case HayesModem::Result::Busy: return "BUSY";
        case HayesModem::Result::NoAnswer: return "NO ANSWER";
    }
    return "ERROR";
}

// Reads a decimal argument at i, if there is one.
unsigned Number(const std::string& text, size_t& i, bool* found = nullptr) {
    unsigned value = 0;
    bool any = false;
    while (i < text.size() && isdigit(uint8_t(text[i]))) {
        value = std::min(value * 10 + unsigned(text[i++] - '0'), 100000u);
        any = true;
    }
    if (found) *found = any;
    return value;
}

}  // namespace

HayesModem::HayesModem(ModemLine& line) : HayesModem(line, Options{}) {}

HayesModem::HayesModem(ModemLine& line, Options options) : line(line), options(std::move(options)) {
    if (this->options.lineSpeed < 300) this->options.lineSpeed = 300;
    Reset();
}

// Factory settings, as after AT&F or ATZ.
void HayesModem::Reset() {
    s.fill(0);
    s[2] = '+';
    s[3] = '\r';
    s[4] = '\n';
    s[5] = 8;
    s[6] = 2;
    s[7] = 50;
    s[8] = 2;
    s[9] = 6;
    s[10] = 14;
    s[11] = 95;
    s[12] = 50;
    echo = true;
    quiet = false;
    verbose = true;
    x = 4;
    dtrMode = 2;
    pulse = false;
    selected = 0;
}

void HayesModem::SetPower(bool on, uint64_t now) {
    if (on == (state != State::Off)) return;
    if (state == State::Online || state == State::OnlineCommand) line.HangUp();
    Reset();
    state = on ? State::Command : State::Off;
    holding = false;
    command.clear();
    lastCommand.clear();
    toGuest.clear();
    toLine.clear();
    escapes = 0;
    previous = 0;
    paceMark = now;
}

void HayesModem::SetDTR(bool on, uint64_t now) {
    const bool dropped = dtr && !on;
    dtr = on;
    if (!dropped || state == State::Off) return;
    switch (dtrMode) {
        case 1:
            if (state == State::Online) {
                state = State::OnlineCommand;
                escapes = 0;
                Respond(Result::Ok);
            }
            break;
        case 2:
            if (state != State::Command || holding) EndCall(true);
            break;
        case 3:
            EndCall(false);
            Reset();
            break;
        default: break;
    }
}

void HayesModem::Write(const uint8_t* data, size_t size, uint64_t now) {
    if (state == State::Off) return;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t byte = data[i];
        switch (state) {
            case State::Dialling:
                // Any key while dialling gives up the call.
                EndCall(true);
                break;
            case State::Online: OnlineByte(byte, now); break;
            default: CommandByte(byte, now); break;
        }
    }
}

size_t HayesModem::Read(uint8_t* data, size_t max) {
    const size_t n = std::min(max, toGuest.size());
    std::copy(toGuest.begin(), toGuest.begin() + long(n), data);
    toGuest.erase(toGuest.begin(), toGuest.begin() + long(n));
    return n;
}

void HayesModem::Drop(uint64_t now) {
    if (state == State::Off) return;
    command.clear();
    toGuest.clear();
    if (state != State::Command || holding) EndCall(true);
    paceMark = now;
}

std::string HayesModem::SavedCall() const {
    if (!OffHook()) return "";
    char text[64];
    snprintf(text, sizeof text, "e%d q%d v%d x%u d%u p%d s", int(echo), int(quiet), int(verbose), x, dtrMode,
             int(pulse));
    std::string saved = text;
    for (const uint8_t value : s) {
        snprintf(text, sizeof text, "%02x", value);
        saved += text;
    }
    return saved;
}

bool HayesModem::LineDropped(const std::string& saved, uint64_t now) {
    int e = 0, q = 0, v = 0, p = 0, used = 0;
    unsigned xs = 0, d = 0;
    if (sscanf(saved.c_str(), "e%d q%d v%d x%u d%u p%d s%n", &e, &q, &v, &xs, &d, &p, &used) != 6 ||
        saved.size() != size_t(used) + 2 * s.size() || e > 1 || q > 1 || v > 1 || xs > 4 || d > 3 || p > 1 ||
        e < 0 || q < 0 || v < 0 || p < 0)
        return false;
    auto nibble = [](char c) { return isdigit(uint8_t(c)) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::array<uint8_t, 256> registers{};
    for (size_t i = 0; i < registers.size(); ++i) {
        const int high = nibble(saved[size_t(used) + 2 * i]), low = nibble(saved[size_t(used) + 2 * i + 1]);
        if (high < 0 || low < 0) return false;
        registers[i] = uint8_t(high << 4 | low);
    }
    // This modem's own call, if any, isn't the guest's.
    if (state == State::Online || state == State::OnlineCommand) line.HangUp();
    s = registers;
    echo = e;
    quiet = q;
    verbose = v;
    x = xs;
    dtrMode = d;
    pulse = p;
    state = State::Command;
    holding = false;
    command.clear();
    toGuest.clear();
    toLine.clear();
    escapes = 0;
    previous = 0;
    paceMark = now;
    Respond(Result::NoCarrier);
    return true;
}

void HayesModem::Service(uint64_t now) {
    switch (state) {
        case State::Dialling:
            if (now < dialEnd) break;
            if (dialStay) {
                state = State::Command;
                holding = true;
                Respond(Result::Ok);
            } else if (dialResult == Result::Connect) {
                Connect(now);
            } else {
                state = State::Command;
                holding = false;
                Respond(dialResult);
            }
            break;
        case State::Online:
        case State::OnlineCommand:
            line.Service(now);
            if (!line.Carrier(now)) {
                EndCall(true);
                break;
            }
            // Three escape characters followed by the guard time give command mode.
            if (state == State::Online && escapes == 3 && now - escapeAt >= Guard()) {
                escapes = 0;
                state = State::OnlineCommand;
                Respond(Result::Ok);
            }
            Pace(now);
            break;
        default: break;
    }
}

uint64_t HayesModem::Guard() const { return uint64_t(s[12]) * 20 * millisecond; }

// Moves data each way no faster than the line speed (10 bits a byte).
void HayesModem::Pace(uint64_t now) {
    if (now > paceMark) {
        const double bytes = double(now - paceMark) * options.lineSpeed / 10.0 / double(second);
        toGuestCredit = std::min(toGuestCredit + bytes, paceBurst);
        toLineCredit = std::min(toLineCredit + bytes, paceBurst);
    }
    paceMark = now;
    if (state != State::Online) return;
    size_t n = std::min(toLine.size(), size_t(toLineCredit));
    if (n) {
        std::vector<uint8_t> out(toLine.begin(), toLine.begin() + long(n));
        toLine.erase(toLine.begin(), toLine.begin() + long(n));
        toLineCredit -= double(n);
        line.FromModem(out.data(), out.size(), now);
    }
    n = std::min(size_t(toGuestCredit), toGuestLimit - std::min(toGuestLimit, toGuest.size()));
    if (n) {
        uint8_t in[size_t(paceBurst)];
        const size_t got = line.ToModem(in, std::min(n, sizeof(in)), now);
        toGuest.insert(toGuest.end(), in, in + got);
        toGuestCredit -= double(got);
    }
}

void HayesModem::OnlineByte(uint8_t byte, uint64_t now) {
    const uint64_t gap = now - lastByte;
    lastByte = now;
    if (byte == s[2] && s[2] < 128 && s[12] > 0) {
        // The first needs the guard time before it, the others less than it between them.
        if (escapes == 0 || escapes == 3 || gap >= Guard())
            escapes = gap >= Guard() ? 1 : 0;
        else
            ++escapes;
        if (escapes == 3) escapeAt = now;
    } else {
        escapes = 0;
    }
    // Escape characters are data too until the guard time after them has passed.
    if (toLine.size() < toLineLimit) toLine.push_back(byte);
}

void HayesModem::CommandByte(uint8_t byte, uint64_t now) {
    if (echo) toGuest.push_back(byte);
    const uint8_t last = previous;
    previous = byte;
    if ((last == 'A' || last == 'a') && byte == '/' && command.size() == 1) {
        command.clear();
        Execute(lastCommand, now);
        return;
    }
    if (byte == s[3]) {
        if (command.size() >= 2) {
            const std::string body = command.substr(2);
            const bool tooLong = command.size() > commandLimit;
            command.clear();
            if (tooLong) {
                Respond(Result::Error);
            } else {
                lastCommand = body;
                Execute(body, now);
            }
        } else {
            command.clear();
        }
        return;
    }
    if (byte == s[5]) {
        if (!command.empty()) command.pop_back();
        return;
    }
    if (byte < 32 || byte >= 127) return;
    const char c = char(toupper(byte));
    if (command.empty()) {
        if (c == 'A') command = "A";
    } else if (command.size() == 1) {
        command = c == 'T' ? "AT" : c == 'A' ? "A" : "";
    } else if (command.size() <= commandLimit) {
        command.push_back(char(byte));
    }
}

void HayesModem::Execute(const std::string& body, uint64_t now) {
    size_t i = 0;
    while (i < body.size()) {
        const char c = char(toupper(uint8_t(body[i++])));
        bool found = false;
        switch (c) {
            case ' ': break;
            case 'D':
                Dial(body.substr(i), now);
                return;
            case 'A':
                // There's never an incoming call to answer.
                Respond(Result::NoCarrier);
                return;
            case 'E': echo = Number(body, i) != 0; break;
            case 'Q': quiet = Number(body, i) != 0; break;
            case 'V': verbose = Number(body, i) != 0; break;
            case 'X': {
                const unsigned n = Number(body, i);
                if (n > 4) {
                    Respond(Result::Error);
                    return;
                }
                x = n;
                break;
            }
            case 'P': pulse = true; break;
            case 'T': pulse = false; break;
            case 'H':
                if (Number(body, i) == 0 && (state == State::OnlineCommand || holding)) EndCall(false);
                break;
            case 'O':
                Number(body, i);
                if (state != State::OnlineCommand) {
                    Respond(Result::NoCarrier);
                    return;
                }
                state = State::Online;
                escapes = 0;
                lastByte = now;
                Respond(Result::Connect);
                return;
            case 'Z':
                EndCall(false);
                Reset();
                Respond(Result::Ok);
                return;
            case 'I': {
                const unsigned n = Number(body, i);
                Info(n == 0 ? std::to_string(options.lineSpeed) : "Frond data modem");
                break;
            }
            case 'S': {
                const unsigned n = Number(body, i, &found);
                if (!found || n > 255) {
                    Respond(Result::Error);
                    return;
                }
                selected = n;
                break;
            }
            case '=': {
                const unsigned value = Number(body, i);
                if (value > 255) {
                    Respond(Result::Error);
                    return;
                }
                s[selected] = uint8_t(value);
                break;
            }
            case '?': {
                char text[8];
                snprintf(text, sizeof(text), "%03u", unsigned(s[selected]));
                Info(text);
                break;
            }
            case '&': {
                const char what = i < body.size() ? char(toupper(uint8_t(body[i++]))) : 0;
                const unsigned n = Number(body, i);
                if (what == 'D') {
                    if (n > 3) {
                        Respond(Result::Error);
                        return;
                    }
                    dtrMode = n;
                } else if (what == 'F') {
                    Reset();
                } else if (!isalpha(uint8_t(what))) {
                    Respond(Result::Error);
                    return;
                }
                break;
            }
            case '+':
                // Extended commands run to the next ';'.
                while (i < body.size() && body[i] != ';') ++i;
                if (i < body.size()) ++i;
                break;
            case '*':
            case '%':
            case '\\':
            case '#':
            case '-':
            case '$':
                // Vendor commands such as *NC22: a name and a number, taken and ignored.
                while (i < body.size() && isalpha(uint8_t(body[i]))) ++i;
                Number(body, i);
                break;
            default:
                if (!isalpha(uint8_t(c))) {
                    Respond(Result::Error);
                    return;
                }
                // Other letters (B, C, L, M, N, W and the rest) are taken and ignored.
                Number(body, i);
                break;
        }
    }
    Respond(Result::Ok);
}

void HayesModem::Dial(const std::string& text, uint64_t now) {
    if (state == State::OnlineCommand) {
        Respond(Result::Error);
        return;
    }
    std::string number = text;
    size_t start = 0;
    while (start < number.size() && number[start] == ' ') ++start;
    if (start < number.size() && toupper(uint8_t(number[start])) == 'L') number = lastNumber;
    else lastNumber = number;

    bool usePulse = pulse;
    std::string digits;
    uint64_t length = uint64_t(s[6]) * second;
    dialStay = false;
    for (const char raw : number) {
        const char c = char(toupper(uint8_t(raw)));
        if (isdigit(uint8_t(c)) || c == '*' || c == '#' || (c >= 'A' && c <= 'D')) {
            digits.push_back(c);
            const unsigned pulses = c == '0' ? 10 : isdigit(uint8_t(c)) ? unsigned(c - '0') : 1;
            length += usePulse ? pulses * 100 * millisecond + 700 * millisecond : 100 * millisecond;
        } else if (c == 'P') {
            usePulse = true;
        } else if (c == 'T') {
            usePulse = false;
        } else if (c == ',') {
            length += uint64_t(s[8]) * second;
        } else if (c == 'W' || c == '@') {
            length += second;
        } else if (c == '!') {
            length += 500 * millisecond;
        } else if (c == ';') {
            dialStay = true;
            break;
        }
    }

    const auto planned = options.dialPlan.find(digits);
    Result result = planned != options.dialPlan.end() ? planned->second : Result::Connect;
    if (!line.Route(digits) && result == Result::Connect) result = Result::NoAnswer;
    if (digits.empty() && !dialStay) result = Result::NoCarrier;
    // X0 and X1 dial blind and can't tell busy from no answer; X2 and up hear the dial tone,
    // X3 and up the busy tone.
    if (result == Result::NoDialtone && (x == 0 || x == 1 || x == 3)) result = Result::NoCarrier;
    if (result == Result::Busy && x < 3) result = Result::NoCarrier;
    if (result == Result::NoAnswer && x == 0) result = Result::NoCarrier;

    const uint64_t wait = uint64_t(s[7]) * second;
    if (dialStay) {
        dialEnd = now + length;
    } else if (result == Result::NoDialtone) {
        dialEnd = now + std::max<uint64_t>(s[6], 2) * second;
    } else if (result == Result::Busy) {
        dialEnd = now + length + 2 * second;
    } else if (result == Result::Connect) {
        dialEnd = now + length + options.handshakeNs;
        if (dialEnd > now + wait) {
            dialEnd = now + wait;
            result = Result::NoCarrier;
        }
    } else {
        dialEnd = now + std::max(wait, length);
    }
    dialResult = result;
    holding = false;
    state = State::Dialling;
}

void HayesModem::Connect(uint64_t now) {
    state = State::Online;
    holding = false;
    toLine.clear();
    toGuestCredit = toLineCredit = 0;
    paceMark = now;
    lastByte = now;
    escapes = 0;
    line.Answer(now);
    Respond(Result::Connect);
}

// Back to command mode on hook, with NO CARRIER if report is set.
void HayesModem::EndCall(bool report) {
    if (state == State::Online || state == State::OnlineCommand) line.HangUp();
    if (state != State::Off) state = State::Command;
    holding = false;
    toLine.clear();
    escapes = 0;
    if (report) Respond(Result::NoCarrier);
}

void HayesModem::Respond(Result result) {
    if (quiet || state == State::Off) return;
    if (verbose) {
        std::string text = ResultText(result);
        if (result == Result::Connect && x > 0) text += " " + std::to_string(options.lineSpeed);
        Text(std::string() + char(s[3]) + char(s[4]) + text + char(s[3]) + char(s[4]));
    } else {
        Text(std::to_string(int(result)) + char(s[3]));
    }
}

// Information text such as ATI and ATSn? answers; Q1 doesn't hide it.
void HayesModem::Info(const std::string& text) {
    if (verbose)
        Text(std::string() + char(s[3]) + char(s[4]) + text + char(s[3]) + char(s[4]));
    else
        Text(text + char(s[3]) + char(s[4]));
}

void HayesModem::Text(const std::string& text) { toGuest.insert(toGuest.end(), text.begin(), text.end()); }

}  // namespace frondModem
