// The stick as the guest reads it for each orientation the OS can turn the screen to.
#include <cstdio>
#include <cstdlib>
#include "zodiac_panel.h"

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "zodiac stick view: %s\n", what); std::exit(1); }
}

int main() {
    struct Row { int turns; float inX, inY, outX, outY; };
    // Pushed up (0, -1), right (1, 0), down (0, 1) and left (-1, 0) as the player sees the view.
    const Row rows[] = {
        {0, 0, -1, 0, -1}, {0, 1, 0, 1, 0}, {0, 0, 1, 0, 1}, {0, -1, 0, -1, 0},
        {1, 0, -1, 1, 0}, {1, 1, 0, 0, 1}, {1, 0, 1, -1, 0}, {1, -1, 0, 0, -1},
        {2, 0, -1, 0, 1}, {2, 1, 0, -1, 0}, {2, 0, 1, 0, -1}, {2, -1, 0, 1, 0},
        {3, 0, -1, -1, 0}, {3, 1, 0, 0, -1}, {3, 0, 1, 1, 0}, {3, -1, 0, 0, 1},
    };
    for (const auto& row : rows) {
        float x = 9, y = 9;
        zodiacStickForView(row.inX, row.inY, row.turns, x, y);
        check(x == row.outX && y == row.outY, "direction not turned to the guest's frame");
    }
    // A diagonal and a half push keep their size, and the same call can overwrite its own input.
    float x = 0.5f, y = -0.25f;
    zodiacStickForView(x, y, 1, x, y);
    check(x == 0.25f && y == 0.5f, "in place");
    std::puts("zodiac stick view: ok");
}
