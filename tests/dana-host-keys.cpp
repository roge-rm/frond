// The Dana's host keyboard chords (DanaHostKeys.h).
#include <cstdio>
#include <string>

#include "DanaHostKeys.h"

static int failures = 0;

static std::string show(const std::vector<DanaHostChange>& changes) {
    std::string out;
    for (const auto& c : changes) out += (out.empty() ? "" : " ") + std::string(c.down ? "+" : "-") + c.name + (c.source == DanaSource5::physicalRight ? "/r" : c.source == DanaSource5::physicalPage ? "/p" : c.source == DanaSource5::physicalChord ? "/c" : "");
    return out;
}

static void expect(const char* what, const std::vector<DanaHostChange>& got, const char* want) {
    if (show(got) != want) {
        std::printf("FAIL %s: got \"%s\", want \"%s\"\n", what, show(got).c_str(), want);
        ++failures;
    }
}

static SDL_Scancode S_L() { return SDL_SCANCODE_LSHIFT; }

int main() {
    const auto L = SDL_SCANCODE_LCTRL, R = SDL_SCANCODE_RCTRL;
    {
        DanaHostKeys k;
        expect("ctrl down is held back", k.Event(L, true, "ctrl"), "");
        expect("ctrl+f1", k.Event(SDL_SCANCODE_F1, true, "f1"), "+calendar/c");
        expect("ctrl up after a chord", k.Event(L, false, "ctrl"), "");
        expect("f1 up releases calendar", k.Event(SDL_SCANCODE_F1, false, "f1"), "-calendar/c");
    }
    {
        DanaHostKeys k;
        k.Event(R, true, "ctrl");
        expect("right ctrl+esc", k.Event(SDL_SCANCODE_ESCAPE, true, "escape"), "+power/c");
        expect("esc up with ctrl still down", k.Event(SDL_SCANCODE_ESCAPE, false, "escape"), "-power/c");
        expect("right ctrl up", k.Event(R, false, "ctrl"), "");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        expect("ctrl+a", k.Event(SDL_SCANCODE_A, true, "a"), "+ctrl +a");
        expect("a up", k.Event(SDL_SCANCODE_A, false, "a"), "-a");
        expect("ctrl up", k.Event(L, false, "ctrl"), "-ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(R, true, "ctrl");
        expect("right ctrl+a", k.Event(SDL_SCANCODE_A, true, "a"), "+ctrl/r +a");
        expect("right ctrl up", k.Event(R, false, "ctrl"), "-ctrl/r");
    }
    {
        DanaHostKeys k;
        expect("ctrl tap down", k.Event(L, true, "ctrl"), "");
        expect("ctrl tap up", k.Event(L, false, "ctrl"), "+ctrl -ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_F3, true, "f3");
        expect("ctrl up first", k.Event(L, false, "ctrl"), "");
        expect("f3 up", k.Event(SDL_SCANCODE_F3, false, "f3"), "-todo/c");
        expect("f3 plain", k.Event(SDL_SCANCODE_F3, true, "f3"), "+f3");
        expect("f3 plain up", k.Event(SDL_SCANCODE_F3, false, "f3"), "-f3");
    }
    {
        DanaHostKeys k;
        expect("alt passes", k.Event(SDL_SCANCODE_LALT, true, "function"), "+function");
        k.Event(L, true, "ctrl");
        expect("ctrl, alt, f1 sends no stray ctrl", k.Event(SDL_SCANCODE_F1, true, "f1"), "+calendar/c");
        expect("alt up", k.Event(SDL_SCANCODE_LALT, false, "function"), "-function");
        k.Event(SDL_SCANCODE_F1, false, "f1");
        k.Event(L, false, "ctrl");
        k.Event(L, true, "ctrl");
        expect("gui while ctrl held", k.Event(SDL_SCANCODE_LGUI, true, "command"), "+command");
        expect("ctrl up alone after gui", k.Event(SDL_SCANCODE_LGUI, false, "command"), "-command");
        expect("ctrl tap after alt/gui", k.Event(L, false, "ctrl"), "+ctrl -ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_A, true, "a");
        expect("ctrl already sent comes up first", k.Event(SDL_SCANCODE_F2, true, "f2"), "-ctrl +address/c");
        expect("ctrl up after that", k.Event(L, false, "ctrl"), "");
    }
    {
        DanaHostKeys k;
        expect("pgup", k.Event(SDL_SCANCODE_PAGEUP, true, "pageup"), "+function/p +up/p");
        expect("pgup up", k.Event(SDL_SCANCODE_PAGEUP, false, "pageup"), "-up/p -function/p");
        expect("pgdn", k.Event(SDL_SCANCODE_PAGEDOWN, true, "pagedown"), "+function/p +down/p");
        expect("pgdn up", k.Event(SDL_SCANCODE_PAGEDOWN, false, "pagedown"), "-down/p -function/p");
    }
    {
        DanaHostKeys k;
        expect("plain f1", k.Event(SDL_SCANCODE_F1, true, "f1"), "+f1");
        expect("plain esc", k.Event(SDL_SCANCODE_ESCAPE, true, "escape"), "+escape");
        expect("right alt", k.Event(SDL_SCANCODE_RALT, true, "function"), "+function/r");
        k.Event(L, true, "ctrl");
        k.Clear();
        expect("clear forgets ctrl", k.Event(L, false, "ctrl"), "");
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_F1, true, "f1");
        k.Clear();
        expect("clear forgets chords", k.Event(SDL_SCANCODE_F1, false, "f1"), "-f1");
    }
    {
        DanaHostKeys k;
        const auto S = SDL_SCANCODE_LSHIFT;
        expect("shift alone passes", k.Event(S, true, "lshift"), "+lshift");
        expect("shift+a", k.Event(SDL_SCANCODE_A, true, "a"), "+a");
        k.Event(SDL_SCANCODE_A, false, "a");
        expect("shift up", k.Event(S, false, "lshift"), "-lshift");
    }
    {
        DanaHostKeys k;
        expect("ctrl", k.Event(L, true, "ctrl"), "");
        expect("shift held back with ctrl", k.Event(S_L(), true, "lshift"), "");
        expect("ctrl+shift+f1: function and calendar only", k.Event(SDL_SCANCODE_F1, true, "f1"), "+function/c +calendar/c");
        expect("f1 up", k.Event(SDL_SCANCODE_F1, false, "f1"), "-calendar/c -function/c");
        expect("shift up", k.Event(S_L(), false, "lshift"), "");
        expect("ctrl up", k.Event(L, false, "ctrl"), "");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        k.Event(S_L(), true, "lshift");
        expect("ctrl+shift+a", k.Event(SDL_SCANCODE_A, true, "a"), "+ctrl +lshift +a");
        expect("a up", k.Event(SDL_SCANCODE_A, false, "a"), "-a");
        expect("shift up", k.Event(S_L(), false, "lshift"), "-lshift");
        expect("ctrl up", k.Event(L, false, "ctrl"), "-ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(SDL_SCANCODE_RSHIFT, true, "rshift");
        k.Event(R, true, "ctrl");
        expect("shift sent before ctrl comes up first", k.Event(SDL_SCANCODE_F8, true, "f8"), "-rshift/r +function/c +send/c");
        expect("two function chords", k.Event(SDL_SCANCODE_F7, true, "f7"), "+apps/c");
        expect("f8 up keeps function", k.Event(SDL_SCANCODE_F8, false, "f8"), "-send/c");
        expect("f7 up drops function", k.Event(SDL_SCANCODE_F7, false, "f7"), "-apps/c -function/c");
    }
    {
        DanaHostKeys k;
        expect("apps key", k.Event(SDL_SCANCODE_APPLICATION, true, "apps"), "+apps");
        k.Event(L, true, "ctrl");
        expect("ctrl+f7 uses its own source", k.Event(SDL_SCANCODE_F7, true, "f7"), "+apps/c");
        expect("f7 up leaves the apps key", k.Event(SDL_SCANCODE_F7, false, "f7"), "-apps/c");
        expect("apps up", k.Event(SDL_SCANCODE_APPLICATION, false, "apps"), "-apps");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        expect("screen input flushes ctrl", k.Flush(), "+ctrl");
        expect("only once", k.Flush(), "");
        expect("ctrl up", k.Event(L, false, "ctrl"), "-ctrl");
    }
    std::printf(failures ? "dana-host-keys: FAIL\n" : "dana-host-keys: ok\n");
    return failures ? 1 : 0;
}
