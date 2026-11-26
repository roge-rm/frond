// The Palm Portable Keyboard's serial protocol (core/src/common/serial/PortableKeyboard.h) against
// Think Outside's Hardware and Electronics Reference 1.1: HotSync pulses and their retries, the ID on
// RTS rising, make and break codes with the doubled last break, low power, the 6-key buffer, the
// typist, and the host key map frond-68k uses.

#include <cstdio>
#include <vector>

#include "../core/src/cloudpilot/frond/PortableKeyboardKeys.h"
#include "serial/PortableKeyboard.h"

using frondKeyboard::PortableKeyboard;
using frondKeyboard::PortableKeyboardTypist;
using Bytes = std::vector<uint8_t>;

static unsigned checks, failures;
#define CHECK(condition)                                                            \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(condition)) {                                                         \
            ++failures;                                                             \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                           \
    } while (0)

constexpr uint64_t ms = 1000000ull;

static Bytes Drain(PortableKeyboard& k) {
    Bytes out(k.Available());
    out.resize(k.Read(out.data(), out.size()));
    return out;
}

static void Pulses() {
    PortableKeyboard k;
    k.Attach(false, 0);
    CHECK(k.TakePulses() == 1);
    CHECK(std::string(k.Describe()) == "pulsing");
    k.Service(999 * ms);
    CHECK(k.TakePulses() == 0);
    k.Service(1000 * ms);
    CHECK(k.TakePulses() == 1);
    k.Service(2000 * ms);
    CHECK(k.TakePulses() == 1);
    // A third pulse ignored: the keyboard sleeps, and keys pulse no more.
    k.Service(3000 * ms);
    CHECK(k.TakePulses() == 0);
    CHECK(std::string(k.Describe()) == "asleep");
    CHECK(k.Key(0x11, true, 3100 * ms));
    k.Service(9000 * ms);
    CHECK(k.TakePulses() == 0);
    CHECK(k.Available() == 0);
    // RTS rising wakes it with the ID and the key still held.
    k.SetRts(true, 9100 * ms);
    CHECK(k.Awake());
    CHECK((Drain(k) == Bytes{0xfa, 0xfd, 0x11}));
    CHECK(k.Key(0x11, false, 9200 * ms));
    CHECK((Drain(k) == Bytes{0x91, 0x91}));
}

static void Codes() {
    PortableKeyboard k;
    k.Attach(false, 0);
    k.TakePulses();
    CHECK(k.Available() == 0);
    k.SetRts(true, 10 * ms);
    CHECK((Drain(k) == Bytes{0xfa, 0xfd}));
    CHECK(k.TakePulses() == 0);
    // Shift-A from the reference's example 2.
    k.Key(PortableKeyboard::kLeftShift, true, 20 * ms);
    k.Key(0x11, true, 30 * ms);
    k.Key(0x11, false, 40 * ms);
    k.Key(PortableKeyboard::kLeftShift, false, 50 * ms);
    CHECK((Drain(k) == Bytes{0x58, 0x11, 0x91, 0xd8, 0xd8}));
    // A key already down isn't sent twice, and one never down isn't let go.
    k.Key(0x00, true, 60 * ms);
    k.Key(0x00, true, 61 * ms);
    k.Key(0x01, false, 62 * ms);
    k.Key(0x00, false, 63 * ms);
    CHECK((Drain(k) == Bytes{0x00, 0x80, 0x80}));
    // Not on the matrix.
    CHECK(!k.Key(0x1b, true, 70 * ms));
    CHECK(!k.Key(0x60, true, 70 * ms));
    CHECK(PortableKeyboard::Valid(PortableKeyboard::kFn) && PortableKeyboard::Valid(PortableKeyboard::kDone));
    CHECK(!PortableKeyboard::Valid(0x24) && !PortableKeyboard::Valid(0x43) && !PortableKeyboard::Valid(0x54));
}

static void LowPower() {
    PortableKeyboard k;
    k.Attach(true, 0);
    // Powered with RTS already high: a pulse, no ID until the driver toggles RTS.
    CHECK(k.TakePulses() == 1);
    CHECK(!k.Awake());
    k.Key(0x11, true, 5 * ms);
    CHECK(k.Available() == 0);
    k.Key(0x11, false, 6 * ms);
    k.SetRts(false, 10 * ms);
    k.SetRts(true, 20 * ms);
    CHECK((Drain(k) == Bytes{0xfa, 0xfd}));
    // RTS low: a key pulses and sends nothing.
    k.SetRts(false, 30 * ms);
    CHECK(std::string(k.Describe()) == "low-power");
    k.Key(0x12, true, 40 * ms);
    CHECK(k.TakePulses() == 1);
    CHECK(k.Available() == 0);
    // A second key during the pulses doesn't start more.
    k.Key(0x13, true, 41 * ms);
    CHECK(k.TakePulses() == 0);
    k.SetRts(true, 60 * ms);
    CHECK((Drain(k) == Bytes{0xfa, 0xfd, 0x12, 0x13}));
    k.Key(0x12, false, 70 * ms);
    k.Key(0x13, false, 80 * ms);
    CHECK((Drain(k) == Bytes{0x92, 0x93, 0x93}));
    // Ten minutes with RTS high and no key: low power, keys ignored, no pulses.
    k.Service(80 * ms + PortableKeyboard::kIdleSleepNs);
    CHECK(std::string(k.Describe()) == "idle");
    k.Key(0x11, true, 81 * ms + PortableKeyboard::kIdleSleepNs);
    k.Key(0x11, false, 82 * ms + PortableKeyboard::kIdleSleepNs);
    CHECK(k.Available() == 0);
    CHECK(k.TakePulses() == 0);
    // Toggling RTS high to low to high brings it back with its ID.
    k.SetRts(false, 90 * ms + PortableKeyboard::kIdleSleepNs);
    k.SetRts(true, 91 * ms + PortableKeyboard::kIdleSleepNs);
    CHECK((Drain(k) == Bytes{0xfa, 0xfd}));
    CHECK(k.Awake());
    k.Detach();
    CHECK(!k.Attached());
    CHECK(!k.Key(0x11, true, 0));
}

