// Native database call budget: a call gets 10 slices plus 16 per MiB it moved itself. A later call in
// the same session that moves nothing (a hang after an export-all's big database) still stops at 10,
// the session's limits keep what earlier calls were given, and both stop growing at 1000.
#include <cassert>
#include <cstdio>
#include "native_budget.h"

int main() {
    NativeBudget b;
    b.beginCall();
    assert(b.sliceLimit() == 10 && b.sessionExtra() == 0);
    b.add(10u << 20);  // a 10 MiB export
    assert(b.callExtra() == 160 && b.sliceLimit() == 170 && b.sessionExtra() == 160);
    b.endCall();
    b.beginCall();  // the next database moves nothing
    assert(b.callExtra() == 0 && b.sliceLimit() == 10 && b.sessionExtra() == 160);
    b.endCall();
    b.beginCall();
    b.add(1u << 20);
    assert(b.sliceLimit() == 26 && b.sessionExtra() == 176);
    b.endCall();
    b.beginCall();
    b.add(uint64_t(1) << 40);
    assert(b.sliceLimit() == 1010 && b.sessionExtra() == 1000);
    b.endCall();
    b.beginCall();
    assert(b.sliceLimit() == 10 && b.sessionExtra() == 1000);
    std::puts("PASS: each native call's slices follow its own data; a later call that moves nothing stops at 10");
}
