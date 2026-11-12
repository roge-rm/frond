#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "imx1_cs4_capture.h"

int main() {
    using Access = Imx1BootstrapAccess;
    Imx1Eim eim;
    Imx1Cs4Capture capture(0xff), other(0);
    auto configure = [&](uint32_t address, uint32_t value) {
        assert(eim.access(address, 4, true, value) == Imx1EimAccess::ok);
    };
    auto write = [&](uint32_t port, uint32_t value, Access expected = Access::ok) {
        assert(capture.access(eim, port, 1, true, value) == expected);
    };
    auto reject = [&](uint32_t address, uint32_t size, bool write, Access expected) {
        const int index = capture.selectedIndex(), bank = capture.observedBank();
        uint32_t value = 0xabcdef;
        assert(capture.access(eim, address, size, write, value) == expected);
        assert(value == 0xabcdef);
        assert(capture.selectedIndex() == index && capture.observedBank() == bank);
    };
    assert(capture.selectedIndex() == -1 && capture.observedBank() == -1);
    reject(0x15000000, 1, true, Access::sequence);
    configure(0x00220020, 0x3e70);
    reject(0x15000000, 1, true, Access::sequence);
    configure(0x00220024, 0xffff4300);  // Disabled CS4 cannot transport bytes.
    reject(0x15000000, 1, true, Access::sequence);
    configure(0x00220024, 0xffff4301);
    for (uint32_t size : {0u, 2u, 4u, 8u}) {
        for (uint32_t port : {0x15000000u, 0x15000001u}) {
            reject(port, size, true, Access::width);
            reject(port, size, false, Access::width);
        }
    }
    for (uint32_t hole : {0x15000002u, 0x15000100u, 0x15ffffffu, 0x16000000u}) {
        reject(hole, 1, true, Access::unmapped);
        reject(hole, 1, false, Access::unmapped);
    }
    reject(0x15000001, 1, true, Access::sequence);
    reject(0x15000001, 1, false, Access::externalInput);
    uint32_t value = 0x12345678;
    assert(capture.access(eim, 0x15000000, 1, false, value) == Access::ok && value == 0xff);
    assert(capture.selectedIndex() == -1 && capture.observedBank() == -1);
    write(0x15000000, 0x49);
    reject(0x15000001, 1, true, Access::sequence);
    write(0x15000000, 0x0f);
    write(0x15000001, 2, Access::sideEffect);
    assert(capture.observedBank() == -1);
    write(0x15000001, 1);
    assert(capture.selectedIndex() == 0x0f && capture.observedBank() == 1);
    write(0x15000001, 0xff, Access::sideEffect);
    assert(capture.observedBank() == 1);
    for (uint32_t index : {0x0du, 0x49u, 0x48u, 0u, 0xffu}) {
        write(0x15000000, index);
        for (uint32_t command : {0u, 3u, 0xffu}) write(0x15000001, command);
        reject(0x15000001, 1, false, Access::externalInput);
        assert(capture.access(eim, 0x15000000, 1, false, value) == Access::ok && value == 0xff);
        assert(capture.selectedIndex() == int(index) && capture.observedBank() == 1);
    }
    write(0x15000000, 0x0f); write(0x15000001, 0);
    assert(capture.observedBank() == 0);
    configure(0x00220020, 0x3e00);
    reject(0x15000000, 1, true, Access::sequence);
    reject(0x15000001, 1, false, Access::sequence);
    configure(0x00220020, 0x3e70);
    assert(other.selectedIndex() == -1 && other.observedBank() == -1);
    assert(other.access(eim, 0x15000000, 1, false, value) == Access::ok && value == 0);
    assert(capture.observedBank() == 0);
    Imx1Cs4IndexedInputs samples;
    assert(samples.empty() && samples.sample(-1, 14) == -1);
    assert(samples.set(1, 14, 0));
    assert(samples.set(0, 14, 3));
    assert(!samples.empty() && !samples.set(1, 14, 255));
    for (auto fields : {std::array<uint32_t,3>{2,14,0}, {1,256,0}, {1,14,256}})
        assert(!samples.set(fields[0], fields[1], fields[2]));
    assert(samples.sample(1, 14) == 0 && samples.sample(0, 14) == 3);
    Imx1Cs4Capture indexed(0xff, samples);
    auto indexedWrite = [&](uint32_t port, uint32_t byte) {
        assert(indexed.access(eim, port, 1, true, byte) == Access::ok);
    };
    auto sampleRead = [&](Access expected, uint32_t byte = 0xdeadbeef) {
        uint32_t actual = 0xdeadbeef;
        const int index = indexed.selectedIndex(), bank = indexed.observedBank();
        assert(indexed.access(eim, 0x15000001, 1, false, actual) == expected && actual == byte);
        assert(indexed.selectedIndex() == index && indexed.observedBank() == bank);
    };
    sampleRead(Access::externalInput);
    indexedWrite(0x15000000, 14);
    sampleRead(Access::externalInput);  // A bank must be selected even for supplied index samples.
    indexedWrite(0x15000000, 15); indexedWrite(0x15000001, 1);
    indexedWrite(0x15000000, 14);
    for (unsigned i = 0; i < 16; ++i) sampleRead(Access::ok, 0);
    indexedWrite(0x15000001, 255);
    sampleRead(Access::ok, 0);  // Recorded writes do not turn input samples into register storage.
    indexedWrite(0x15000000, 13); sampleRead(Access::externalInput);
    indexedWrite(0x15000000, 15); indexedWrite(0x15000001, 0);
    indexedWrite(0x15000000, 14); sampleRead(Access::ok, 3);
    auto copy = indexed;
    value = 0;
    assert(copy.access(eim, 0x15000001, 1, false, value) == Access::ok && value == 3);
    value = 0xdeadbeef;
    assert(copy.access(eim, 0x15000001, 2, false, value) == Access::width && value == 0xdeadbeef);
    configure(0x00220020, 0x3e00);
    assert(copy.access(eim, 0x15000001, 1, false, value) == Access::sequence && value == 0xdeadbeef);
    configure(0x00220020, 0x3e70);
    assert(capture.access(eim, 0x15000001, 1, false, value) == Access::externalInput);
    std::puts("PASS: diagnostic CS4 raw samples, independent latches, EIM/width/sequence guards and explicitly selected indexed input samples");
}
