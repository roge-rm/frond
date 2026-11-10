// A savestate's layout is probed at the first save. A machine whose chunks grow, shrink or appear
// after that (a card going into the Dana's second slot) must still save, with every chunk whole.
#include "savestate/Savestate.h"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

static void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::fprintf(stderr, "savestate layout: FAIL line %d: %s\n", line, what);
    std::exit(1);
}
#define CHECK(x) check((x), #x, __LINE__)

enum class Type : uint32_t { fixed = 1, keys = 2, extra = 3, pad = 4 };

namespace std {
template <>
struct hash<Type> {
    size_t operator()(Type t) const { return hash<uint32_t>()(static_cast<uint32_t>(t)); }
};
}  // namespace std

// Like ExternalStorage: a second key only while a second card is in.
struct Machine {
    bool secondCard{false};
    bool extra{false};
    bool padOnProbe{false};  // like the patchMgr chunk: the probe writes more than the real save
    int realSaves{0};

    template <typename T>
    void Save(T& s) {
        constexpr bool probing = std::is_same_v<T, SavestateProbe<Type>>;
        if (!probing) ++realSaves;
        if (padOnProbe)
            if (auto* c = s.GetChunk(Type::pad)) {
                c->Put32(5);
                if (probing) c->Put32(6);
            }
        if (auto* c = s.GetChunk(Type::fixed)) c->Put32(7);
        if (auto* c = s.GetChunk(Type::keys)) {
            c->Put32(secondCard ? 2 : 1);
            c->Put32(11);
            if (secondCard) c->Put32(22);
        }
        if (extra)
            if (auto* c = s.GetChunk(Type::extra)) c->Put32(33);
    }
};

static unsigned keysWords(Savestate<Type>& state) {
    const auto* words = static_cast<const uint32_t*>(state.GetBuffer());
    const uint32_t count = words[0];
    for (uint32_t i = 0; i < count; ++i)
        if (words[1 + 2 * i] == static_cast<uint32_t>(Type::keys)) return words[2 + 2 * i];
    return 0;
}

static bool hasChunk(Savestate<Type>& state, Type type) {
    const auto* words = static_cast<const uint32_t*>(state.GetBuffer());
    for (uint32_t i = 0; i < words[0]; ++i)
        if (words[1 + 2 * i] == static_cast<uint32_t>(type)) return true;
    return false;
}

int main() {
    Savestate<Type> state;
    Machine machine;

    CHECK(state.Save(machine));
    CHECK(keysWords(state) == 2);

    // Second card in: the keys chunk grows past the probed layout.
    machine.secondCard = true;
    CHECK(state.Save(machine));
    CHECK(keysWords(state) == 3);

    // Taken out again: the chunk comes out short, which loads fine, so the layout stays.
    machine.secondCard = false;
    const int before = machine.realSaves;
    CHECK(state.Save(machine));
    CHECK(machine.realSaves == before + 1);
    CHECK(keysWords(state) == 3);

    // A chunk the first layout didn't have.
    machine.extra = true;
    CHECK(state.Save(machine));
    CHECK(hasChunk(state, Type::extra));

    // A chunk written short in the real save (the probe wrote more) is not a layout change.
    Machine padded;
    padded.padOnProbe = true;
    Savestate<Type> paddedState;
    CHECK(paddedState.Save(padded));
    CHECK(padded.realSaves == 1);
    CHECK(paddedState.Save(padded));
    CHECK(padded.realSaves == 2);

    std::puts("savestate layout: pass");
    return 0;
}
