// The UART cache flag stays set once uart() has handed out a reference, through copy and move assignment.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "imx1_bootstrap.h"
#undef private

int main() {
    Imx1Bootstrap shared;
    shared.uart(1);
    assert(shared.runCache.uartShared);
    Imx1Bootstrap fresh;
    assert(!fresh.runCache.uartShared);
    shared = fresh;
    assert(shared.runCache.uartShared);
    shared = Imx1Bootstrap();
    assert(shared.runCache.uartShared);
    fresh = shared;
    assert(fresh.runCache.uartShared);
    std::puts("PASS: uartShared is kept through board assignment");
}