static void Buffer() {
    PortableKeyboard k;
    k.Attach(false, 0);
    k.SetRts(true, ms);
    Drain(k);
    const uint8_t keys[] = {0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x07};
    for (int i = 0; i < 6; ++i) CHECK(k.Key(keys[i], true, 2 * ms));
    CHECK(!k.Key(keys[6], true, 2 * ms));
    CHECK(Drain(k).size() == 6);
    CHECK(k.Key(keys[6], true, 3 * ms));
}

static void Characters() {
    uint8_t code = 0;
    bool shift = true;
    CHECK(PortableKeyboard::CharKey('a', code, shift) && code == 0x11 && !shift);
    CHECK(PortableKeyboard::CharKey('A', code, shift) && code == 0x11 && shift);
    CHECK(PortableKeyboard::CharKey('1', code, shift) && code == 0x00 && !shift);
    CHECK(PortableKeyboard::CharKey(')', code, shift) && code == 0x36 && shift);
    CHECK(PortableKeyboard::CharKey('\n', code, shift) && code == PortableKeyboard::kEnter && !shift);
    CHECK(PortableKeyboard::CharKey(' ', code, shift) && code == PortableKeyboard::kSpace && !shift);
    CHECK(PortableKeyboard::CharKey('"', code, shift) && code == 0x40 && shift);
    CHECK(PortableKeyboard::CharKey('?', code, shift) && code == 0x48 && shift);
    CHECK(!PortableKeyboard::CharKey('\x01', code, shift));
    CHECK(!PortableKeyboard::CharKey(char(0xe9), code, shift));
}

static void Typist() {
    PortableKeyboard k;
    PortableKeyboardTypist t;
    k.Attach(false, 0);
    k.TakePulses();
    CHECK(!t.Type("a\x01"));
    CHECK(!t.Busy());
    // From low power the first key is held until the driver raises RTS.
    CHECK(t.Type("Hi"));
    uint64_t now = 0;
    t.Service(k, now);
    CHECK(k.TakePulses() == 0);  // Shift went first; the pulses had already started.
    for (int i = 0; i < 4; ++i) t.Service(k, now += 50 * ms);
    CHECK(k.Available() == 0);
    k.SetRts(true, now += 10 * ms);
    for (int i = 0; i < 20; ++i) t.Service(k, now += 50 * ms);
    CHECK(!t.Busy());
    CHECK((Drain(k) == Bytes{0xfa, 0xfd, 0x58, 0x16, 0x96, 0xd8, 0xd8, 0x3d, 0xbd, 0xbd}));
    // Ten keys a second: four steps 50 ms apart for one character with Shift.
    CHECK(t.Type("Z"));
    const uint64_t start = now;
    while (t.Busy()) t.Service(k, now += ms);
    CHECK(now - start == 151 * ms);
}

static void HostKeys() {
    CHECK(portableKeyboardKey(SDL_SCANCODE_A).code == 0x11);
    CHECK(portableKeyboardKey(SDL_SCANCODE_1).code == 0x00 && portableKeyboardKey(SDL_SCANCODE_1));
    CHECK(portableKeyboardKey(SDL_SCANCODE_LALT).code == PortableKeyboard::kFn);
    CHECK(portableKeyboardKey(SDL_SCANCODE_RALT).code == PortableKeyboard::kAlt);
    CHECK(portableKeyboardKey(SDL_SCANCODE_LGUI).code == PortableKeyboard::kCmd);
    const auto up = portableKeyboardKey(SDL_SCANCODE_PAGEUP);
    CHECK(up.code == PortableKeyboard::kUp && up.fn);
    CHECK(portableKeyboardKey(SDL_SCANCODE_F9).code == PortableKeyboard::kDone);
    for (auto kept : {SDL_SCANCODE_F1, SDL_SCANCODE_F4, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F11, SDL_SCANCODE_F12})
        CHECK(!portableKeyboardKey(kept));
    // Every key the map gives is on the matrix, and the matrix keys a host can reach are all there.
    int mapped = 0;
    for (int scan = 0; scan < SDL_NUM_SCANCODES; ++scan) {
        const auto key = portableKeyboardKey(SDL_Scancode(scan));
        if (!key) continue;
        CHECK(PortableKeyboard::Valid(uint8_t(key.code)));
        ++mapped;
    }
    CHECK(mapped >= 70);
}

int main() {
    Pulses();
    Codes();
    LowPower();
    Buffer();
    Characters();
    Typist();
    HostKeys();
    printf("portable-keyboard: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
