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
    std::puts("PASS: diagnostic CS4 raw samples, independent latches, EIM/width/sequence guards and no data readback");
}
