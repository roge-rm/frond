#include <cassert>
#include <cstdint>
#include "FrameBounds.h"

int main() {
    assert(FrameBoundsValid(560, 160, 70, 0, 1, 11200, 0x3b00));
    assert(FrameBoundsValid(160, 160, 80, 0, 4, 12800, 0x1000));
    assert(FrameBoundsValid(560, 160, 142, 7, 2, 22720, 0x1000));
    assert(!FrameBoundsValid(560, 160, 140, 7, 2, 22720, 0x1000));
    assert(!FrameBoundsValid(560, 160, 70, 0, 1, 11199, 0x1000));
    // A short dirty copy can fit by itself while its destination offset is out of bounds.
    assert(!FrameBoundsValid(80, 11, 10, 0, 1, 100, 0x1000));
    assert(FrameBoundsValid(8, 1, 1, 0, 1, 1, UINT32_MAX));
    assert(!FrameBoundsValid(8, 2, 1, 0, 1, 2, UINT32_MAX));
    assert(!FrameBoundsValid(UINT32_MAX, 1, 1, UINT32_MAX, 4, SIZE_MAX, 1));
    assert(!FrameBoundsValid(1, UINT32_MAX, UINT32_MAX, 0, 1, SIZE_MAX, 1));
    assert(!FrameBoundsValid(1, 1, 0, 0, 1, 1, 0));
    assert(!FrameBoundsValid(1, 0, 1, 0, 1, 1, 0));
    assert(!FrameBoundsValid(0, 1, 1, 0, 1, 1, 0));
    assert(!FrameBoundsValid(1, 1, 1, 0, 8, 1, 0));
    assert(FrameDirtyLinesValid(160, 159, 159));
    assert(!FrameDirtyLinesValid(160, 160, 160));
    assert(!FrameDirtyLinesValid(160, 3, 2));
    assert(!FrameDirtyLinesValid(0, 0, 0));
}
