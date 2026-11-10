// A savestate's layout is probed at the first save. A machine whose chunks grow, shrink or appear
// after that (a card going into the Dana's second slot) must still save, with every chunk whole.
#include "savestate/Savestate.h"

#include <cassert>
#include <cstdio>
#include <cstring>

enum class Type : uint32_t { fixed = 1, keys = 2, extra = 3 };

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

    template <typename T>
    void Save(T& s) {
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

    assert(state.Save(machine));
    assert(keysWords(state) == 2);

    // Second card in: the keys chunk grows past the probed layout.
    machine.secondCard = true;
    assert(state.Save(machine));
    assert(keysWords(state) == 3);

    // Taken out again: the chunk shrinks, and the layout follows it.
    machine.secondCard = false;
    assert(state.Save(machine));
    assert(keysWords(state) == 2);

    // A chunk the first layout didn't have.
    machine.extra = true;
    assert(state.Save(machine));
    assert(hasChunk(state, Type::extra));

    std::puts("savestate layout: pass");
    return 0;
}
